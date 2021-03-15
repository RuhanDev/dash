
#include <dash/dart/if/dart_communication.h>

#include <dash/dart/base/env.h>
#include <dash/dart/base/mutex.h>
#include <dash/dart/tasking/dart_tasking_copyin.h>
#include <dash/dart/tasking/dart_tasking_remote.h>
#include <dash/dart/tasking/dart_tasking_datadeps.h>
#include <dash/dart/tasking/dart_tasking_wait.h>

#define DEFAULT_WAIT_TYPE COPYIN_WAIT_DETACH_INLINE

#define COPYIN_TASK_PRIO (INT_MAX-1)

#define MEMPOOL_MAGIC_NUM 0xDEADBEEF

typedef struct mem_pool mem_pool_t;
typedef struct mem_pool_elem mem_pool_elem_t;
struct mem_pool_elem{
  DART_STACK_MEMBER_DEF;
  void         *mem;
  uint64_t      magic;
  mem_pool_t   *mpool; // back reference to the memory pool
};

typedef struct mem_pool mem_pool_t;
struct mem_pool{
  mem_pool_t *next;
  size_t      size;
  dart_stack_t elems;
};
static mem_pool_t *mem_pool = NULL;
static dart_mutex_t mpool_lock = DART_MUTEX_INITIALIZER;


// we know that the stack member entry is the first element of the struct
// so we can cast directly
#define DART_MEMPOOL_ELEM_POP(__freelist) \
  (mem_pool_elem_t*)((void*)dart__base__stack_pop(&__freelist))

#define DART_MEMPOOL_ELEM_PUSH(__freelist, __elem) \
  dart__base__stack_push(&__freelist, &DART_STACK_MEMBER_GET(__elem))

static
mem_pool_t *find_mem_pool(size_t size)
{
  mem_pool_t *mpool = mem_pool;
  while (mpool != NULL) {
    if (size == mpool->size) break;
    mpool = mpool->next;
  }
  return mpool;
}

static
void *allocate_from_mempool(size_t size)
{
  // search for an existing memory pool first
  mem_pool_t *mpool = find_mem_pool(size);
  if (NULL == mpool) {
    // create a new mempool and add it to the list
    // take the lock first
    dart__base__mutex_lock(&mpool_lock);
    mpool = find_mem_pool(size);
    if (NULL == mpool) {
      mpool = malloc(sizeof(*mpool));
      dart__base__stack_init(&mpool->elems);
      mpool->size = size;
      DART_STACK_PUSH(mem_pool, mpool);
    }
    dart__base__mutex_unlock(&mpool_lock);
  }

  mem_pool_elem_t *elem = DART_MEMPOOL_ELEM_POP(mpool->elems);

  if (NULL == elem) {
    // allocate a new element
    elem = malloc(sizeof(*elem) + size);
    elem->magic = MEMPOOL_MAGIC_NUM;
    elem->mpool = mpool;
    elem->mem   = (void*)((intptr_t)elem + sizeof(*elem));
  }

  return elem->mem;
}


static
void return_to_mempool(void *mem)
{
  mem_pool_elem_t *elem = (mem_pool_elem_t*) ((intptr_t)mem - sizeof(*elem));
  DART_ASSERT_MSG(elem->magic == MEMPOOL_MAGIC_NUM,
                  "Corrupt memory pool element detected: %p %xu", mem, elem->magic);

  DART_MEMPOOL_ELEM_PUSH(elem->mpool->elems, elem);
}


enum dart_copyin_t {
  COPYIN_IMPL_UNDEFINED,
  COPYIN_IMPL_GET,
  COPYIN_IMPL_SENDRECV
};

enum dart_copyin_wait_t {
  COPYIN_WAIT_UNDEFINED,
  COPYIN_WAIT_BLOCK,         // block the task
  COPYIN_WAIT_DETACH,        // detach the task
  COPYIN_WAIT_DETACH_INLINE, // detach inlined task
  COPYIN_WAIT_YIELD          // test-yield cycle
};

