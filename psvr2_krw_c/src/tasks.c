#include "psvr2/tasks.h"
#include "kernel_list_internal.h"

#include <string.h>

enum { PSVR2_TASK_TRAVERSAL_LIMIT = 500 };

typedef struct {
    psvr2_krw *krw;
    const psvr2_firmware_profile *profile;
    psvr2_task_fn fn;
    void *opaque;
    bool read_failed;
} task_walk_context;

static bool visit_task_node(void *opaque, uint64_t list_node) {
    task_walk_context *context = opaque;
    uint64_t task = list_node - context->profile->task_tasks_offset;
    char name[17] = {0};
    if (!psvr2_krw_read(
            context->krw, task + context->profile->task_comm_offset,
            name, sizeof(name) - 1)) {
        context->read_failed = true;
        return false;
    }
    uint64_t mm = psvr2_krw_read_ptr(
        context->krw, task + context->profile->task_mm_offset);
    return context->fn(context->opaque, task, mm, name);
}

psvr2_walk_result psvr2_tasks_foreach_ex(
    psvr2_krw *krw, psvr2_task_fn fn, void *opaque) {
    if (!krw || !krw->ex || !krw->ex->constants || !fn)
        return PSVR2_WALK_INVALID;
    const psvr2_firmware_profile *profile = &krw->ex->constants->fw;
    if (!profile->init_task || !profile->task_tasks_offset ||
        !profile->task_comm_offset || !profile->task_mm_offset)
        return PSVR2_WALK_INVALID;
    task_walk_context context = {
        .krw = krw, .profile = profile, .fn = fn, .opaque = opaque
    };
    uint64_t head = profile->init_task + profile->task_tasks_offset;
    psvr2_walk_result result = psvr2_kernel_list_walk(
        krw, head, head, true, PSVR2_TASK_TRAVERSAL_LIMIT,
        visit_task_node, &context);
    return context.read_failed ? PSVR2_WALK_READ_ERROR : result;
}

bool psvr2_tasks_foreach(psvr2_krw *krw, psvr2_task_fn fn, void *opaque) {
    psvr2_walk_result result =
        psvr2_tasks_foreach_ex(krw, fn, opaque);
    return result == PSVR2_WALK_COMPLETE ||
           result == PSVR2_WALK_CALLBACK_STOPPED;
}

typedef struct {
    const char *wanted;
    uint64_t found;
} find_context;

static bool find_task_cb(void *opaque, uint64_t task,
                         uint64_t mm, const char *name) {
    (void)mm;
    find_context *context = opaque;
    if (strcmp(context->wanted, name) != 0) return true;
    context->found = task;
    return false;
}

uint64_t psvr2_task_find(psvr2_krw *krw, const char *name) {
    if (!name || !*name) return 0;
    find_context context = {.wanted = name};
    return psvr2_tasks_foreach(krw, find_task_cb, &context)
               ? context.found : 0;
}
