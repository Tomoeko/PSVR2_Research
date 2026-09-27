#ifndef PSVR2_MODULE_HELPER_H
#define PSVR2_MODULE_HELPER_H

#include "psvr2/runtime.h"

typedef enum {
    PSVR2_RMMOD_HELPER_UNAVAILABLE,
    PSVR2_RMMOD_HELPER_READY,
    PSVR2_RMMOD_HELPER_ERROR
} psvr2_rmmod_helper_state;

psvr2_rmmod_helper_state psvr2_ensure_rmmod_helper(
    psvr2_runtime *runtime);

#endif