static const struct dart_env_str2int copyin_env_vals[] = {
  {"GET",             COPYIN_IMPL_GET},
  {"SENDRECV",        COPYIN_IMPL_SENDRECV},
  {NULL, 0}
};

static const struct dart_env_str2int wait_env_vals[] = {
  {"BLOCK",           COPYIN_WAIT_BLOCK},
  {"DETACH",          COPYIN_WAIT_DETACH},
  {"DETACH_INLINE",   COPYIN_WAIT_DETACH_INLINE},
  {"YIELD",           COPYIN_WAIT_YIELD},
  {"TESTYIELD",       COPYIN_WAIT_YIELD},
  {NULL, 0}
};

static enum dart_copyin_wait_t wait_type = COPYIN_WAIT_UNDEFINED;

/**
 * Functionality for pre-fetching data asynchronously, to be used in a COPYIN
 * dependency.
 */

struct copyin_taskdata {
  dart_gptr_t   src;         // the global pointer to send from/get from
  size_t        num_bytes;   // number of bytes
  dart_unit_t   unit;        // global unit ID to send to / recv from
  int           tag;         // a tag to use in case of send/recv
};

struct copyin_task {
  struct copyin_task *next;
  dart_task_dep_t in_dep;
  struct copyin_taskdata arg;
};

static struct copyin_task *delayed_tasks = NULL;
static dart_mutex_t delayed_tasks_mtx = DART_MUTEX_INITIALIZER;

/**
 * A task action to prefetch data in a COPYIN dependency.
 * The argument should be a pointer to an object of type
 * struct copyin_taskdata.
 */
static void
dart_tasking_copyin_recv_taskfn(void *data);

/**
 * A task action to send data in a COPYIN dependency
 * (potentially required by the recv_taskfn if two-sided pre-fetch is required).
 * The argument should be a pointer to an object of type
 * struct copyin_taskdata.
 */
static void
dart_tasking_copyin_send_taskfn(void *data);

/**
 * A task action to prefetch data in a COPYIN dependency using dart_get.
 * The argument should be a pointer to an object of type
 * struct copyin_taskdata.
 */
static void
dart_tasking_copyin_get_taskfn(void *data);

static void
wait_for_handle(dart_handle_t *handle);


void
dart_tasking_copyin_init()
{
  wait_type = dart__base__env__str2int(DART_COPYIN_WAIT_ENVSTR, wait_env_vals,
                                       DEFAULT_WAIT_TYPE);
}

void
dart_tasking_copyin_fini()
{
  // nothing to do
}

static dart_ret_t
dart_tasking_copyin_create_task_sendrecv(
  const dart_task_dep_t * dep,
        taskref           local_task)
{
  static int global_tag_counter = 0; // next tag for pre-fetch communication

  // a) send a request for sending to the target
  int tag = 0;

  dart_global_unit_t myid;
  dart_myid(&myid);

  struct copyin_taskdata arg;
  arg.src = DART_GPTR_NULL;

  dart_global_unit_t send_unit;
  dart_team_unit_l2g(dep->gptr.teamid,
                     DART_TEAM_UNIT_ID(dep->gptr.unitid),
                     &send_unit);

  if (myid.id != send_unit.id) {
    tag = global_tag_counter++;
    DART_LOG_TRACE("Copyin: sendrequest with tag %d for task %p to unit %d in phase %d",
                   tag, local_task.local, send_unit.id, dep->phase);
    dart_tasking_remote_sendrequest(
      send_unit, dep->copyin.gptr, dep->copyin.size, tag, dep->phase);
  } else {
    // make it a local copy
    dart_gptr_t src_gptr = dart_tasking_datadeps_localize_gptr(dep->copyin.gptr);
    arg.src = src_gptr;
  }
  // b) add the receive to the destination

  arg.tag       = tag;
  arg.num_bytes = dep->copyin.size;
  arg.unit      = send_unit.id;

  int ndeps = 1;
  dart_task_dep_t deps[2];
  deps[0] = *dep;
  deps[0].type = DART_DEP_COPYIN_OUT;

  // output dependency on the buffer if provided
  if (NULL != dep->copyin.dest) {
    ++ndeps;
    deps[1].type = DART_DEP_OUT;
    dart_gptr_t dest_gptr;
    dest_gptr.addr_or_offs.addr = dep->copyin.dest;
    dest_gptr.flags = 0;
    dart_global_unit_t guid;
    dart_myid(&guid);
    dest_gptr.unitid = guid.id;
    dest_gptr.teamid = DART_TEAM_ALL;
    dest_gptr.segid  = DART_SEGMENT_LOCAL;
    deps[1].gptr = dest_gptr;
  }


  DART_LOG_TRACE("Copyin: creating task to recv from unit %d with tag %d in phase %d",
                 arg.unit, tag, dep->phase);

  int flags = (wait_type == COPYIN_WAIT_DETACH_INLINE)
              ? DART_TASK_NOYIELD : 0;
  dart_task_t *task;
  dart__tasking__create_task(
      &dart_tasking_copyin_recv_taskfn, &arg, sizeof(arg),
      deps, ndeps, COPYIN_TASK_PRIO, flags, "COPYIN (RECV)", &task);

  // set the communication flag
  DART_TASK_SET_FLAG(task, DART_TASK_IS_COMMTASK);

  // free the handle
  dart__tasking__taskref_free(&task);

  return DART_OK;
}

