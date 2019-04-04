#include <stdlib.h>
#include <unistd.h>
#include <mpi.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include <dash/dart/base/mutex.h>
#include <dash/dart/base/logging.h>
#include <dash/dart/base/assert.h>
#include <dash/dart/base/env.h>
#include <dash/dart/if/dart_active_messages.h>
#include <dash/dart/if/dart_communication.h>
#include <dash/dart/if/dart_globmem.h>
#include <dash/dart/mpi/dart_team_private.h>
#include <dash/dart/mpi/dart_globmem_priv.h>
#include <dash/dart/mpi/dart_active_messages_priv.h>

/**
 * Name of the environment variable controlling whether sends are performed
 * directly (true) or using MPI_Isend (false)
 *
 * Type: boolean
 */
#define DART_AMSGQ_SENDRECV_DIRECT_ENVSTR "DART_AMSGQ_SENDRECV_DIRECT"

/**
 * Name of the environment variable controlling whether using synchronous
 * (default, true) or regular send operations (false). Setting this to false
 * may cause processing of messages to be delayed beyond the current round,
 * which may impact the correctness of the upper software layers.
 *
 * Use this only for (performance) benchmarks!
 *
 * Type: boolean
 */
#define DART_AMSGQ_SENDRECV_SYNC_ENVSTR "DART_AMSGQ_SENDRECV_SYNC"

// 100*512B (512KB) receives posted by default
//#define DEFAULT_MSG_SIZE 256
//#define NUM_MSG 100

static int amsgq_mpi_tag = 10001;

struct dart_amsgq_impl_data {
  MPI_Request *recv_reqs;
  char *      *recv_bufs;
  MPI_Request *send_reqs;
  char *      *send_bufs;
  int         *recv_outidx;
  MPI_Status  *recv_status;
  int         *send_outidx;
  int64_t     *send_count;
  int64_t     *recv_count;
  int64_t     *send_round_count;
  int64_t     *recv_round_count;
  int          send_tailpos;
  size_t       msg_size;
  int          msg_count;
  MPI_Comm     comm;
  dart_mutex_t send_mutex;
  dart_mutex_t processing_mutex;
  int          comm_rank;
  int          comm_size;
  int          tag;
  bool         direct_send;
  bool         sync_send;
};

static dart_ret_t
amsgq_test_sendreqs_unsafe(struct dart_amsgq_impl_data* amsgq)
{
  int outcount;
  MPI_Testsome(
    amsgq->send_tailpos, amsgq->send_reqs, &outcount,
    amsgq->send_outidx, MPI_STATUSES_IGNORE);
  DART_LOG_TRACE("  MPI_Testsome: send_tailpos %d, outcount %d",
                 amsgq->send_tailpos, outcount);
  if (outcount > 0) {
    if (outcount == amsgq->send_tailpos) {
      // all messages have finished --> nothing to be done
      amsgq->send_tailpos = 0;
      DART_LOG_TRACE("  All send messages finished");
    } else {
#if 0
      printf("  Finished requests: ");
      for (int outidx = 0; outidx < outcount; ++outidx) {
        printf("%d ", amsgq->send_outidx[outidx]);
      }
      printf("\n");
#endif
      // move requests from the back to the free spots
      int back_pos = amsgq->msg_count - 1;
      for (int outidx = 0; outidx < outcount; ++outidx) {
        int done_idx = amsgq->send_outidx[outidx];
        DART_LOG_TRACE("  outidx %d -> done_idx %d", outidx, done_idx);
        while (back_pos > done_idx &&
                amsgq->send_reqs[back_pos] == MPI_REQUEST_NULL) back_pos--;
        if (done_idx >= back_pos) {
          // we met in the middle
          break;
        }

        DART_LOG_TRACE("  Moving back_pos %d to done_idx %d", back_pos, done_idx);
        // copy the request and buffer to the front
        amsgq->send_reqs[done_idx]  = amsgq->send_reqs[back_pos];
        amsgq->send_reqs[back_pos] = MPI_REQUEST_NULL;
        char *tmp = amsgq->send_bufs[done_idx];
        amsgq->send_bufs[done_idx]  = amsgq->send_bufs[back_pos];
        amsgq->send_bufs[back_pos] = tmp;
        back_pos--;
      }
      amsgq->send_tailpos -= outcount;
    }
    DART_LOG_TRACE("  send_tailpos: %d", amsgq->send_tailpos);
  } else {
    // come back later
    return DART_ERR_AGAIN;
  }
  return DART_OK;
}

