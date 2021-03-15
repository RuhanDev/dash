
// whether to use mmap to allocate stack
//#define USE_MMAP 1
// whether to protect the stack using mprotect
//#define USE_MPROTECT 1

#if defined(USE_MPROTECT) || defined(USE_MMAP)
#define _GNU_SOURCE
#include <sys/mman.h>
#endif

#include <ucontext.h>
#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

#ifdef DART_ENABLE_VALGRIND
#include <valgrind/valgrind.h>
#endif

#include <dash/dart/base/env.h>
#include <dash/dart/base/assert.h>
#include <dash/dart/base/stack.h>
#include <dash/dart/tasking/dart_tasking_priv.h>
#include <dash/dart/tasking/dart_tasking_context.h>

/**
 * Management of task contexts needed for proper yielding of tasks.
 *
 * TODO: make the choice of whether to use mmap automatically
 */

// Use 2M stack size per task
#define DEFAULT_TASK_STACK_SIZE (1<<21)

// the maximum number of ctx to store per thread
#define PER_THREAD_CTX_STORE 10

// we know that the stack member entry is the first element of the struct
// so we can cast directly
#define DART_CTX_ELEM_POP(__freelist) \
  ((context_list_t*)((void*)dart__base__stack_pop(&__freelist)))

#define DART_CTX_ELEM_PUSH(__freelist, __elem) \
  dart__base__stack_push(&__freelist, &DART_STACK_MEMBER_GET(__elem))


struct context_list_s {
  DART_STACK_MEMBER_DEF;
  dart_thread_t         *owner;
  char                  *stack;
#if defined(USE_MMAP)
  size_t                 size;
#endif
#if defined(DART_ENABLE_VALGRIND)
  unsigned               vg_stack_id;
#endif
  context_t              ctx;
};


static size_t task_stack_size = DEFAULT_TASK_STACK_SIZE;
static size_t page_size;

static size_t
dart__tasking__context_pagesize()
{
  long sz = sysconf(_SC_PAGESIZE);
  return sz;
}

static inline size_t
dart__tasking__context_adjust_size(size_t size)
{
  size_t mask = page_size - 1;
  return (size + mask) & ~mask;
}

size_t
dart__tasking__context_stack_size()
{
  return task_stack_size;
}

void dart__tasking__context_init()
{
  page_size = dart__tasking__context_pagesize();
  ssize_t env_stack_size = dart__base__env__size(DART_TASKSTACKSIZE_ENVSTR, -1);
  if (env_stack_size > -1) {
    DART_LOG_INFO("Using user-provided task stack size of %zu", task_stack_size);
    task_stack_size = env_stack_size;
  } else {
    DART_LOG_INFO("Using default task stack size of %zu", task_stack_size);
  }
  if (task_stack_size < page_size) {
    DART_LOG_INFO("Rounding up task stack size to page size (%zu)", page_size);
    task_stack_size = page_size;
  }
}

#ifdef USE_UCONTEXT
static void
dart__tasking__context_entry(void)
{
  dart_thread_t  *thread  = dart__tasking__current_thread();
  context_list_t *ctxlist = thread->ctx_to_enter;
  thread->ctx_to_enter    = NULL;
  DART_ASSERT(ctxlist != NULL);

  context_func_t *fn  = ctxlist->ctx.fn;
  void           *arg = ctxlist->ctx.arg;
  ctxlist->ctx.fn = ctxlist->ctx.arg = NULL;

  // invoke function
  fn(arg);

  // fn should never return!
  DART_ASSERT_MSG(NULL, "task context invocation function returned!");
}
#endif // USE_UCONTEXT