static dart_ret_t
dart_tasking_copyin_create_task_get(
  const dart_task_dep_t *dep,
        taskref          local_task)
{
  // unused
  (void)local_task;

  int ndeps = 2;
  dart_task_dep_t deps[3];
  deps[0].type  = DART_DEP_IN;
  deps[0].phase = dep->phase;
  deps[0].gptr  = dep->copyin.gptr;

  deps[1] = *dep;
  deps[1].type = DART_DEP_COPYIN_OUT;

  // output dependency on the buffer if provided
  if (NULL != dep->copyin.dest) {
    ++ndeps;
    deps[2].type = DART_DEP_OUT;
    dart_gptr_t dest_gptr;
    dest_gptr.addr_or_offs.addr = dep->copyin.dest;
    dest_gptr.flags = 0;
    dart_global_unit_t guid;
    dart_myid(&guid);
    dest_gptr.unitid = guid.id;
    dest_gptr.teamid = DART_TEAM_ALL;
    dest_gptr.segid  = DART_SEGMENT_LOCAL;
    deps[2].gptr = dest_gptr;
  }

  struct copyin_taskdata arg;
  arg.tag       = 0; // not needed
  arg.src       = dep->copyin.gptr;
  arg.num_bytes = dep->copyin.size;
  arg.unit      = 0; // not needed

  int flags = (wait_type == COPYIN_WAIT_DETACH_INLINE)
              ? DART_TASK_NOYIELD : 0;

  dart_task_t *task;
  dart__tasking__create_task(
    &dart_tasking_copyin_get_taskfn, &arg, sizeof(arg),
    deps, ndeps, COPYIN_TASK_PRIO, flags, "COPYIN (GET)", &task);

  // set the communication flag
  DART_TASK_SET_FLAG(task, DART_TASK_IS_COMMTASK);

  // free the handle
  dart__tasking__taskref_free(&task);

  return DART_OK;
}

dart_ret_t
dart_tasking_copyin_create_task(
  const dart_task_dep_t *dep,
        taskref          local_task)
{
  static enum dart_copyin_t impl = COPYIN_IMPL_UNDEFINED;
  if (impl == COPYIN_IMPL_UNDEFINED) {
    // no locking needed here, copyin will be used only by master thread
    impl = dart__base__env__str2int(DART_COPYIN_IMPL_ENVSTR,
                                    copyin_env_vals, COPYIN_IMPL_GET);
    DART_LOG_INFO("Using copyin implementation %s",
                  impl == COPYIN_IMPL_GET ? "GET" : "SENDRECV");
  }

  dart_ret_t ret;
  if (impl == COPYIN_IMPL_SENDRECV) {
    ret =  dart_tasking_copyin_create_task_sendrecv(dep, local_task);
  } else if (impl == COPYIN_IMPL_GET) {
    ret =  dart_tasking_copyin_create_task_get(dep, local_task);
  } else {
    // just in case...
    DART_ASSERT(impl == COPYIN_IMPL_GET || impl == COPYIN_IMPL_SENDRECV);
    DART_LOG_ERROR("Unknown copyin type: %d", impl);
    ret = DART_ERR_INVAL;
  }

  return ret;
}

