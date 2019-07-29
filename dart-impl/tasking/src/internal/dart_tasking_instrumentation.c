#include <dash/dart/tasking/dart_tasking_instrumentation.h>
#include <stdio.h>
//#include <dash/dart/tasking/dart_tasking_priv.h>

struct dart_tool_task_create_cb {
    dart_tool_task_create_cb_t cb;
    void *userdata;
};
struct dart_tool_task_begin_cb {
    dart_tool_task_begin_cb_t cb;
    void *userdata;
};
struct dart_tool_task_end_cb {
    dart_tool_task_end_cb_t cb;
    void *userdata;
};
struct dart_tool_task_cancel_cb {
    dart_tool_task_cancel_cb_t cb;
    void *userdata;
};
struct dart_tool_task_yield_leave_cb {
    dart_tool_task_yield_leave_cb_t cb;
    void *userdata;
};
struct dart_tool_task_yield_resume_cb {
    dart_tool_task_yield_resume_cb_t cb;
    void *userdata;
};

struct dart_tool_task_finalize_cb {
    dart_tool_task_finalize_cb_t cb;
    void *userdata;
};

struct dart_tool_task_add_to_queue_cb {
    dart_tool_task_add_to_queue_cb_t cb;
    void *userdata;
};

struct dart_tool_local_dep_raw_cb {
    dart_tool_local_dep_raw_cb_t cb;
    void *userdata;
};

struct dart_tool_local_dep_waw_cb {
    dart_tool_local_dep_waw_cb_t cb;
    void *userdata;
};

struct dart_tool_local_dep_war_cb {
    dart_tool_local_dep_war_cb_t cb;
    void *userdata;
};

struct dart_tool_dummy_dep_create_cb {
    dart_tool_dummy_dep_create_cb_t cb;
    void *userdata;
};
struct dart_tool_dummy_dep_capture_cb {
    dart_tool_dummy_dep_capture_cb_t cb;
    void *userdata;
};

struct dart_tool_remote_in_dep_cb {
    dart_tool_remote_in_dep_cb_t cb;
    void *userdata;
};

typedef struct dart_tool_task_create_cb dart_tool_task_create_cb;
typedef struct dart_tool_task_begin_cb dart_tool_task_begin_cb;
typedef struct dart_tool_task_cancel_cb dart_tool_task_cancel_cb;
typedef struct dart_tool_task_yield_leave_cb dart_task_yield_leave_cb;
typedef struct dart_tool_task_yield_resume_cb dart_task_yield_resume_cb;
typedef struct dart_tool_task_finalize_cb dart_tool_task_finalize_cb;
typedef struct dart_tool_task_add_to_queue_cb dart_tool_task_add_to_queue_cb;

typedef struct dart_tool_local_dep_raw_cb dart_tool_local_dep_raw_cb;
typedef struct dart_tool_local_dep_waw_cb dart_tool_local_dep_waw_cb;
typedef struct dart_tool_local_dep_war_cb dart_tool_local_dep_war_cb;

typedef struct dart_tool_dummy_dep_create_cb dart_tool_dummy_dep_create_cb;
typedef struct dart_tool_dummy_dep_capture_cb dart_tool_dummy_dep_capture_cb;

typedef struct dart_tool_remote_in_dep_cb dart_tool_remote_in_dep_cb;
/* data structures to save the function pointer and user data from the callback */
struct dart_tool_task_create_cb dart_tool_task_create_cb_data;
struct dart_tool_task_begin_cb dart_tool_task_begin_cb_data;
struct dart_tool_task_end_cb dart_tool_task_end_cb_data;
struct dart_tool_task_finalize_cb dart_tool_task_finalize_cb_data;
struct dart_tool_task_add_to_queue_cb dart_tool_task_add_to_queue_cb_data;

struct dart_tool_local_dep_raw_cb dart_tool_local_dep_raw_cb_data;
struct dart_tool_local_dep_waw_cb dart_tool_local_dep_waw_cb_data;
struct dart_tool_local_dep_war_cb dart_tool_local_dep_war_cb_data;

struct dart_tool_dummy_dep_create_cb dart_tool_dummy_dep_create_cb_data;
struct dart_tool_dummy_dep_capture_cb dart_tool_dummy_dep_capture_cb_data;

struct dart_tool_remote_in_dep_cb dart_tool_remote_in_dep_cb_data;

/* implementation of the register function of the callback */
int dart_tool_register_task_create (dart_tool_task_create_cb_t cb, void *userdata_task_create) {
   dart_tool_task_create_cb_data.cb = cb;
   dart_tool_task_create_cb_data.userdata = userdata_task_create;
   printf("dart_tool_register_task_create was called\nPointer: %p and userdata %d\n", cb, *(int *) userdata_task_create);
   return 0;
} 

int dart_tool_register_task_begin(dart_tool_task_begin_cb_t cb, void *userdata_task_begin) {
    dart_tool_task_begin_cb_data.cb = cb;
    dart_tool_task_begin_cb_data.userdata = userdata_task_begin;
    printf("dart_tool_register_task_begin was called\nPointer: %p and userdata %d\n", cb, *(int *) userdata_task_begin);
    return 0;
}

