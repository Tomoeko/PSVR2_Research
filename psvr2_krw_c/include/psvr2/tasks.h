#ifndef PSVR2_TASKS_H
#define PSVR2_TASKS_H

#include "psvr2/rw.h"

typedef bool (*psvr2_task_fn)(void *opaque, uint64_t task,
                              uint64_t mm, const char *name);

bool psvr2_tasks_foreach(psvr2_krw *krw, psvr2_task_fn fn, void *opaque);
psvr2_walk_result psvr2_tasks_foreach_ex(
    psvr2_krw *krw, psvr2_task_fn fn, void *opaque);
uint64_t psvr2_task_find(psvr2_krw *krw, const char *name);

#endif
