#ifndef PSVR2_KERNEL_BOOTSTRAP_INTERNAL_H
#define PSVR2_KERNEL_BOOTSTRAP_INTERNAL_H

#include "psvr2/kernel_exec.h"
#include "psvr2/kernel_payload.h"

#define PSVR2_TEMP_HELPER_SIZE 128U
#define PSVR2_TEMP_FINALIZER_OFFSET UINT64_C(40)

bool psvr2_build_temporary_patch_helper(
    const psvr2_constants *constants, uint64_t address,
    uint64_t mep, uint64_t pte_address,
    uint8_t pte_byte,
    uint8_t out[PSVR2_TEMP_HELPER_SIZE]);
bool psvr2_kernel_probe_slot_is_pristine(psvr2_krw *krw);
bool psvr2_kernel_exact_execution_anchors(psvr2_krw *krw);
bool psvr2_kernel_bootstrap_helpers(psvr2_krw *krw);
bool psvr2_kernel_patch_word_with_helper(
    psvr2_krw *krw, uint64_t target, uint32_t instruction);
bool psvr2_kernel_read_u32_nonfatal(
    psvr2_krw *krw, uint64_t address, uint32_t *value);

#endif