void
dart_tasking_copyin_sendrequest(
  dart_gptr_t            src_gptr,
  int32_t                num_bytes,
  dart_taskphase_t       phase,
  int                    tag,
  dart_global_unit_t     unit)
{
  struct copyin_task *ct = malloc(sizeof(*ct));
  ct->arg.src       = dart_tasking_datadeps_localize_gptr(src_gptr);
  ct->arg.num_bytes = num_bytes;
  ct->arg.unit      = unit.id;
  ct->arg.tag       = tag;

  ct->in_dep.type   = DART_DEP_DELAYED_IN;
  ct->in_dep.phase  = phase;
  ct->in_dep.gptr   = src_gptr;

  DART_LOG_TRACE("Copyin: defering task creation to send to unit %d with tag %d in phase %d",
                 unit.id, tag, phase);

  dart__base__mutex_lock(&delayed_tasks_mtx);
  DART_STACK_PUSH(delayed_tasks, ct);
  dart__base__mutex_unlock(&delayed_tasks_mtx);
}

void
dart_tasking_copyin_create_delayed_tasks()
{
  dart__base__mutex_lock(&delayed_tasks_mtx);
  while(delayed_tasks != NULL) {
    struct copyin_task *ct;
    DART_STACK_POP(delayed_tasks, ct);
    DART_LOG_TRACE("Copyin: creating task to send to unit %d with tag %d",
                  ct->arg.unit, ct->arg.tag);
    int flags = (wait_type == COPYIN_WAIT_DETACH_INLINE)
                ? DART_TASK_NOYIELD : 0;

    dart__tasking__create_task(&dart_tasking_copyin_send_taskfn, &ct->arg, sizeof(ct->arg),
                               &ct->in_dep, 1, COPYIN_TASK_PRIO, flags, "COPYIN (SEND)", NULL);
    free(ct);
  }
  dart__base__mutex_unlock(&delayed_tasks_mtx);
}

static void
dart_tasking_copyin_send_taskfn(void *data)
{
  struct copyin_taskdata *td = (struct copyin_taskdata*) data;

  DART_LOG_TRACE("Copyin: Posting send to unit %d (tag %d, size %zu)",
                 td->unit, td->tag, td->num_bytes);
  dart_handle_t handle;
  dart_send_handle(td->src.addr_or_offs.addr, td->num_bytes, DART_TYPE_BYTE,
                   td->tag, DART_GLOBAL_UNIT_ID(td->unit), &handle);
  wait_for_handle(&handle);

  if (wait_type != COPYIN_WAIT_DETACH && wait_type != COPYIN_WAIT_DETACH_INLINE) {
    DART_LOG_TRACE("Copyin: Send to unit %d completed (tag %d)",
                   td->unit, td->tag);
  }
}

static void
dart_tasking_copyin_release_mem(dart_dephash_elem_t *dephash)
{
  DART_ASSERT_MSG(dephash != NULL && dephash->dep.copyin.dest != NULL,
                  "Invalid dephash element passed in destuctor: %p (dest %p)",
                  dephash, dephash->dep.copyin.dest);
  return_to_mempool(dephash->dep.copyin.dest);
  dephash->dep.copyin.dest = NULL;
}

static
dart_task_dep_t *
dart_tasking_copyin_prepare_dep()
{
  // find the dependency in the task's dependency list
  dart_task_t *task = dart__tasking__current_task();
  dart_task_dep_t *dep = NULL;
  dart_dephash_elem_t *elem;
  for (elem = task->deps_owned; elem != NULL; elem = elem->next_in_task) {
    if (DART_DEP_COPYIN_OUT == elem->dep.type) {
      dep = &elem->dep;
      break;
    }
  }

  DART_ASSERT_MSG(dep != NULL, "Failed to find COPYIN dependency for copyin task %p", task);

  if (NULL == dep->copyin.dest) {
    dep->copyin.dest = allocate_from_mempool(dep->copyin.size);
    elem->dtor = &dart_tasking_copyin_release_mem;
  }

  return dep;
}