static
dart_ret_t
dart_amsg_sendrecv_openq(
  size_t         msg_size,
  size_t         msg_count,
  dart_team_t    team,
  struct dart_amsgq_impl_data** queue)
{
  *queue = NULL;
  bool sync_send   = dart__base__env__bool(DART_AMSGQ_SENDRECV_SYNC_ENVSTR,
                                           true);
  bool direct_send = dart__base__env__bool(DART_AMSGQ_SENDRECV_DIRECT_ENVSTR,
                                           sync_send ? false : true);

  dart_team_data_t *team_data = dart_adapt_teamlist_get(team);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_gptr_getaddr ! Unknown team %i", team);
    return DART_ERR_INVAL;
  }
  struct dart_amsgq_impl_data* res = calloc(1, sizeof(*res));
  MPI_Comm_dup(team_data->comm, &res->comm);

  // signal MPI that we don't care about the order of messages
  /**
   * NOTE: allow_overtake may not be used for regular sends as it may distort
   *       our accounting. For synchronous sends, allow_overtake can be used
   *       but is broken on Open MPI <= 4.0.1.
   */
#if 0
  MPI_Info info;
  MPI_Info_create(&info);
  MPI_Info_set(info, "mpi_assert_allow_overtaking", "true");
  MPI_Comm_set_info(res->comm, info);
  MPI_Info_free(&info);
#endif

  dart__base__mutex_init(&res->send_mutex);
  dart__base__mutex_init(&res->processing_mutex);
  res->msg_size    = msg_size;
  res->msg_count   = msg_count;
  res->direct_send = direct_send;
  res->sync_send   = sync_send;
  if (!direct_send) {
    res->send_bufs   = malloc(msg_count*sizeof(*res->send_bufs));
    res->send_reqs   = malloc(msg_count*sizeof(*res->send_reqs));
    res->send_outidx = malloc(msg_count*sizeof(*res->send_outidx));
  }
  res->recv_bufs   = malloc(msg_count*sizeof(*res->recv_bufs));
  res->recv_reqs   = malloc(msg_count*sizeof(*res->recv_reqs));
  res->recv_outidx = malloc(msg_count*sizeof(*res->recv_outidx));
  res->recv_status = malloc(msg_count*sizeof(*res->recv_status));
  MPI_Comm_rank(res->comm, &res->comm_rank);
  MPI_Comm_size(res->comm, &res->comm_size);

  res->tag = amsgq_mpi_tag++;

  // post receives
  for (int i = 0; i < msg_count; ++i) {
    res->recv_bufs[i] = malloc(res->msg_size);
    MPI_Recv_init(
      res->recv_bufs[i],
      res->msg_size,
      MPI_BYTE,
      MPI_ANY_SOURCE,
      res->tag,
      res->comm,
      &res->recv_reqs[i]);
    if (!direct_send) {
      res->send_bufs[i] = malloc(res->msg_size);
      res->send_reqs[i] = MPI_REQUEST_NULL;
    }
  }

  if (!res->sync_send) {
    res->send_count       = calloc(res->comm_size, sizeof(int64_t));
    res->send_round_count = calloc(res->comm_size, sizeof(int64_t));
    res->recv_count       = calloc(res->comm_size, sizeof(int64_t));
    res->recv_round_count = calloc(res->comm_size, sizeof(int64_t));
  }

  MPI_Startall(msg_count, res->recv_reqs);

  MPI_Barrier(res->comm);

  *queue = res;

  return DART_OK;
}