static context_list_t *
dart__tasking__context_allocate()
{
  size_t task_stack_size = dart__tasking__context_stack_size();
  // align to page boundary: first page contains struct data and pointer to
  //                         second page, the start of the stack
  size_t meta_size = dart__tasking__context_adjust_size(sizeof(context_list_t));
#ifdef USE_MPROTECT
  size_t size = meta_size
              + dart__tasking__context_adjust_size(task_stack_size)
              + 2 * page_size; // upper and lower guard pages
#else
  size_t size = meta_size
              + dart__tasking__context_adjust_size(task_stack_size);
#endif
  // TODO: allocate guard pages and mprotect() them?
#ifdef USE_MMAP
  context_list_t *ctxlist = mmap(NULL, size,
                                 PROT_EXEC|PROT_READ|PROT_WRITE,
                                 MAP_PRIVATE|MAP_ANONYMOUS|MAP_STACK,
                                 -1, 0);
  DART_ASSERT_MSG(ctxlist != MAP_FAILED, "Failed to mmap new stack!");
  ctxlist->size  = size;
#else
  context_list_t *ctxlist;
  DART_ASSERT_RETURNS(posix_memalign((void**)&ctxlist, page_size, size), 0);
#endif

  ctxlist->owner = dart__tasking__current_thread();

#ifdef USE_MPROTECT
  ctxlist->stack    = ((char*)ctxlist) + meta_size + page_size;
#else
  ctxlist->stack    = ((char*)ctxlist) + meta_size;
#endif

  DART_LOG_TRACE("Allocated context %p (sp:%p)",
                 &ctxlist->ctx, ctxlist->stack);

#ifdef USE_MPROTECT
  void *ub_guard = ctxlist->stack + task_stack_size;
  // mprotect upper guard page
  if (mprotect(ub_guard, page_size, PROT_NONE) != 0) {
    DART_LOG_WARN("Failed(%d) to mprotect upper guard page of size %zu at %p: %s",
                  errno, page_size, ub_guard, strerror(errno));
  }
  void *lb_guard = ctxlist->stack - page_size;
  // mprotect lower guard page
  if (mprotect(lb_guard, page_size, PROT_NONE) != 0) {
    DART_LOG_WARN("Failed(%d) to mprotect lower guard page of size %zu at %p: %s",
                  errno, page_size, lb_guard, strerror(errno));
  }
#endif

#ifdef DART_ENABLE_VALGRIND
  ctxlist->vg_stack_id = VALGRIND_STACK_REGISTER(ctxlist->stack,
                                                 ctxlist->stack + task_stack_size);
#endif
  return ctxlist;
}

static void
dart__tasking__context_free(context_list_t *ctxlist)
{
#ifdef USE_MPROTECT
  void *ub_guard = ctxlist->stack + task_stack_size;
  if (mprotect(ub_guard, page_size, PROT_READ|PROT_EXEC|PROT_WRITE) != 0) {
    DART_LOG_WARN("Failed(%d) to mprotect upper guard page of size %zu at %p: %s",
                  errno, page_size, ub_guard, strerror(errno));
  }

  void *lb_guard = ctxlist->stack - page_size;
  if (mprotect(lb_guard, page_size, PROT_READ|PROT_EXEC|PROT_WRITE) != 0) {
    DART_LOG_WARN("Failed(%d) to mprotect lower guard page of size %zu at %p: %s",
                  errno, page_size, lb_guard, strerror(errno));
  }
#endif
#ifdef DART_ENABLE_VALGRIND
  VALGRIND_STACK_DEREGISTER(ctxlist->vg_stack_id);
#endif
#ifdef USE_MMAP
  munmap(ctxlist, ctxlist->size);
#else
  free(ctxlist);
#endif
}

context_t* dart__tasking__context_create(context_func_t *fn, void *arg)
{
  dart_thread_t* thread = dart__tasking__current_thread();
  context_list_t *ctxlist = (DART_CTX_ELEM_POP(thread->ctxlist));
  context_t* res = NULL;
#ifdef USE_UCONTEXT

  // look for already allocated contexts
  // thread-local list, no locking required
  if (ctxlist != NULL) {
    res = &ctxlist->ctx;
    DART_LOG_TRACE("Reusing context %p (sp: %p)",
                   &res->ctx, res->ctx.uc_stack.ss_sp);
  } else  {
    // allocate a new context
    context_list_t *ctxlist = dart__tasking__context_allocate();
    // initialize context and set stack
    getcontext(&ctxlist->ctx.ctx);
    ctxlist->ctx.ctx.uc_link           = NULL;
    ctxlist->ctx.ctx.uc_stack.ss_sp    = ctxlist->stack;
    ctxlist->ctx.ctx.uc_stack.ss_size  = task_stack_size;
    ctxlist->ctx.ctx.uc_stack.ss_flags = 0;
    res = &ctxlist->ctx;
    DART_LOG_TRACE("Created new context %p (sp:%p)",
                  &res->ctx, res->ctx.uc_stack.ss_sp);
  }

  DART_ASSERT(res->ctx.uc_stack.ss_sp != NULL);

#ifdef DART_DEBUG
  // set the stack guards
  char *stack = (char*)res->ctx.uc_stack.ss_sp;
  *(uint64_t*)(stack) = 0xDEADBEEF;
  *(uint64_t*)(stack + task_stack_size - sizeof(uint64_t)) = 0xDEADBEEF;
#endif // DART_DEBUG

  makecontext(&res->ctx, &dart__tasking__context_entry, 0, 0);
  res->fn  = fn;
  res->arg = arg;

#else // USE_UCONTEXT
  res = malloc(sizeof(*res));
#endif // USE_UCONTEXT
  return res;
}