int dart_tool_register_task_end (dart_tool_task_end_cb_t cb, void *userdata_task_end) {
    dart_tool_task_end_cb_data.cb = cb;
    dart_tool_task_end_cb_data.userdata = userdata_task_end;
    printf("dart_tool_register_task_end was called\nPointer: %p and userdata %d\n", cb, *(int *) userdata_task_end);
    return 0;
}

int dart_tool_register_task_finalize(dart_tool_task_finalize_cb_t cb, void *userdata_task_all_end) {
    dart_tool_task_finalize_cb_data.cb = cb;
    dart_tool_task_finalize_cb_data.userdata = userdata_task_all_end;
    printf("dart_tool_register_task_finalize was called\nPointer: %p and userdata %d\n", cb, *(int*) userdata_task_all_end);
    return 0;
}
/* missing task_yield register functions yet*/


int dart_tool_register_task_add_to_queue (dart_tool_task_add_to_queue_cb_t cb, void *userdata_queue) {
    dart_tool_task_add_to_queue_cb_data.cb = cb;
    dart_tool_task_add_to_queue_cb_data.userdata = userdata_queue;
    printf("dart_tool_register_task_add_to_queue was called\nPointer: %p and userdata %d\n", cb, *(int*) userdata_queue);
    return 0;
}
int dart_tool_register_local_dep_raw (dart_tool_local_dep_raw_cb_t cb, void *userdata_local_dep_raw) {
    dart_tool_local_dep_raw_cb_data.cb = cb;
    dart_tool_local_dep_raw_cb_data.userdata = userdata_local_dep_raw;
    printf("dart_tool_register_local_dep_raw was called\nPointer: %p and userdata %d\n", cb, *(int*) userdata_local_dep_raw);
    return 0;
}

int dart_tool_register_local_dep_waw (dart_tool_local_dep_waw_cb_t cb, void *userdata_local_dep_waw) {
    dart_tool_local_dep_waw_cb_data.cb = cb;
    dart_tool_local_dep_waw_cb_data.userdata = userdata_local_dep_waw;
    printf("dart_tool_register_local_dep_waw was called\nPointer: %p and userdata %d\n", cb, *(int*) userdata_local_dep_waw);
    return 0;
}

int dart_tool_register_local_dep_war (dart_tool_local_dep_war_cb_t cb, void *userdata_local_dep_war) {
    dart_tool_local_dep_war_cb_data.cb = cb;
    dart_tool_local_dep_war_cb_data.userdata = userdata_local_dep_war;
    printf("dart_tool_register_local_dep_war was called\nPointer: %p and userdata %d\n", cb, *(int*) userdata_local_dep_war);
    return 0;
}

int dart_tool_register_dummy_dep_create(dart_tool_dummy_dep_create_cb_t cb, void *userdata_dummy_dep_create) {
    dart_tool_dummy_dep_create_cb_data.cb = cb;
    dart_tool_dummy_dep_create_cb_data.userdata = userdata_dummy_dep_create;
    printf("dart_tool_register_dummy_dep_create was called\nPointer: %p and userdata %d\n", cb, *(int*) userdata_dummy_dep_create);
    return 0;
}

int dart_tool_register_dummy_dep_capture(dart_tool_dummy_dep_capture_cb_t cb, void *userdata_dummy_dep_capture) {
    dart_tool_dummy_dep_capture_cb_data.cb = cb;
    dart_tool_dummy_dep_capture_cb_data.userdata = userdata_dummy_dep_capture;
    printf("dart_tool_register_dummy_dep_capture was called\nPointer: %p and userdata %d\n", cb, *(int*) userdata_dummy_dep_capture);
    return 0;
}

int dart_tool_register_remote_in_dep (dart_tool_remote_in_dep_cb_t cb, void *userdata_remote_in_dep) {
    dart_tool_remote_in_dep_cb_data.cb = cb;
    dart_tool_remote_in_dep_cb_data.userdata = userdata_remote_in_dep;
    printf("dart_tool_register_remote_in_dep was called\nPointer: %p and userdata %d\n", cb, *(int*) userdata_remote_in_dep);
    return 0;
}

void dart__tasking__instrument_task_create(
  dart_task_t      *task,
  dart_task_prio_t  prio,
  const char       *name,
  int32_t task_unitid)
{
    if (!name) {
        name = "";
    }
    if (dart_tool_task_create_cb_data.cb) {
          dart_tool_task_create_cb_data.cb((uint64_t) task, prio, name, task_unitid, dart_tool_task_create_cb_data.userdata);

    }
}

void dart__tasking__instrument_task_begin(
  dart_task_t   *task,
  dart_thread_t *thread,
  int32_t task_unitid)
{
    if (dart_tool_task_begin_cb_data.cb) {
        dart_tool_task_begin_cb_data.cb((uint64_t) task, (uint64_t) thread, task_unitid, dart_tool_task_begin_cb_data.userdata);
    }
  // TODO
}

