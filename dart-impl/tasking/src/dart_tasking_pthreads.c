
#include <dash/dart/base/logging.h>
#include <dash/dart/base/atomic.h>
#include <dash/dart/base/assert.h>
#include <dash/dart/base/macro.h>
#include <dash/dart/if/dart_tasking.h>
#include <dash/dart/if/dart_active_messages.h>
#include <dash/dart/base/hwinfo.h>
#include <dash/dart/base/env.h>
#include <dash/dart/base/stack.h>
#include <dash/dart/tasking/dart_tasking_priv.h>
#include <dash/dart/tasking/dart_tasking_signal.h>
#include <dash/dart/tasking/dart_tasking_ayudame.h>
#include <dash/dart/tasking/dart_tasking_taskqueue.h>
#include <dash/dart/tasking/dart_tasking_tasklist.h>
#include <dash/dart/tasking/dart_tasking_taskqueue.h>
#include <dash/dart/tasking/dart_tasking_datadeps.h>
#include <dash/dart/tasking/dart_tasking_remote.h>
#include <dash/dart/tasking/dart_tasking_context.h>
#include <dash/dart/tasking/dart_tasking_cancellation.h>
#include <dash/dart/tasking/dart_tasking_affinity.h>
#include <dash/dart/tasking/dart_tasking_envstr.h>
#include <dash/dart/tasking/dart_tasking_wait.h>
#include <dash/dart/tasking/dart_tasking_copyin.h>
#include <dash/dart/tasking/dart_tasking_extrae.h>
#include <dash/dart/tasking/dart_tasking_craypat.h>

#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <setjmp.h>
#include <stddef.h>

#if !defined(DART_TASKING_USE_OPENMP)

#define EVENT_ENTER(_ev) do {\
  EXTRAE_ENTER(_ev);         \
  CRAYPAT_ENTER(_ev); \
} while (0)

#define EVENT_EXIT(_ev) do {\
  EXTRAE_EXIT(_ev);         \
  CRAYPAT_EXIT(_ev); \
} while (0)

#define CLOCK_DIFF_USEC(start, end)  \
  (uint64_t)((((end).tv_sec - (start).tv_sec)*1E6 + ((end).tv_nsec - (start).tv_nsec)/1E3))
// the grace period after which idle thread go to sleep
#define IDLE_THREAD_GRACE_USEC 1000
// the amount of usec idle threads should sleep within the grace period
#define IDLE_THREAD_GRACE_SLEEP_USEC 100
// the number of us a thread should sleep if IDLE_THREAD_SLEEP is not defined
#define IDLE_THREAD_DEFAULT_USLEEP 1000
// the number of tasks to wait until remote progress is triggered (10ms)
#define REMOTE_PROGRESS_INTERVAL_USEC  1E4

// we know that the stack member entry is the first element of the struct
// so we can cast directly
#define DART_TASKLIST_ELEM_POP(__freelist) \
  (dart_task_t*)((void*)dart__base__stack_pop(&__freelist))

#define DART_TASKLIST_ELEM_PUSH(__freelist, __elem) \
  dart__base__stack_push(&__freelist, &DART_STACK_MEMBER_GET(__elem))

// true if threads should process tasks. Set to false to quit parallel processing
static volatile bool parallel         = false;
// true if the tasking subsystem has been initialized
static          bool initialized      = false;
// true if the worker threads are running (delayed thread-startup)
static          bool threads_running  = false;
// whether or not worker threads should poll for incoming remote messages
// Disabling this in the task setup phase might be beneficial due to
// MPI-internal congestion
static volatile bool worker_poll_remote = false;

static int num_threads;
static int num_utility_threads = 0;

// whether or not to respect numa placement
static bool respect_numa  = false;
// the number of numa nodes
static int num_numa_nodes = 1;

// thread-private data
static _Thread_local dart_thread_t* __tpd = NULL;

// mutex and conditional variable to wait for tasks to get ready
static pthread_cond_t  task_avail_cond   = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t thread_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

// task life-cycle list, the tasks are not free'd directly but instead the
// memory is free'd through the memory pool
static dart_stack_t *task_free_lists;

static dart_thread_t **thread_pool;

static bool bind_threads = false;

static dart_taskqueue_t *task_queue;

static size_t num_units;

enum dart_thread_idle_t {
  DART_THREAD_IDLE_POLL,
  DART_THREAD_IDLE_USLEEP,
  DART_THREAD_IDLE_WAIT
};

static struct dart_env_str2int thread_idle_env[] = {
  {"POLL",   DART_THREAD_IDLE_POLL},
  {"USLEEP", DART_THREAD_IDLE_USLEEP},
  {"WAIT",   DART_THREAD_IDLE_WAIT},
};
static enum dart_thread_idle_t thread_idle_method;

static struct timespec thread_idle_sleeptime;

// a dummy task that serves as a root task for all other tasks
static dart_task_t root_task = {
    .next             = NULL,
    .prev             = NULL,
    .flags            = 0,
    .fn               = NULL,
    .data             = NULL,
    .successor        = NULL,
    .parent           = NULL,
    .remote_successor = NULL,
    .local_deps       = NULL,
    .prio             = DART_PRIO_DEFAULT,
    .num_children     = 0,
    .state            = DART_TASK_ROOT,
    .descr            = "root_task"
#ifdef TRACK_CHILDREN
    , .children       = NULL
#endif
};


/* Memory pool for task objects. The memory is never reclaimed,
 * tasks are instead inserted into the free list upon release.
 */
#define TASK_MEMPOOL_SIZE 64
typedef
struct task_mempool task_mempool_t;

struct task_mempool {
  size_t pos;
  task_mempool_t *next;
  dart_task_t tasks[TASK_MEMPOOL_SIZE];
};

/* Thread-private task memory pool */
static _Thread_local task_mempool_t* __taskpool = NULL;

/*
 * Back-references to each thread's memory pool, which will be used for
 * eventually free'ing the memory allocated in private.
 */
static task_mempool_t **thread_task_mempool = NULL;

static void
destroy_threadpool();

static inline
void set_current_task(dart_task_t *t);

static inline
dart_task_t * get_current_task();

static inline
dart_thread_t * get_current_thread();

static
dart_task_t * next_task(dart_thread_t *thread);

static
void handle_task(dart_task_t *task, dart_thread_t *thread);

static
void remote_progress(dart_thread_t *thread, bool force);

static inline void
dart__tasking__handle_task_internal(dart_task_t *task, dart_thread_t *thread);

static int64_t acc_matching_time_us = 0;
static int64_t acc_idle_time_us     = 0;
static int64_t acc_post_time_us     = 0;
_Thread_local static int64_t thread_acc_idle_time_us = 0;
_Thread_local static int64_t thread_idle_start_ts    = 0;
_Thread_local static int64_t thread_acc_post_time_us        = 0;

dart_task_t *
dart__tasking__root_task()
{
  return &root_task;
}

void
dart__tasking__mark_detached(dart_taskref_t task)
{
  LOCK_TASK(task);
  task->state = DART_TASK_DETACHED;
  UNLOCK_TASK(task);
}