static
dart_ret_t
dart_amsg_sendrecv_trysend(
  dart_team_unit_t              target,
  struct dart_amsgq_impl_data * amsgq,
  const void                  * data,
  size_t                        data_size)
{
  int ret;

  DART_ASSERT(amsgq->send_tailpos <= amsgq->msg_count);

  if (!amsgq->direct_send) {
    dart__base__mutex_lock(&amsgq->send_mutex);
    // search for a free handle if necessary
    if (amsgq->send_tailpos == amsgq->msg_count) {
      int ret = amsgq_test_sendreqs_unsafe(amsgq);
      if (ret != DART_OK) {
        dart__base__mutex_unlock(&amsgq->send_mutex);
        return ret;
      }
    }
    int idx = amsgq->send_tailpos++;
    DART_LOG_TRACE("Send request idx: %d", idx);

    char *sendbuf = amsgq->send_bufs[idx];

    memcpy(sendbuf, data, data_size);

    if (amsgq->sync_send) {
      ret = MPI_Issend(
                  sendbuf, data_size,
                  MPI_BYTE, target.id, amsgq->tag, amsgq->comm,
                  &amsgq->send_reqs[idx]);
    } else {
      ret = MPI_Isend(
                  sendbuf, data_size,
                  MPI_BYTE, target.id, amsgq->tag, amsgq->comm,
                  &amsgq->send_reqs[idx]);
      amsgq->send_count[target.id]++;
    }
    DART_LOG_TRACE("Sent message of size %zu to unit %i using request %d",
                   data_size, target.id, idx);

    dart__base__mutex_unlock(&amsgq->send_mutex);
  } else {
    if (amsgq->sync_send) {
      ret = MPI_Ssend(
                  data, data_size,
                  MPI_BYTE, target.id, amsgq->tag, amsgq->comm);
    } else {
      ret = MPI_Send(
                  data, data_size,
                  MPI_BYTE, target.id, amsgq->tag, amsgq->comm);
      amsgq->send_count[target.id]++;
    }
    DART_LOG_TRACE("Sent message of size %zu to unit %i", data_size, target.id);
  }

  if (ret != MPI_SUCCESS) {
    DART_LOG_ERROR("Failed to send active message to unit %i", target.id);
    return DART_ERR_AGAIN;
  }


  return DART_OK;
}

static dart_ret_t
amsg_sendrecv_process_internal(
  struct dart_amsgq_impl_data* amsgq,
  bool                         blocking,
  bool                         has_lock)
{
  uint64_t num_msg;

  DART_ASSERT(amsgq != NULL);

  if (!has_lock) {
    if (!blocking) {
      dart_ret_t ret = dart__base__mutex_trylock(&amsgq->processing_mutex);
      if (ret != DART_OK) {
        return DART_ERR_AGAIN;
      }
    } else {
      dart__base__mutex_lock(&amsgq->processing_mutex);
    }
  }

  do {
    num_msg = 0;
    int outcount = 0;
    MPI_Testsome(
      amsgq->msg_count, amsgq->recv_reqs, &outcount,
      amsgq->recv_outidx, amsgq->recv_status);
    if (outcount > 0) {
      DART_LOG_TRACE("MPI_Testsome: %d/%d incoming messages available\n",
                    outcount, amsgq->msg_count);
    }
    for (int i = 0; i < outcount; ++i) {
      // pick the message
      int   idx  = amsgq->recv_outidx[i];
      char *dbuf = amsgq->recv_bufs[idx];
      int tailpos;
      MPI_Get_elements(&amsgq->recv_status[i], MPI_BYTE, &tailpos);
      int source = amsgq->recv_status[i].MPI_SOURCE;
      if (tailpos == MPI_UNDEFINED) {
        DART_LOG_ERROR("MPI_Get_elements returned MPI_UNDEFINED!");
      }
      DART_LOG_TRACE("Processing received messages (tailpos %d) in buffer %d of %d (idx %d)",
                     tailpos, i, outcount, idx);
      DART_ASSERT(tailpos > 0);
      if (!amsgq->sync_send) {
        amsgq->recv_count[source]++;
      }
      dart__amsgq__process_buffer(dbuf, tailpos);

      // repost the recv
      MPI_Start(&amsgq->recv_reqs[idx]);
      ++num_msg;
    }

    // repeat until we do not find messages anymore
  } while (blocking && num_msg > 0);

  if (!has_lock) {
    dart__base__mutex_unlock(&amsgq->processing_mutex);
  }
  return DART_OK;
}