void dart__tasking__instrument_task_end(
  dart_task_t   *task,
  dart_thread_t *thread,
  int32_t task_unitid)
{
    if (dart_tool_task_end_cb_data.cb) {
        dart_tool_task_end_cb_data.cb((uint64_t) task, (uint64_t) thread, task_unitid, dart_tool_task_end_cb_data.userdata);
    }
  // TODO
}

void dart__tasking__instrument_task_cancel(
  dart_task_t   *task,
  dart_thread_t *thread)
{
  //printf("DASH: task cancel\n");
  // TODO
}

void dart__tasking__instrument_task_yield_leave(
  dart_task_t   *task,
  dart_thread_t *thread)
{
  //printf("DASH: task yield leave\n");
  // TODO
}


void dart__tasking__instrument_task_yield_resume(
  dart_task_t   *task,
  dart_thread_t *thread)
{
  //printf("DASH: task yield resume\n");
  // TODO
}

void dart__tasking__instrument_task_finalize()
{
    if (dart_tool_task_finalize_cb_data.cb) {
        dart_tool_task_finalize_cb_data.cb(dart_tool_task_finalize_cb_data.userdata);
    }
}

void dart__tasking__instrument_local_dep_raw(
    dart_task_t *task1,
    dart_task_t *task2,
    uint64_t memaddr_raw,
    uint64_t orig_memaddr_raw,
    int32_t task1_unitid,
    int32_t task2_unitid)
{
    //uint64_t memaddr_raw = 0;
    //uint64_t orig_memaddr_raw = 1;
    if (dart_tool_local_dep_raw_cb_data.cb) {
        dart_tool_local_dep_raw_cb_data.cb((uint64_t) task1, (uint64_t) task2, memaddr_raw, orig_memaddr_raw, task1_unitid, task2_unitid, dart_tool_local_dep_raw_cb_data.userdata);
    }
    
}

void dart__tasking__instrument_local_dep_waw(
    dart_task_t *task1,
    dart_task_t *task2,
    uint64_t memaddr_waw,
    uint64_t orig_memaddr_waw,
    int32_t task1_unitid,
    int32_t task2_unitid)
{
    //uint64_t memaddr_waw = 0;
    //uint64_t orig_memaddr_waw = 1;
    if (dart_tool_local_dep_waw_cb_data.cb) {
        dart_tool_local_dep_waw_cb_data.cb((uint64_t) task1, (uint64_t) task2, memaddr_waw, orig_memaddr_waw, task1_unitid, task2_unitid, dart_tool_local_dep_waw_cb_data.userdata);
    }
    
}

void dart__tasking__instrument_local_dep_war(
    dart_task_t *task1,
    dart_task_t *task2,
    uint64_t memaddr_war,
    uint64_t orig_memaddr_war,
    int32_t task1_unitid,
    int32_t task2_unitid)
{
    //offset des gptr
    //uint64_t memaddr_war = 0;
    //uint64_t orig_memaddr_war = 1;
    if (dart_tool_local_dep_war_cb_data.cb) {
        dart_tool_local_dep_war_cb_data.cb((uint64_t) task1, (uint64_t) task2, memaddr_war, orig_memaddr_war, task1_unitid, task2_unitid,dart_tool_local_dep_war_cb_data.userdata);
    }
    
}
void dart__tasking__instrument_task_add_to_queue(
    dart_task_t *task,
    dart_thread_t *thread,
    int32_t task_unitid)
{
    if (dart_tool_task_add_to_queue_cb_data.cb) {
        dart_tool_task_add_to_queue_cb_data.cb((uint64_t) task, (uint64_t) thread, task_unitid, dart_tool_task_add_to_queue_cb_data.userdata);
    }
    
}
void dart__tasking__instrument_dummy_dep_create(
    dart_task_t *task,
    uint64_t dummy_dep,
    uint64_t in_dep,
    dart_task_dep_t out_dep,
    int32_t task_unitid)
{
    if (dart_tool_dummy_dep_create_cb_data.cb) {
        dart_tool_dummy_dep_create_cb_data.cb((uint64_t) task, dummy_dep, in_dep, out_dep.phase, task_unitid, dart_tool_dummy_dep_create_cb_data.userdata);
    }
    
}

void dart__tasking__instrument_dummy_dep_capture (
    dart_task_t *task,
    uint64_t dummy_dep,
    uint64_t remote_dep,
    int32_t task_unitid)
{
    if (dart_tool_dummy_dep_capture_cb_data.cb) {
        dart_tool_dummy_dep_capture_cb_data.cb((uint64_t) task, dummy_dep, remote_dep, task_unitid, dart_tool_dummy_dep_create_cb_data.userdata);
    }
    
}

void dart__tasking__instrument_remote_in_dep(
    uint64_t local_task,
    uint64_t remote_task,
    int local_dep_type,
    int remote_dep_type,
    int32_t local_unitid,
    int32_t remote_unitid)
{
    if (dart_tool_remote_in_dep_cb_data.cb) {
        dart_tool_remote_in_dep_cb_data.cb(local_task, remote_task, local_dep_type,remote_dep_type, local_unitid, remote_unitid, dart_tool_remote_in_dep_cb_data.userdata);
    }
    
}