void
dart__tasking__release_detached(dart_taskref_t task)
{
  DART_ASSERT(task->state == DART_TASK_DETACHED);

  dart_thread_t *thread = get_current_thread();

  dart_tasking_datadeps_release_local_task(task, thread);

  // we need to lock the task shortly here before releasing datadeps
  // to allow for atomic check and update
  // of remote successors in dart_tasking_datadeps_handle_remote_task
  LOCK_TASK(task);
  task->state = DART_TASK_FINISHED;
  bool has_ref = DART_TASK_HAS_FLAG(task, DART_TASK_HAS_REF);
  UNLOCK_TASK(task);

  dart_task_t *parent = task->parent;

  // clean up
  if (!has_ref){
    // only destroy the task if there are no references outside
    // referenced tasks will be destroyed in task_wait/task_freeref
    // TODO: this needs some more thoughts!
    dart__tasking__destroy_task(task);
  }


  // let the parent know that we are done
  int32_t nc = DART_DEC_AND_FETCH32(&parent->num_children);
  DART_LOG_DEBUG("Parent %p has %i children left\n", parent, nc);
}

dart_taskqueue_t *
dart__tasking__get_taskqueue()
{
  // TODO: make sure thread-local tasks are somehow accessible in the cancellation!
  dart_thread_t *thread = get_current_thread();
  dart_taskqueue_t *q;
  q = &task_queue[thread->numa_id];
  return q;
}

static void
invoke_taskfn(dart_task_t *task)
{
  DART_ASSERT(task != NULL && task->fn != NULL);
  DART_LOG_DEBUG("Invoking task %p (fn:%p data:%p descr:'%s')",
                 task, task->fn, task->data, task->descr);
  if (setjmp(task->taskctx->cancel_return) == 0) {
    task->fn(task->data);
    DART_LOG_DEBUG("Done with task %p (fn:%p data:%p descr:'%s')",
                   task, task->fn, task->data, task->descr);
  } else {
    // we got here through longjmp, the task is cancelled
    task->state = DART_TASK_CANCELLED;
    DART_LOG_DEBUG("Task %p (fn:%p data:%p) cancelled", task, task->fn, task->data);
  }
}

#ifdef USE_UCONTEXT

static
void requeue_task(dart_task_t *task)
{
  dart_thread_t *thread = get_current_thread();
  dart_taskqueue_t *q = dart__tasking__get_taskqueue();
  int delay = thread->delay;
  if (delay == 0) {
    dart_tasking_taskqueue_push(q, task);
  } else if (delay > 0) {
    dart_tasking_taskqueue_insert(q, task, delay);
  } else {
    dart_tasking_taskqueue_pushback(q, task);
  }
}

static
void wrap_task(dart_task_t *task)
{
  DART_ASSERT(task != &root_task);
  // invoke the new task
  EVENT_ENTER(EVENT_TASK);
  invoke_taskfn(task);
  EVENT_EXIT(EVENT_TASK);
  // return into the current thread's main context
  // this is not necessarily the thread that originally invoked the task
  dart_thread_t *thread = get_current_thread();
  dart__tasking__context_invoke(&thread->retctx);
}

static
void invoke_task(dart_task_t *task, dart_thread_t *thread)
{
  DART_LOG_TRACE("invoke_task: %p, cancellation %d", task, dart__tasking__cancellation_requested());
  if (!dart__tasking__cancellation_requested()) {
    if (task->taskctx == NULL) {
      DART_ASSERT(task->fn != NULL);
      // create a context for a task invoked for the first time
      task->taskctx = dart__tasking__context_create(
                        (context_func_t*)&wrap_task, task);
    }

    // update current task
    set_current_task(task);
    // store current thread's context and jump into new task
    dart__tasking__context_swap(&thread->retctx, task->taskctx);
    DART_LOG_TRACE("Returning from task %p ('%s')", task, task->descr);
  } else {
    DART_LOG_TRACE("Skipping task %p because cancellation has been requested!",
                   task);

    // simply set the current task
    set_current_task(task);
  }
}

dart_ret_t
dart__tasking__yield(int delay)
{
  if (!threads_running) {
    // threads are not running --> no tasks to yield to
    return DART_OK;
  }

  dart_thread_t *thread = get_current_thread();
  // save the current task
  dart_task_t *current_task = dart_task_current_task();

  if (dart__tasking__cancellation_requested())
    dart__tasking__abort_current_task(thread);

  // we cannot yield from inlined tasks
  if (DART_TASK_HAS_FLAG(current_task, DART_TASK_INLINE)) {
    return DART_ERR_INVAL;
  }

  // exit task if the task is blocked and return as soon as we get back here
  if (current_task->state == DART_TASK_BLOCKED) {
    return dart__tasking__context_swap(current_task->taskctx, &thread->retctx);
  }

  dart_task_t *next = next_task(thread);
  if (next == NULL) {
    // progress
    remote_progress(thread, (next == NULL));
    // try again
    next = next_task(thread);
  }

  if (next) {
    thread->delay = delay;

    DART_LOG_TRACE("Yield: leaving task %p ('%s') to yield to next task %p ('%s')",
                    current_task, current_task->descr, next, next->descr);

    if (current_task == &root_task) {
      // NOTE: the root task is not suspended and requeued, the master thread
      //       will jump back into it (see above)
      // NOTE: worker thread will never call yield from within the root task
      DART_ASSERT(thread->thread_id == 0);

      // invoke the task directly
      dart__tasking__handle_task_internal(next, thread);
    } else {
      // mark task as suspended to avoid invoke_task to update the retctx
      // the next task should return to where the current task would have
      // returned
      if (current_task->wait_handle == NULL) {
        current_task->state  = DART_TASK_SUSPENDED;
      } else {
        current_task->state  = DART_TASK_BLOCKED;
      }
      // we got a task, store it in the thread and leave this task
      DART_ASSERT(thread->next_task == NULL);
      thread->next_task = next;
      // here we leave this task
      dart__tasking__context_swap(current_task->taskctx, &thread->retctx);
      // sanity check after returning
      DART_ASSERT_MSG(get_current_task()->state == DART_TASK_RUNNING,
                      "Expected state: %d, found %d for task  %p",
                      DART_TASK_RUNNING,
                      get_current_task()->state,
                      get_current_task());
    }
    // sanity checks after returning to this task
    DART_LOG_TRACE("Yield: got back into task %p", get_current_task());
    DART_ASSERT(get_current_task() == current_task);
  } else {
    //DART_LOG_TRACE("Yield: no task to yield to from task %p",
    //                current_task);
  }


  return DART_OK;
}

#else
dart_ret_t
dart__tasking__yield(int delay)
{
  if (!threads_running) {
    // threads are not running --> no tasks to yield to
    return DART_OK;
  }

  // "nothing to be done here" (libgomp)
  // we do not execute another task to prevent serialization
  DART_LOG_DEBUG("Skipping dart__task__yield");
  // progress
  remote_progress(get_current_thread(), false);
  // check for abort
  if (dart__tasking__cancellation_requested())
    dart__tasking__abort_current_task(thread);

  return DART_OK;
}


static
void invoke_task(dart_task_t *task, dart_thread_t *thread)
{
  // set new task
  set_current_task(task);

  // allocate a context (required for setjmp)
  task->taskctx = dart__tasking__context_create(
                    (context_func_t*)&wrap_task, task);

  //invoke the task function
  invoke_taskfn(task);
}
#endif // USE_UCONTEXT


static void wait_for_work(enum dart_thread_idle_t method)
{
  if (method == DART_THREAD_IDLE_WAIT) {
    DART_LOG_TRACE("Thread %d going to sleep waiting for work",
                  get_current_thread()->thread_id);
    pthread_mutex_lock(&thread_pool_mutex);
    if (parallel) {
      pthread_cond_wait(&task_avail_cond, &thread_pool_mutex);
    }
    pthread_mutex_unlock(&thread_pool_mutex);
    DART_LOG_TRACE("Thread %d waking up", get_current_thread()->thread_id);
  } else if (method == DART_THREAD_IDLE_USLEEP) {
    nanosleep(&thread_idle_sleeptime, NULL);
  }
}