static
dart_ret_t
dart_amsg_sendrecv_process(struct dart_amsgq_impl_data* amsgq)
{
  return amsg_sendrecv_process_internal(amsgq, false, false);
}

static
bool
dart_amsgq_sendrecv_check_round_completion(struct dart_amsgq_impl_data* amsgq)
{
  for (int i = 0; i < amsgq->comm_size; ++i) {
    // check that we have received at least as many messages as were sent to us
    DART_LOG_TRACE("  recv_round_count[%d]=%ld, recv_count[%d]=%ld", i, amsgq->recv_round_count[i], i, amsgq->recv_count[i]);
    if ((amsgq->recv_round_count[i] - amsgq->recv_count[i]) > 0) {
      return false;
    }
  }
  return true;
}

static
dart_ret_t
dart_amsg_sendrevc_process_blocking(
  struct dart_amsgq_impl_data* amsgq,
  dart_team_t                  team)
{
  MPI_Request req = MPI_REQUEST_NULL;

  dart__base__mutex_lock(&amsgq->processing_mutex);

  // get a copy of the send_count to use in the allreduce
  // other threads may continue sending messages but they are not part of this
  // communication round
  if (!amsgq->sync_send) {
    dart__base__mutex_lock(&amsgq->send_mutex);
    memcpy(amsgq->send_round_count, amsgq->send_count,
          sizeof(int64_t)*amsgq->comm_size);
    memset(amsgq->send_count, 0, sizeof(int64_t)*amsgq->comm_size);
    dart__base__mutex_unlock(&amsgq->send_mutex);
  }

  for (int i = 0; i < amsgq->comm_size; ++i) {
    DART_LOG_TRACE("  send_round_count[%d]=%ld", i, amsgq->send_round_count[i]);
  }

  DART_LOG_TRACE("Starting blocking processing of message queue %p", amsgq);

  int         barrier_flag = 0;
  int         send_flag = 0;
  do {
    amsg_sendrecv_process_internal(amsgq, true, true);
    if (req != MPI_REQUEST_NULL) {
      MPI_Test(&req, &barrier_flag, MPI_STATUS_IGNORE);
      if (barrier_flag) {
        DART_LOG_DEBUG("Finished blocking processing of messages!");
      }
    }
    if (!send_flag) {
      if (!amsgq->direct_send) {
        dart__base__mutex_lock(&amsgq->send_mutex);
        MPI_Testall(amsgq->send_tailpos, amsgq->send_reqs,
                    &send_flag, MPI_STATUSES_IGNORE);
        dart__base__mutex_unlock(&amsgq->send_mutex);
        if (send_flag) {
          DART_LOG_DEBUG("MPI_Testall: all %d sent active messages completed!",
                        amsgq->send_tailpos);
          amsgq->send_tailpos = 0;
        }
      } else {
        // we don't have to wait for direct sends
        send_flag = 1;
      }
      if (send_flag) {
        if (amsgq->sync_send) {
          // for synchronous sends we don't have to track received messages
          MPI_Ibarrier(amsgq->comm, &req);
        } else {
          MPI_Ialltoall(amsgq->send_round_count, 1, MPI_INT64_T,
                        amsgq->recv_round_count, 1, MPI_INT64_T,
                        amsgq->comm, &req);
        }
      }
    }
  } while (!(barrier_flag && send_flag));

  /**
  * final processing of any message that has not yet been processed
  */
  if (!amsgq->sync_send) {
    while (!dart_amsgq_sendrecv_check_round_completion(amsgq)) {
      amsg_sendrecv_process_internal(amsgq, true, true);
    }
  }

  /**
   * final synchronization
   * NOTE: this is needed to ensure that the above processing does not pick up
   *       messages that were sent after the completion of the Ibarrier.
   */
  MPI_Barrier(amsgq->comm);

  /**
   * adjust the counter for actually received messages to account for the
   * messages that should have been received in this round.
   */
  if (!amsgq->sync_send) {
    for (int i = 0; i < amsgq->comm_size; ++i) {
      amsgq->recv_count[i] -= amsgq->recv_round_count[i];
    }
  }

  DART_LOG_TRACE("Finished blocking processing of message queue %p", amsgq);

  dart__base__mutex_unlock(&amsgq->processing_mutex);
  return DART_OK;
}

