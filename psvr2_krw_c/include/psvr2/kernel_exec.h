#ifndef PSVR2_KERNEL_EXEC_H
#define PSVR2_KERNEL_EXEC_H

#include "psvr2/memory.h"

typedef struct {
    uint64_t target;
    const void *data;
    size_t length;
} psvr2_kernel_text_patch_request;

/*
 * A caller-selected firmware profile is intentionally execution-disabled
 * until bounded, read-only checks match the live kernel to the exact image.
 */
bool psvr2_kernel_certify_live_profile(psvr2_krw *krw);

bool psvr2_kernel_text_patch_u32(
    psvr2_krw *krw, uint64_t target, uint32_t instruction);
bool psvr2_kernel_text_patch(
    psvr2_krw *krw, uint64_t target,
    const void *data, size_t length);
bool psvr2_kernel_text_patch_many(
    psvr2_krw *krw,
    const psvr2_kernel_text_patch_request *requests,
    size_t request_count);

bool psvr2_kernel_workspace_prepare(psvr2_runtime *runtime);
bool psvr2_kernel_force_restart(psvr2_runtime *runtime);
bool psvr2_kernel_force_power_off(psvr2_runtime *runtime);

#endif