static void wakeup_thread_single()
{
  if (thread_idle_method == DART_THREAD_IDLE_WAIT) {
    pthread_mutex_lock(&thread_pool_mutex);
    pthread_cond_signal(&task_avail_cond);
    pthread_mutex_unlock(&thread_pool_mutex);
  }
}

static void wakeup_thread_all()
{
  if (thread_idle_method == DART_THREAD_IDLE_WAIT) {
    pthread_mutex_lock(&thread_pool_mutex);
    pthread_cond_broadcast(&task_avail_cond);
    pthread_mutex_unlock(&thread_pool_mutex);
  }
}

static int determine_num_threads()
{
  int num_threads = dart__base__env__number(DART_NUMTHREADS_ENVSTR, -1);

  if (num_threads == -1) {
    // query hwinfo
    dart_hwinfo_t hw;
    dart_hwinfo(&hw);
    if (hw.num_cores > 0) {
      num_threads = hw.num_cores * ((hw.max_threads > 0) ? hw.max_threads : 1);
      if (num_threads <= 0) {
        num_threads = -1;
      }
    }
  }

  if (num_threads == -1) {
    DART_LOG_WARN("Failed to get number of cores! Playing it safe with 2 threads...");
    num_threads = 2;
  }

  return num_threads;
}

static inline
dart_thread_t * get_current_thread()
{
  return __tpd;
}

static inline
void set_current_task(dart_task_t *t)
{
  get_current_thread()->current_task = t;
}

static inline
dart_task_t * get_current_task()
{
  return get_current_thread()->current_task;
}

/**
 * Try to get a task from the thread-local queue.
 */
static
dart_task_t * next_task_thread(dart_thread_t *target_thread)
{
  for (int i = 0; i < THREAD_QUEUE_SIZE; ++i) {
    dart_task_t *task = target_thread->queue[i];
    if (task != NULL &&
        DART_COMPARE_AND_SWAPPTR(&target_thread->queue[i], task, NULL)) {
      DART_LOG_TRACE("Taking task %p from slot %d of thread %d",
                    task, i, target_thread->thread_id);
      return task;
    }
  }
  return NULL;
}

/**
 * Try to get a task from the back of the thread-local queue.
 */
static
dart_task_t * next_task_thread_back(dart_thread_t *target_thread)
{
  for (int i = THREAD_QUEUE_SIZE-1; i >= 0; --i) {
    dart_task_t *task = target_thread->queue[i];
    if (task != NULL &&
        DART_COMPARE_AND_SWAPPTR(&target_thread->queue[i], task, NULL)) {
      DART_LOG_TRACE("Taking task %p from slot %d of thread %d",
                    task, i, target_thread->thread_id);
      return task;
    }
  }
  return NULL;
}

static
dart_task_t * next_task(dart_thread_t *thread)
{
  dart_task_t *task;
  if (thread->next_task != NULL) {
    task = thread->next_task;
    thread->next_task = NULL;
  } else {
    task = next_task_thread(thread);
  }
  if (task != NULL) return task;

  // try to steal from the last successful thread
  task = next_task_thread_back(thread_pool[thread->last_steal_thread_id]);

  if (task != NULL) return task;

  // if not successful, try to steal from another thread on the same NUMA node
  for (int target = (thread->thread_id + 1) % num_threads;
        target   != thread->thread_id;
        target    = (++target == num_threads) ? 0 : target) {
    dart_thread_t *target_thread = thread_pool[target];
    if (dart__likely(target_thread != NULL) &&
        target_thread->numa_id == thread->numa_id) {
      task = next_task_thread_back(target_thread);
      if (task != NULL) {
        DART_LOG_DEBUG("Stole task %p from thread %i", task, target);
        thread->last_steal_thread_id = target;
        return task;
      }
    }
  }

  // if the thread has no local task, we query the global queue and
  // try to get a task from a taskqeue on our NUMA domain and fall-back to
  // other domains
  int i = 0;
  do {
    task = dart_tasking_taskqueue_pop(
              &task_queue[(thread->numa_id + i) % num_numa_nodes]);
    ++i;
  } while (task == NULL && i < num_numa_nodes);
  if (task != NULL) return task;

  // still no luck, try again with threads on other NUMA nodes
  if (num_numa_nodes > 1) {
    for (int target = (thread->thread_id + 1) % num_threads;
         target    != thread->thread_id;
         target     = (++target == num_threads) ? 0 : target) {
      dart_thread_t *target_thread = thread_pool[target];
      if (dart__likely(target_thread != NULL) &&
          target_thread->numa_id != thread->numa_id) {
        task = next_task_thread_back(target_thread);
        if (task != NULL) {
          DART_LOG_DEBUG("Stole task %p from thread %i", task, target);
          thread->last_steal_thread_id = target;
          return task;
        }
      }
    }
  }

  // no task to find
  return NULL;
}

static
dart_task_t * allocate_task()
{
  dart_task_t *task = NULL;
#ifdef DART_TASKING_NOMEMPOOL
  task = calloc(1, sizeof(*task));
  TASKLOCK_INIT(task);
#else // DART_TASKING_NOMEMPOOL
  task = DART_TASKLIST_ELEM_POP(task_free_lists[dart__tasking__thread_num()]);
#endif // DART_TASKING_NOMEMPOOL

  if (task == NULL) {
    task_mempool_t *taskpool = __taskpool;
    if (taskpool == NULL || taskpool->pos == TASK_MEMPOOL_SIZE-1) {
      // allocate a new task memory pool
      taskpool = malloc(sizeof(task_mempool_t));
      taskpool->pos = 0;
      taskpool->next = __taskpool;
      __taskpool = taskpool;
    }
    // take the next task from the memory pool
    task = &(taskpool->tasks[taskpool->pos++]);
    // owner is only set once, should not change
    task->owner = dart__tasking__thread_num();
    TASKLOCK_INIT(task);
  }

  return task;
}

static
dart_task_t * create_task(
  void (*fn) (void *),
  void             *data,
  size_t            data_size,
  dart_task_prio_t  prio,
  const char       *descr)
{
  dart_task_t *task = allocate_task();
  task->flags        = 0;
  task->remote_successor = NULL;
  task->local_deps    = NULL;
  task->prev          = NULL;
  task->successor     = NULL;
  task->fn            = fn;
  task->num_children  = 0;
  task->parent        = get_current_task();
  task->state         = DART_TASK_NASCENT;
  task->taskctx       = NULL;
  task->unresolved_deps = 0;
  task->unresolved_remote_deps = 0;
  task->deps_owned    = NULL;
  task->wait_handle   = NULL;
  task->numaptr       = NULL;

  // NOTE: never reset the instance counter of the task!
  task->instance++;

  DART_LOG_TRACE("Task %p: data %p, data_size %zu, fn %p",
                 task, data, data_size, fn);

  if (data_size) {
    const size_t var_space_size = DART_TASK_STRUCT_SIZE - offsetof(dart_task_t, inline_data);
    if (data_size > var_space_size) {
      DART_TASK_SET_FLAG(task, DART_TASK_DATA_ALLOCATED);
      task->data           = malloc(data_size);
    } else {
      // use the task-internal buffer
      task->data = (void*)((intptr_t)task + offsetof(dart_task_t, inline_data));
    }
    memcpy(task->data, data, data_size);
  } else {
    task->data           = data;
  }

  if (task->parent->state == DART_TASK_ROOT) {
    task->phase      = dart__tasking__phase_current();
    dart__tasking__phase_add_task();
  } else {
    task->phase      = DART_PHASE_ANY;
  }

  //task->prio          = (prio == DART_PRIO_PARENT) ? task->parent->prio : prio;
  switch (prio) {
    case DART_PRIO_PARENT:
      task->prio       = task->parent->prio;
      break;
    case DART_PRIO_INLINE:
      task->prio       = DART_PRIO_HIGH;
      DART_TASK_SET_FLAG(task, DART_TASK_INLINE);
      DART_TASK_SET_FLAG(task, DART_TASK_IMMEDIATE);
      break;
    default:
      task->prio       = prio;
      break;
  }

  // if descr is an absolute path (as with __FILE__) we only use the basename
  if (descr && descr[0] == '/') {
    const char *descr_base = strrchr(descr, '/');
    task->descr            = descr_base+1;
  } else {
    task->descr = descr;
  }

#ifdef TRACK_CHILDREN
  LOCK_TASK(task->parent);
  dart_tasking_tasklist_prepend(&task->parent->children, task);
  UNLOCK_TASK(task->parent);
  task->children = NULL;
#endif // TRACK_CHILDREN

  return task;
}