static
dart_ret_t
dart_amsg_sendrecv_closeq(struct dart_amsgq_impl_data* amsgq)
{
  if (amsgq->send_tailpos > 0) {
    DART_LOG_TRACE("Waiting for %d active messages to complete",
                  amsgq->send_tailpos);
    MPI_Waitall(amsgq->msg_count, amsgq->send_reqs, MPI_STATUSES_IGNORE);
  }

  int outcount = 0;
  MPI_Testsome(
    amsgq->msg_count, amsgq->recv_reqs, &outcount,
    amsgq->recv_outidx, MPI_STATUSES_IGNORE);

  if (outcount) {
    DART_LOG_WARN("Cowardly refusing to invoke %d unhandled incoming active "
                  "messages upon shutdown!", outcount);
  }


  if (!amsgq->direct_send) {
    for (int i = 0; i < amsgq->msg_count; ++i) {
      if (amsgq->recv_reqs[i] != MPI_REQUEST_NULL) {
        MPI_Request_free(&amsgq->recv_reqs[i]);
      }
      free(amsgq->recv_bufs[i]);
      free(amsgq->send_bufs[i]);
    }
    free(amsgq->send_bufs);
    free(amsgq->send_outidx);
    free(amsgq->send_reqs);
  }

  free(amsgq->recv_bufs);
  free(amsgq->recv_reqs);
  free(amsgq->recv_outidx);
  free(amsgq->recv_status);

  dart__base__mutex_destroy(&amsgq->send_mutex);
  dart__base__mutex_destroy(&amsgq->processing_mutex);

  MPI_Comm_free(&amsgq->comm);

  for (int i = 0; i < amsgq->comm_size; ++i) {
    DART_ASSERT_MSG(amsgq->recv_count[i] == 0,
                    "Found unaccounted recv messages from %d: %ld",
                    i, amsgq->recv_count[i]);
    DART_ASSERT_MSG(amsgq->send_count[i] == 0,
                    "Found unaccounted sent messages to %d: %ld",
                    i, amsgq->send_count[i]);
  }

  if (!amsgq->sync_send) {
    free(amsgq->send_count);
    free(amsgq->send_round_count);
    free(amsgq->recv_count);
    free(amsgq->recv_round_count);
  }

  free(amsgq);

  return DART_OK;
}


dart_ret_t dart_amsg_sendrecv_init(dart_amsgq_impl_t* impl)
{
  impl->openq   = dart_amsg_sendrecv_openq;
  impl->closeq  = dart_amsg_sendrecv_closeq;
  impl->trysend = dart_amsg_sendrecv_trysend;
  impl->process = dart_amsg_sendrecv_process;
  impl->process_blocking = dart_amsg_sendrevc_process_blocking;
  return DART_OK;
}