static void
dart_tasking_copyin_recv_taskfn(void *data)
{
  struct copyin_taskdata *td = (struct copyin_taskdata*) data;

  dart_task_dep_t *dep = dart_tasking_copyin_prepare_dep();

  if (DART_GPTR_ISNULL(td->src)) {
    DART_LOG_TRACE("Copyin: Posting recv from unit %d (tag %d, size %zu)",
                   td->unit, td->tag, td->num_bytes);

    dart_handle_t handle;
    dart_recv_handle(dep->copyin.dest, td->num_bytes, DART_TYPE_BYTE,
                    td->tag, DART_GLOBAL_UNIT_ID(td->unit), &handle);
    wait_for_handle(&handle);
    if (wait_type != COPYIN_WAIT_DETACH && wait_type != COPYIN_WAIT_DETACH_INLINE) {
      DART_LOG_TRACE("Copyin: Recv from unit %d completed (tag %d)",
                     td->unit, td->tag);
    }
  } else {
    DART_LOG_TRACE("Local memcpy of size %zu: %p -> %p",
                   td->num_bytes, td->src.addr_or_offs.addr, dep->copyin.dest);
    memcpy(dep->copyin.dest, td->src.addr_or_offs.addr, td->num_bytes);
  }

}


static void
dart_tasking_copyin_get_taskfn(void *data)
{
  struct copyin_taskdata *td = (struct copyin_taskdata*) data;

  dart_task_dep_t *dep = dart_tasking_copyin_prepare_dep();

  DART_LOG_TRACE("Copyin: Posting GET from unit %d (size %zu)",
                 td->unit, td->num_bytes);
  dart_handle_t handle;
  dart_get_handle(dep->copyin.dest, dep->copyin.gptr, dep->copyin.size,
                  DART_TYPE_BYTE, DART_TYPE_BYTE, &handle);
  wait_for_handle(&handle);
  if (wait_type != COPYIN_WAIT_DETACH && wait_type != COPYIN_WAIT_DETACH_INLINE) {
    DART_LOG_TRACE("Copyin: GET from unit %d completed (size %zu)",
                   td->unit, td->num_bytes);
  }
}


static void
wait_for_handle(dart_handle_t *handle)
{
  if (wait_type == COPYIN_WAIT_BLOCK) {
    dart__task__wait_handle(handle, 1);
  } else if (wait_type == COPYIN_WAIT_DETACH ||
             wait_type == COPYIN_WAIT_DETACH_INLINE) {
    dart__task__detach_handle(handle, 1);
  } else {
    // lower task priority to better overlap communication/computation
    dart_task_t *task = dart__tasking__current_task();
    task->prio = DART_PRIO_LOW;
    while (1) {
      int32_t flag;
      dart_test_local(handle, &flag);
      if (flag) break;
      dart_task_yield(-1);
    }
    task->prio = COPYIN_TASK_PRIO;
  }
}


void *
dart__tasking__copyin_info(dart_task_t *task, int depnum)
{
  dart_dephash_elem_t *elem;
  int i = 0;
  for (elem = task->deps_owned; elem != NULL; elem = elem->next_in_task) {
    if (elem->dep.type == DART_DEP_COPYIN || elem->dep.type == DART_DEP_COPYIN_R) {
      if (i == depnum) {
        break;
      }
      ++i;
    }
  }

  DART_ASSERT_MSG(elem != NULL, "Failed to find copyin dep %d in task %p", depnum, task);
  // look at the output dependency this copyin is attached to (either output or a copyin_out)
  dart_dephash_elem_t *out_elem = elem->dep_list;
  if (out_elem->dep.type == DART_DEP_COPYIN_OUT) {
    // return the destination address of the dependency that copied it locally
    return out_elem->dep.copyin.dest;
  } else {
    // return the address referenced in the last out dependency
    return out_elem->dep.gptr.addr_or_offs.addr;
  }
}