void dart__tasking__destroy_task(dart_task_t *task)
{
  if (DART_TASK_HAS_FLAG(task, DART_TASK_DATA_ALLOCATED)) {
    free(task->data);
  }

  // take the task out of the phase
  if (dart__tasking__is_root_task(task->parent)) {
    dart__tasking__phase_take_task(task->phase);
  }

#ifdef TRACK_CHILDREN
  LOCK_TASK(task->parent);
  dart_tasking_tasklist_remove(&task->parent->children, task);
  UNLOCK_TASK(task->parent);
#endif // TRACK_CHILDREN

  dart_tasking_datadeps_reset(task);

  task->state = DART_TASK_DESTROYED;

#ifdef DART_TASKING_NOMEMPOOL
  free(task);
#else // DART_TASKING_NOMEMPOOL
  DART_TASKLIST_ELEM_PUSH(task_free_lists[task->owner], task);
#endif // DART_TASKING_NOMEMPOOL
}

dart_task_t *
dart__tasking__allocate_dummytask()
{
  dart_task_t *task = allocate_task();
  memset(task, 0, sizeof(*task));
  task->state  = DART_TASK_DUMMY;
  task->parent = dart__tasking__current_task();

  if (task->parent->state == DART_TASK_ROOT) {
    task->phase      = dart__tasking__phase_current();
    dart__tasking__phase_add_task();
  } else {
    task->phase      = DART_PHASE_ANY;
  }
  return task;
}

void remote_progress(dart_thread_t *thread, bool force)
{
  // short-cut if we only run on one unit
  if (num_units == 1) return;

  // only progress periodically or if the caller mandates it
  if (force ||
      thread->last_progress_ts + REMOTE_PROGRESS_INTERVAL_USEC >= current_time_us())
  {
    dart_tasking_remote_progress();
    thread->last_progress_ts = current_time_us();
  }
}


/**
 * Execute the given task.
 */
static
void handle_task(dart_task_t *task, dart_thread_t *thread)
{
  if (task != NULL)
  {
    int64_t postprocessing_start_ts;
    DART_LOG_DEBUG("Thread %i executing task %p ('%s')",
                  thread->thread_id, task, task->descr);

    dart_task_t *current_task = get_current_task();

    DART_ASSERT_MSG(IS_ACTIVE_TASK(task), "Invalid state of task %p: %d",
                    task, task->state);
    DART_ASSERT_MSG(task->unresolved_deps == 0,
                    "Runnable task %p has %d unresolved local dependencies",
                    task, task->unresolved_deps);
    DART_ASSERT_MSG(task->unresolved_remote_deps == 0,
                    "Runnable task %p has %d unresolved remote dependencies",
                    task, task->unresolved_remote_deps);

    // set task to running state, protected to prevent race conditions with
    // dependency handling code
    LOCK_TASK(task);
    task->state = DART_TASK_RUNNING;
    UNLOCK_TASK(task);

    // start execution, change to another task in between
    if (thread_idle_start_ts) {
      int64_t idle_time = current_time_us() - thread_idle_start_ts;
      thread_acc_idle_time_us += idle_time;
    }
    invoke_task(task, thread);
    thread_idle_start_ts = postprocessing_start_ts = current_time_us();

    // we're coming back into this task here
    dart_task_t *prev_task = dart_task_current_task();

    DART_LOG_TRACE("Returned from invoke_task(%p, %p): prev_task=%p, state=%d",
                   task, thread, prev_task, prev_task->state);

    if (prev_task->state == DART_TASK_DETACHED) {

      // release the context
      dart__tasking__context_release(task->taskctx);
      task->taskctx = NULL;
      dart__task__wait_enqueue(prev_task);

    } else if (prev_task->state == DART_TASK_BLOCKED) {
      // we came back here because there were no other tasks to yield from
      // the blocked task so we have to make sure this task is enqueued as
      // blocked (see dart__tasking__yield)
      dart__task__wait_enqueue(prev_task);
    } else if (prev_task->state == DART_TASK_SUSPENDED) {
      // the task was yielded, requeue it
      requeue_task(prev_task);
    } else {
      DART_ASSERT_MSG(prev_task->state == DART_TASK_RUNNING ||
                      prev_task->state == DART_TASK_CANCELLED,
                      "Unexpected task state: %d", prev_task->state);
      if (DART_FETCH32(&prev_task->num_children) &&
          !dart__tasking__cancellation_requested()) {
        // Implicit wait for child tasks
        // TODO: really necessary? Can we transfer child ownership to parent->parent?
        dart__tasking__task_complete(true);
      }

      bool has_ref;

      // the task may have changed once we get back here
      task = get_current_task();

      DART_ASSERT(task != &root_task);

      // release dependencies
      dart_tasking_datadeps_release_local_task(task, thread);

      // we need to lock the task shortly here before releasing datadeps
      // to allow for atomic check and update
      // of remote successors in dart_tasking_datadeps_handle_remote_task
      LOCK_TASK(task);
      task->state = DART_TASK_FINISHED;
      has_ref = DART_TASK_HAS_FLAG(task, DART_TASK_HAS_REF);
      UNLOCK_TASK(task);

      // release the context
      dart__tasking__context_release(task->taskctx);
      task->taskctx = NULL;

      dart_task_t *parent = task->parent;


      // clean up
      if (!has_ref){
        // only destroy the task if there are no references outside
        // referenced tasks will be destroyed in task_wait/task_freeref
        // TODO: this needs some more thoughts!
        dart__tasking__destroy_task(task);
      }

      // let the parent know that we are done
      int32_t nc = DART_DEC_AND_FETCH32(&parent->num_children);
      DART_LOG_DEBUG("Parent %p has %i children left\n", parent, nc);
      ++(thread->taskcntr);
    }
    // return to previous task
    set_current_task(current_task);
    acc_post_time_us += current_time_us() - postprocessing_start_ts;
  }
}

/**
 * Execute the given inlined task.
 * Tha task action will be called directly and no context will be created for it.
 */