void dart__tasking__context_release(context_t* ctx)
{
#ifdef USE_UCONTEXT
  DART_LOG_TRACE("Releasing context %p (sp:%p)",
                 &ctx->ctx, ctx->ctx.uc_stack.ss_sp);
  DART_ASSERT(ctx->ctx.uc_stack.ss_sp != NULL);

#ifdef DART_DEBUG
  // check the stack guards
  char *stack = (char*)ctx->ctx.uc_stack.ss_sp;
  if (*(uint64_t*)(stack) != 0xDEADBEEF &&
      *(uint64_t*)(stack + task_stack_size - sizeof(uint64_t)) != 0xDEADBEEF)
  {
    DART_LOG_WARN(
        "Possible TASK STACK OVERFLOW detected! "
        "Consider changing the stack size via DART_TASK_STACKSIZE! "
        "(current stack size: %zu)", task_stack_size);
  }
#endif // DART_DEBUG

  // thread-local list, no locking required
  context_list_t *ctxlist = (context_list_t*)
                                (((char*)ctx) - offsetof(context_list_t, ctx));

  // push back to the thread that allocated the context
  dart_thread_t* thread = ctxlist->owner;
  DART_CTX_ELEM_PUSH(thread->ctxlist, ctxlist);
#else
  free(ctx);
#endif
}

void dart__tasking__context_invoke(context_t* ctx)
{
#ifdef USE_UCONTEXT
  // first invocation --> prepend to thread's ctxlist
  if (ctx->fn) {
    dart_thread_t  *thread  = dart__tasking__current_thread();
    context_list_t *ctxlist = (context_list_t*)
                                (((char*)ctx) - offsetof(context_list_t, ctx));
    // save the context here so we can access it in dart__tasking__context_entry
    thread->ctx_to_enter = ctxlist;
    DART_ASSERT(ctx->ctx.uc_stack.ss_sp != NULL);
  }

  DART_LOG_TRACE("Invoking context %p (sp:%p)",
                 &ctx->ctx, ctx->ctx.uc_stack.ss_sp);
  setcontext(&ctx->ctx);
#else
  DART_ASSERT_MSG(NULL, "Cannot call %s without UCONTEXT support!", __FUNCTION__);
#endif
}

dart_ret_t
dart__tasking__context_swap(context_t* old_ctx, context_t* new_ctx)
{
#ifdef USE_UCONTEXT
  // first invocation --> prepend to thread's ctxlist
  if (new_ctx->fn) {
    dart_thread_t  *thread  = dart__tasking__current_thread();
    context_list_t *ctxlist = (context_list_t*)
                             (((char*)new_ctx) - offsetof(context_list_t, ctx));
    // save the context here so we can access it in dart__tasking__context_entry
    thread->ctx_to_enter = ctxlist;
  }

  if (old_ctx->fn) {
    // make sure we do not call the entry function upon next swap
    old_ctx->fn = old_ctx->arg = NULL;
  }

  DART_LOG_TRACE("Swapping context %p (sp:%p) -> %p (sp:%p)",
                 &old_ctx->ctx, old_ctx->ctx.uc_stack.ss_sp,
                 &new_ctx->ctx, new_ctx->ctx.uc_stack.ss_sp);
  int ret = swapcontext(&old_ctx->ctx, &new_ctx->ctx);
  if (ret == -1) {
    DART_LOG_ERROR("Call to swapcontext failed! (errno=%i)", errno);
    return DART_ERR_OTHER;
  } else {
    return DART_OK;
  }
#else
  DART_ASSERT_MSG(NULL, "Cannot call %s without UCONTEXT support!", __FUNCTION__);
#endif
}

void dart__tasking__context_cleanup()
{
  dart_thread_t* thread = dart__tasking__current_thread();

  context_list_t *ctxlist;
  while (NULL != (ctxlist = DART_CTX_ELEM_POP(thread->ctxlist))) {
    dart__tasking__context_free(ctxlist);
  }
}