static
void handle_inline_task(dart_task_t *task, dart_thread_t *thread)
{
  if (task != NULL)
  {
    DART_ASSERT_MSG(task->fn != NULL, "task %p has invalid function!", task);
    DART_LOG_DEBUG("Thread %i executing inlined task %p ('%s')",
                  thread->thread_id, task, task->descr);

    dart_task_t *current_task = get_current_task();

    // set task to running state, protected to prevent race conditions with
    // dependency handling code
    LOCK_TASK(task);
    task->state = DART_TASK_RUNNING;
    UNLOCK_TASK(task);

    // start execution, change to another task in between
    set_current_task(task);

    task->fn(task->data);

    DART_LOG_TRACE("Returned from inlined task (%p, %p)",
                   task, thread);

    dart_task_t *parent = task->parent;

    if (DART_FETCH32(&task->num_children) &&
          !dart__tasking__cancellation_requested()) {
      // Implicit wait for child tasks
      dart__tasking__task_complete(true);
    }

    if (task->state == DART_TASK_DETACHED) {
      dart__task__wait_enqueue(task);
    } else {
      // release dependencies
      dart_tasking_datadeps_release_local_task(task, thread);

      // we need to lock the task shortly
      // to allow for atomic check and update
      // of remote successors in dart_tasking_datadeps_handle_remote_task
      LOCK_TASK(task);
      task->state  = DART_TASK_FINISHED;
      bool has_ref = DART_TASK_HAS_FLAG(task, DART_TASK_HAS_REF);
      UNLOCK_TASK(task);


      // clean up
      if (!has_ref){
        // only destroy the task if there are no references outside
        // referenced tasks will be destroyed in task_wait/task_freeref
        // TODO: this needs some more thoughts!
        dart__tasking__destroy_task(task);
      }

      // let the parent know that we are done
      int32_t nc = DART_DEC_AND_FETCH32(&parent->num_children);
      DART_LOG_DEBUG("Parent %p has %i children left\n", parent, nc);
    }

    // return to previous task
    set_current_task(current_task);
    ++(thread->taskcntr);
  }
}

static inline void
dart__tasking__handle_task_internal(dart_task_t *task, dart_thread_t *thread)
{
  if (NULL == task) return;
  if (DART_TASK_HAS_FLAG(task, DART_TASK_INLINE)) {
    handle_inline_task(task, thread);
  } else {
    handle_task(task, thread);
  }
}

void
dart__tasking__handle_task(dart_task_t *task)
{
  dart_thread_t *thread = dart__tasking__current_thread();
  dart__tasking__handle_task_internal(task, thread);
}


static
void dart_thread_init(dart_thread_t *thread, int threadnum)
{
  thread->thread_id         = threadnum;
  thread->current_task      = &root_task;
  thread->taskcntr          = 0;
  thread->core_id           = 0;
  thread->numa_id           = 0;
  thread->is_utility_thread = false;
  thread->ctx_to_enter      = NULL;
  thread->last_steal_thread_id = 0;
  dart__base__stack_init(&thread->ctxlist);

  DART_LOG_TRACE("Thread %i (%p) has task queue %p",
                 threadnum, thread, &thread->queue);

  if (threadnum == 0)
    DART_LOG_INFO("sizeof(dart_task_t) = %zu", sizeof(dart_task_t));
}

struct thread_init_data {
  pthread_t pthread;
  int       threadid;
};

static
void* thread_main(void *data)
{
  DART_ASSERT(data != NULL);
  struct thread_init_data* tid = (struct thread_init_data*)data;

  DART_LOG_INFO("Thread %d starting up", tid->threadid);
  int core_id = 0;
  if (bind_threads) {
    // leave room for utility threads if we have enough cores
    if (dart__tasking__affinity_num_cores() > (num_utility_threads + num_threads)) {
      dart__tasking__affinity_set(tid->pthread, tid->threadid + num_utility_threads);
    } else {
      dart__tasking__affinity_set(tid->pthread, tid->threadid);
    }
  }

  dart_thread_t *thread = calloc(1, sizeof(dart_thread_t));

  DART_LOG_DEBUG("Thread %d: %p", tid->threadid, thread);

  // populate the thread-private data
  int threadid    = tid->threadid;
  dart_thread_init(thread, threadid);
  thread->pthread = tid->pthread;
  thread->core_id = core_id;
  thread->numa_id =
            respect_numa ? dart__tasking__affinity_core_numa_node(core_id) : 0;
  DART_ASSERT(thread->numa_id >= 0);
  free(tid);
  tid = NULL;

  // set thread-private data
  __tpd = thread;
  // make thread available to other threads
  thread_pool[threadid] = thread;

  set_current_task(&root_task);

  // cache the idle_method here to reduce NUMA effects
  enum dart_thread_idle_t idle_method = thread_idle_method;

  DART_LOG_INFO("Thread %d starting to process tasks", threadid);

  struct timespec begin_idle_ts;
  bool in_idle = false;
  // sleep-time: 100us
  const struct timespec sleeptime = {0, IDLE_THREAD_GRACE_SLEEP_USEC*1000};
  // enter work loop
  while (parallel) {

    // check whether cancellation has been activated
    dart__tasking__check_cancellation(thread);

    // process the next task
    dart_task_t *task = next_task(thread);

    if (!in_idle && task == NULL) {
      EVENT_ENTER(EVENT_IDLE);
    } else if (in_idle && task != NULL) {
      EVENT_EXIT(EVENT_IDLE);
    }

    dart__tasking__handle_task_internal(task, thread);

    //DART_LOG_TRACE("thread_main: finished processing task %p", task);

    // look for incoming remote tasks and responses
    // NOTE: only the first worker thread does the polling
    //       if polling is enabled or we have no runnable tasks anymore

    if ((task == NULL || worker_poll_remote) && threadid == 1) {
      //DART_LOG_TRACE("worker polling for remote messages");
      remote_progress(thread, (task == NULL));
    } else if (task == NULL) {
      struct timespec curr_ts;
      if (!in_idle) {
        // start idle time
        clock_gettime(CLOCK_MONOTONIC, &begin_idle_ts);
        in_idle = true;
      } else {
        // check whether we should go to idle
        clock_gettime(CLOCK_MONOTONIC, &curr_ts);
        uint64_t idle_time = CLOCK_DIFF_USEC(begin_idle_ts, curr_ts);
        // go to sleep if we exceeded the max idle time
        if (idle_time > IDLE_THREAD_GRACE_USEC) {
          wait_for_work(idle_method);
          in_idle = false;
        }
      }
      // wait for 100us to reduce pressure on master thread
      nanosleep(&sleeptime, NULL);
    } else {
      in_idle = false;
    }
  }

  DART_FETCH_AND_ADD64(&acc_idle_time_us, thread_acc_idle_time_us);
  DART_FETCH_AND_ADD64(&acc_post_time_us, thread_acc_post_time_us);

  DART_ASSERT_MSG(
    thread == get_current_thread(), "Detected invalid thread return!");

  // clean up the current thread's contexts before leaving
  dart__tasking__context_cleanup();

  DART_LOG_INFO("Thread %i exiting", dart__tasking__thread_num());

  // unset thread-private data
  __tpd = NULL;

  // make the thread's memory pool available to the main thread
  thread_task_mempool[threadid] = __taskpool;

  return NULL;
}

static
void dart_thread_finalize(dart_thread_t *thread)
{
  if (thread != NULL) {
    thread->thread_id = -1;
    thread->current_task = NULL;
  }
}

static void
start_threads(int num_threads)
{
  DART_ASSERT(!threads_running);
  DART_LOG_INFO("Starting %d threads", num_threads);

  // determine thread idle method
  uint64_t thread_idle_sleeptime_us =
                      dart__base__env__us(DART_THREAD_IDLE_SLEEP_ENVSTR,
                                          IDLE_THREAD_DEFAULT_USLEEP);

  if (thread_idle_method == DART_THREAD_IDLE_USLEEP) {
    thread_idle_sleeptime.tv_sec  = thread_idle_sleeptime_us / (1000*1000);
    thread_idle_sleeptime.tv_nsec =
        (thread_idle_sleeptime_us-(thread_idle_sleeptime.tv_sec*1000*1000))*1000;
    DART_LOG_INFO("Using idle thread method SLEEP with %lu sleep time",
                  thread_idle_sleeptime_us);
  } else {
    DART_LOG_INFO("Using idle thread method %s",
                  thread_idle_method == DART_THREAD_IDLE_POLL ? "POLL" : "WAIT");
  }

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, dart__tasking__context_stack_size());

  // start-up all worker threads
  for (int i = 1; i < num_threads; i++)
  {
    // will be free'd by the thread
    struct thread_init_data *tid = malloc(sizeof(*tid));
    tid->threadid = i;
    int ret = pthread_create(&tid->pthread, &attr,
                             &thread_main, tid);
    if (ret != 0) {
      DART_LOG_ERROR("Failed to create thread %i of %i!", i, num_threads);
    }
  }
  threads_running = true;
}

static void
init_threadpool(int num_threads)
{
  // bind the master thread before allocating meta-data objects
  int core_id = 0;
  if (bind_threads) {
    core_id = dart__tasking__affinity_set(pthread_self(), 0);
  }
  thread_pool = calloc(num_threads, sizeof(dart_thread_t*));
  dart_thread_t *master_thread = calloc(1, sizeof(dart_thread_t));
  // initialize master thread data, the other threads will do it themselves
  dart_thread_init(master_thread, 0);
  master_thread->core_id = core_id;
  master_thread->numa_id =
            respect_numa ? dart__tasking__affinity_core_numa_node(core_id) : 0;
  thread_pool[0] = master_thread;
}

dart_ret_t
dart__tasking__init()
{
  if (initialized) {
    DART_LOG_ERROR("DART tasking subsystem can only be initialized once!");
    return DART_ERR_INVAL;
  }

  thread_idle_method = dart__base__env__str2int(DART_THREAD_IDLE_ENVSTR,
                                                thread_idle_env,
                                                DART_THREAD_IDLE_USLEEP);

  respect_numa  = dart__base__env__bool(DART_THREAD_PLACE_NUMA_ENVSTR, false);

  num_threads = determine_num_threads();
  DART_LOG_INFO("Using %i threads", num_threads);

  DART_LOG_TRACE("root_task: %p", &root_task);

  task_free_lists = malloc(num_threads * sizeof(*task_free_lists));
  for (int i = 0; i < num_threads; ++i) {
    dart__base__stack_init(&task_free_lists[i]);
  }

#ifdef USE_EXTRAE
  if (Extrae_define_event_type) {
    unsigned nvalues = 3;
    Extrae_define_event_type(&et, "Thread State", &nvalues, ev, extrae_names);
  }
#endif

  thread_task_mempool = calloc(num_threads, sizeof(*thread_task_mempool));

  dart__tasking__context_init();

  // initialize thread affinity
  dart__tasking__affinity_init();

  if (respect_numa) {
    num_numa_nodes = dart__tasking__affinity_num_numa_nodes();
  }
  task_queue = malloc(num_numa_nodes * sizeof(task_queue[0]));
  for (int i = 0; i < num_numa_nodes; ++i) {
    dart_tasking_taskqueue_init(&task_queue[i]);
  }

  // keep threads running
  parallel = true;

  // set up the active message queue
  dart_tasking_datadeps_init();

  bind_threads = dart__base__env__bool(DART_THREAD_AFFINITY_ENVSTR, false);

  // initialize all task threads before creating them
  init_threadpool(num_threads);

  // set master thread private data
  __tpd = thread_pool[0];

  set_current_task(&root_task);

#ifdef DART_ENABLE_AYUDAME
  dart__tasking__ayudame_init();
#endif // DART_ENABLE_AYUDAME

  dart_team_size(DART_TEAM_ALL, &num_units);

  dart__task__wait_init();

  dart_tasking_copyin_init();

  dart__tasking__cancellation_init();
#ifdef CRAYPAT
  PAT_record(PAT_STATE_ON);
#endif

  // install signal handler
  dart__tasking__install_signalhandler();

  initialized = true;

  return DART_OK;
}

int
dart__tasking__thread_num()
{
  dart_thread_t *t = get_current_thread();
  return (dart__likely(t) ? t->thread_id : 0);
}

int
dart__tasking__num_threads()
{
  return num_threads;
}

int
dart__tasking__num_tasks()
{
  return root_task.num_children;
}

void
dart__tasking__enqueue_runnable(dart_task_t *task)
{
  if (dart__tasking__cancellation_requested()) {
    dart__tasking__cancel_task(task);
    return;
  }

  if (task->state == DART_TASK_DEFERRED) {
    DART_LOG_TRACE("Refusing to enqueue deferred task %p", task);
    return;
  }

  bool queuable = false;
  uint64_t instance = task->instance;
  LOCK_TASK(task);
  if (task->state == DART_TASK_CREATED)
  {
    if (task->instance == instance &&
        task->state == DART_TASK_CREATED &&
        dart_tasking_datadeps_is_runnable(task)) {
      task->state = DART_TASK_QUEUED;
      queuable = true;
    }
  } else if (task->state == DART_TASK_SUSPENDED) {
    queuable = true;
  }
  UNLOCK_TASK(task);

  // make sure we don't queue the task if we are not allowed to
  if (!queuable) {
    DART_LOG_TRACE("Refusing to enqueue task %p which is in state %d",
                   task, task->state);
    return;
  }

  bool enqueued = false;
  // check whether the task has to be deferred
  if (task->parent == &root_task &&
      !dart__tasking__phase_is_runnable(task->phase)) {
    LOCK_TASK(task);
    // Lock the queue to avoid race conditions with the
    // release of deferred tasks and the phase
    dart_tasking_taskqueue_lock(&local_deferred_tasks);
    if (!dart__tasking__phase_is_runnable(task->phase)) {
      DART_LOG_TRACE("Deferring release of task %p in phase %d (q=%p, s=%zu)",
                     task, task->phase,
                     &local_deferred_tasks,
                     local_deferred_tasks.num_elem);
      if (task->state == DART_TASK_CREATED || task->state == DART_TASK_QUEUED) {
        task->state = DART_TASK_DEFERRED;
        dart_tasking_taskqueue_pushback_unsafe(&local_deferred_tasks, task);
        enqueued = true;
      }
    }
    dart_tasking_taskqueue_unlock(&local_deferred_tasks);
    UNLOCK_TASK(task);
  }

  if (!enqueued && DART_TASK_HAS_FLAG(task, DART_TASK_IS_COMMTASK)) {
    dart_tasking_remote_handle_comm_task(task, &enqueued);
  }

  if (!enqueued){

    // execute immediate tasks directly as inline tasks
    if (DART_TASK_HAS_FLAG(task, DART_TASK_IMMEDIATE)) {
      handle_inline_task(task, get_current_thread());
      return;
    }

    dart_thread_t *thread = get_current_thread();

    int numa_node = 0;
    if (respect_numa && task->numaptr != NULL) {
      numa_node = dart__tasking__affinity_ptr_numa_node(task->numaptr);
    }
    if (!thread->is_utility_thread) {

      if (numa_node == thread->numa_id) {
        for (int i = 0; i < THREAD_QUEUE_SIZE; ++i) {
          if (thread->queue[i] == NULL &&
              DART_COMPARE_AND_SWAPPTR(&thread->queue[i], NULL, task)) {
            DART_LOG_TRACE("Putting task %p into slot %d of thread %d",
                          task, i, thread->thread_id);
            return;
          }
        }
      }
    }

    /**
     * we have not stored the task in the thread, put it in the global queue
     */
    dart_taskqueue_t *q = &task_queue[numa_node];
    dart_tasking_taskqueue_push(q, task);
    // wakeup a thread to execute this task
    wakeup_thread_single();
  }
}

dart_ret_t
dart__tasking__create_task(
          void           (*fn) (void *),
          void            *data,
          size_t           data_size,
          dart_task_dep_t *deps,
          size_t           ndeps,
          dart_task_prio_t prio,
          int              flags,
    const char            *descr,
          dart_taskref_t  *ref)
{
  if (dart__tasking__cancellation_requested()) {
    DART_LOG_WARN("dart__tasking__create_task: Ignoring task creation while "
                  "canceling tasks!");
    return DART_OK;
  }

  // start threads upon first task creation
  if (dart__unlikely(!threads_running)) {
    start_threads(num_threads);
  }

  // TODO: add hash table to handle task descriptions

  dart_task_t *task = create_task(fn, data, data_size, prio, descr);

  if (ref != NULL) {
    DART_TASK_SET_FLAG(task, DART_TASK_HAS_REF);
    *ref = task;
  }

  if (flags & DART_TASK_NOYIELD) {
    DART_TASK_SET_FLAG(task, DART_TASK_INLINE);
  }

  int32_t nc = DART_INC_AND_FETCH32(&task->parent->num_children);
  DART_LOG_DEBUG("Parent %p now has %i children", task->parent, nc);

  dart_tasking_datadeps_handle_task(task, deps, ndeps);

  LOCK_TASK(task);
  task->state = DART_TASK_CREATED;
  bool is_runnable = dart_tasking_datadeps_is_runnable(task);
  UNLOCK_TASK(task);
  DART_LOG_TRACE("  Task %p ('%s') created: runnable %i, prio %d, ndeps %d, nrdeps %d",
                 task, task->descr, is_runnable, task->prio,
                 task->unresolved_deps, task->unresolved_remote_deps);
  if (is_runnable) {
    dart__tasking__enqueue_runnable(task);
  }

  return DART_OK;
}

void
dart__tasking__perform_matching(dart_taskphase_t phase)
{
  if (num_units == 1) {
    // nothing to be done for one unit
    return;
  }
  uint64_t start_ts = current_time_us();
  //printf("Performing matching at phase %d\n", phase);
  // make sure all incoming requests are served
  dart_tasking_remote_progress_blocking(DART_TEAM_ALL);
  // release unhandled remote dependencies
  dart_tasking_datadeps_handle_defered_remote(phase);
  DART_LOG_DEBUG("task_complete: releasing deferred tasks of all threads");
  // make sure all newly incoming requests are served
  // TODO: this is not needed anymore
  // dart_tasking_remote_progress_blocking(DART_TEAM_ALL);
  // reset the active epoch
  dart__tasking__phase_set_runnable(phase);
  // release the deferred queue
  dart_tasking_datadeps_handle_defered_local();
  // wakeup all thread to execute potentially available tasks
  wakeup_thread_all();
  uint64_t end_ts = current_time_us() - start_ts;
  DART_FETCH_AND_ADD64(&acc_matching_time_us, end_ts);
}


dart_ret_t
dart__tasking__task_complete(bool local_only)
{
  if (dart__unlikely(!threads_running)) {
    if (local_only) {
      // threads are not running --> nothing to be done here
      return DART_OK;
    }
    // otherwise start up threads and participate in the task matching
    start_threads(num_threads);
  }

  dart_thread_t *thread = get_current_thread();

  DART_ASSERT_MSG(
    !(thread->current_task == &(root_task) && thread->thread_id != 0),
    "Calling dart__tasking__task_complete() on ROOT task "
    "only valid on MASTER thread!");

  DART_LOG_TRACE("Waiting for child tasks of %p to complete", thread->current_task);

  bool is_root_task = thread->current_task == &(root_task);

  if (is_root_task) {
    if (!local_only) {
      dart_taskphase_t entry_phase;
      entry_phase = dart__tasking__phase_current();
      dart__tasking__perform_matching(entry_phase);
      // enable worker threads to poll for remote messages
      worker_poll_remote = true;
    }
  } else {
    EXTRAE_EXIT(EVENT_TASK);
  }

  // 1) wake up all threads (might later be done earlier)
  if (thread_idle_method == DART_THREAD_IDLE_WAIT) {
    pthread_cond_broadcast(&task_avail_cond);
  }


  // 2) start processing ourselves
  dart_task_t *task = get_current_task();

  DART_LOG_DEBUG("dart__tasking__task_complete: waiting for children of task %p", task);

  // save context
  // TODO is this really necessary?
  context_t tmpctx;
  bool restore_ctx = false;
  if (task->num_children) {
    tmpctx = thread->retctx;
    restore_ctx = true;
  }

  // main task processing routine
  while (task->num_children > 0) {
    dart_task_t *next = next_task(thread);
    // a) look for incoming remote tasks and responses
    if (next == NULL) {
      remote_progress(thread, (thread->thread_id == 0));
      next = next_task(thread);
    }
    // b) check cancellation
    dart__tasking__check_cancellation(thread);
    // d) process our tasks
    dart__tasking__handle_task_internal(next, thread);
    // e) requery the thread as it might have changed
    thread = get_current_thread();
  }

  if (restore_ctx) {
    // restore context (in case we're called from within another task and switched threads)
    thread->retctx = tmpctx;
  }

  // 3) clean up if this was the root task and thus no other tasks are running
  if (is_root_task) {
    // reset the runnable phase
    dart__tasking__phase_set_runnable(DART_PHASE_FIRST);
    // disable remote polling of worker threads
    worker_poll_remote = false;
    // reset the phase counter
    dart__tasking__phase_reset();

    if (!local_only) {
      // wait for all units to finish their tasks
      dart_tasking_remote_progress_blocking(DART_TEAM_ALL);
    }
  } else {
    EXTRAE_ENTER(EVENT_TASK);
  }

  return DART_OK;
}

dart_ret_t
dart__tasking__taskref_free(dart_taskref_t *tr)
{
  if (tr == NULL || *tr == DART_TASK_NULL) {
    return DART_ERR_INVAL;
  }

  // free the task if already destroyed
  LOCK_TASK(*tr);
  DART_TASK_UNSET_FLAG((*tr), DART_TASK_HAS_REF);
  if ((*tr)->state == DART_TASK_FINISHED) {
    UNLOCK_TASK(*tr);
    dart__tasking__destroy_task(*tr);
    *tr = DART_TASK_NULL;
    return DART_OK;
  }

  UNLOCK_TASK(*tr);

  return DART_OK;

}

dart_ret_t
dart__tasking__task_wait(dart_taskref_t *tr)
{

  if (tr == NULL || *tr == NULL || (*tr)->state == DART_TASK_DESTROYED) {
    return DART_ERR_INVAL;
  }

  dart_task_t *reftask = *tr;
  // the task has to be locked to avoid race conditions
  LOCK_TASK(reftask);

  // the thread just contributes to the execution
  // of available tasks until the task waited on finishes
  while (reftask->state != DART_TASK_FINISHED) {
    UNLOCK_TASK(reftask);

    dart_thread_t *thread = get_current_thread();

    dart_task_t *task = next_task(thread);
    if (task == NULL) {
      remote_progress(thread, true);
      task = next_task(thread);
    }
    dart__tasking__handle_task_internal(task, thread);

    // lock the task for the check in the while header
    LOCK_TASK(reftask);
  }

  // finally we have to destroy the task
  UNLOCK_TASK(reftask);
  DART_TASK_UNSET_FLAG(reftask, DART_TASK_HAS_REF);
  dart__tasking__destroy_task(reftask);

  *tr = DART_TASK_NULL;

  return DART_OK;
}

dart_ret_t
dart__tasking__task_test(dart_taskref_t *tr, int *flag)
{
  if (flag == NULL) {
    return DART_ERR_INVAL;
  }
  *flag = 0;
  if (tr == NULL || *tr == NULL || (*tr)->state == DART_TASK_DESTROYED) {
    return DART_ERR_INVAL;
  }

  dart_task_t *reftask = *tr;
  // the task has to be locked to avoid race conditions
  LOCK_TASK(reftask);
  dart_task_state_t state = reftask->state;
  UNLOCK_TASK(reftask);

  // if this is the only available thread we have to execute at least one task
  if (num_threads == 1 && state != DART_TASK_FINISHED) {
    dart_thread_t *thread = get_current_thread();
    dart_task_t *task = next_task(thread);
    remote_progress(thread, task == NULL);
    if (task == NULL) task = next_task(thread);
    dart__tasking__handle_task_internal(task, thread);

    // check if this was our task
    LOCK_TASK(reftask);
    state = reftask->state;
    UNLOCK_TASK(reftask);
  }

  if (state == DART_TASK_FINISHED) {
    *flag = 1;
    dart__tasking__destroy_task(reftask);
    *tr = DART_TASK_NULL;
  }
  return DART_OK;
}

dart_taskref_t
dart__tasking__current_task()
{
  return get_current_task();
}

dart_thread_t *
dart__tasking__current_thread()
{
  return get_current_thread();
}

/**
 * Tear-down related functions.
 */

static void
stop_threads()
{
  // wait for all threads to finish
  pthread_mutex_lock(&thread_pool_mutex);
  parallel = false;
  pthread_mutex_unlock(&thread_pool_mutex);

  // wake up all threads to finish
  wakeup_thread_all();

  // use a volatile pointer to wait for threads to set their data
  dart_thread_t* volatile *thread_pool_v = (dart_thread_t* volatile *)thread_pool;

  // wait for all threads to finish
  for (int i = 1; i < num_threads; i++) {
    // wait for the thread to populate it's thread data
    // just make sure all threads are awake
    wakeup_thread_all();
    while (thread_pool_v[i] == NULL) {}
    pthread_join(thread_pool_v[i]->pthread, NULL);
  }

  threads_running = false;
}

void dart__tasking__print_stats()
{
  DART_LOG_INFO_ALWAYS("##############################################");
  for (int i = 0; i < num_threads; ++i) {
    if (thread_pool[i]) {
      DART_LOG_INFO("Thread %i executed %lu tasks",
                    i, thread_pool[i]->taskcntr);
    }
  }
  DART_LOG_INFO_ALWAYS("Accumulated matching time:           %lu us",
                       acc_matching_time_us);
  DART_LOG_INFO_ALWAYS("Accumulated worker idle time:        %lu us",
                       acc_idle_time_us);
  DART_LOG_INFO_ALWAYS("Thread 0 idle time:                  %lu us",
                       thread_acc_idle_time_us);
  DART_LOG_INFO_ALWAYS("Accumulated postprocessing time:     %lu us",
                       acc_post_time_us);
  dart__dephash__print_stats(&root_task);
  dart_tasking_remote_print_stats();
  DART_LOG_INFO_ALWAYS("##############################################");
}

static void
destroy_threadpool()
{
  for (int i = 1; i < num_threads; i++) {
    dart_thread_finalize(thread_pool[i]);
  }

  // unset thread-private data
  __tpd = NULL;
  // save the main thread's taskpool
  thread_task_mempool[0] = __taskpool;
  __taskpool = NULL;

  for (int i = 0; i < num_threads; ++i) {
    free(thread_pool[i]);
    thread_pool[i] = NULL;
    // free the task memory pools
    task_mempool_t *tmp = thread_task_mempool[i];
    while (tmp != NULL) {
      task_mempool_t *next = tmp->next;
      free(tmp);
      tmp = next;
    }
  }

  free(thread_pool);
  thread_pool = NULL;
  free(thread_task_mempool);
  thread_task_mempool = NULL;
  dart__tasking__affinity_fini();
}

dart_ret_t
dart__tasking__fini()
{
  if (!initialized) {
    DART_LOG_ERROR("DART tasking subsystem has not been initialized!");
    return DART_ERR_INVAL;
  }

  DART_LOG_DEBUG("dart__tasking__fini(): Tearing down task subsystem");

  if (threads_running) {
    stop_threads();
  }

  dart__tasking__print_stats();

#ifdef DART_ENABLE_AYUDAME
  dart__tasking__ayudame_fini();
#endif // DART_ENABLE_AYUDAME

  free(task_free_lists);
  task_free_lists = NULL;

  dart_tasking_datadeps_reset(&root_task);

  dart_tasking_datadeps_fini();
  dart__tasking__context_cleanup();
  destroy_threadpool();

  for (int i = 0; i < num_numa_nodes; ++i) {
    dart_tasking_taskqueue_finalize(&task_queue[i]);
  }

  dart__task__wait_fini();

  dart_tasking_copyin_fini();

  dart_tasking_tasklist_fini();

  dart__tasking__cancellation_fini();

  initialized = false;
  DART_LOG_DEBUG("dart__tasking__fini(): Finished with tear-down");

  return DART_OK;
}

/**
 * Utility thread functions
 */

typedef struct utility_thread {
  void     (*fn) (void *);
  void      *data;
  pthread_t  pthread;
} utility_thread_t;

static void* utility_thread_main(void *data)
{
  utility_thread_t *ut = (utility_thread_t*)data;
  void     (*fn) (void *) = ut->fn;
  void      *fn_data      = ut->data;

  int thread_id = ++num_utility_threads;
  DART_ASSERT_MSG(DART_TASKING_MAX_UTILITY_THREADS >= thread_id,
                  "Too many utility threads detected (%d), please adjust "
                  "DART_TASKING_MAX_UTILITY_THREADS (%d)",
                  thread_id, DART_TASKING_MAX_UTILITY_THREADS);
  if (bind_threads) {
    if (dart__tasking__affinity_num_cores() > (num_threads + thread_id)) {
      printf("Binding utility thread like a regular thread!\n");
      dart__tasking__affinity_set(ut->pthread, thread_id);
    } else {
      dart__tasking__affinity_set_utility(ut->pthread, -thread_id);
    }
  }

  dart_thread_t *thread = calloc(1, sizeof(*thread));
  dart_thread_init(thread, -thread_id);
  thread->is_utility_thread = true;

  __tpd = thread;

  free(ut);
  ut = NULL;

  //printf("Launching utility thread\n");
  // invoke the utility function
  fn(fn_data);

  free(thread);
  __tpd = NULL;

  // at some point we get back here and exit the thread
  return NULL;
}

void dart__tasking__utility_thread(
  void (*fn) (void *),
  void  *data)
{
  // will be free'd by the thread
  utility_thread_t *ut = malloc(sizeof(*ut));
  ut->fn = fn;
  ut->data = data;
  int ret = pthread_create(&ut->pthread, NULL, &utility_thread_main, ut);
  if (ret != 0) {
    DART_LOG_ERROR("Failed to create utility thread!");
  }
}

#endif // !defined(DART_TASKING_USE_OPENMP)
