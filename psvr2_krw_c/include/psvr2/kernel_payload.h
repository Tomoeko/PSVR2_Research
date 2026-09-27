#ifndef PSVR2_KERNEL_PAYLOAD_H
#define PSVR2_KERNEL_PAYLOAD_H

#include "psvr2/rw.h"

#define PSVR2_TEXT_PATCH_PAYLOAD_SIZE 200U
#define PSVR2_TLBI_PAYLOAD_SIZE 136U
#define PSVR2_AUTH_COOKIE_PROBE_SIZE 72U
#define PSVR2_DIRECT_READ_PAYLOAD_SIZE 88U
#define PSVR2_COLD_BOOTSTRAP_PAYLOAD_SIZE 120U
#define PSVR2_COLD_BOOTSTRAP_STAGE_SIZE 512U
#define PSVR2_STR_HELPER_SIZE 16U

size_t psvr2_build_tlbi_payload(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_TLBI_PAYLOAD_SIZE], uint64_t next,
    uint64_t x19, uint64_t x20, uint64_t x21);
size_t psvr2_build_text_patch_payload(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_TEXT_PATCH_PAYLOAD_SIZE],
    uint64_t target, uint32_t instruction,
    const psvr2_registers *registers);
size_t psvr2_build_direct_read_payload(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_DIRECT_READ_PAYLOAD_SIZE],
    uint64_t target, uint64_t helper);
size_t psvr2_build_auth_cookie_probe(
    const psvr2_constants *constants, bool corrupt_cookie,
    uint8_t out[PSVR2_AUTH_COOKIE_PROBE_SIZE]);

bool psvr2_build_patch_helper(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_PATCH_SC_SIZE]);
bool psvr2_build_batch_patch_helper(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_BATCH_PATCH_SC_SIZE]);
bool psvr2_build_direct_read_helper(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_DIRECT_READ_SC_SIZE]);
bool psvr2_build_direct_context_helper(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_DIRECT_CONTEXT_SC_SIZE]);
bool psvr2_build_cold_bootstrap(
    const psvr2_constants *constants,
    uint8_t stage[PSVR2_COLD_BOOTSTRAP_STAGE_SIZE],
    uint8_t payload[PSVR2_COLD_BOOTSTRAP_PAYLOAD_SIZE]);
bool psvr2_build_cold_cleanup_helper(
    const psvr2_constants *constants, uint64_t address,
    uint8_t out[PSVR2_COLD_CLEANUP_SIZE]);
bool psvr2_build_workspace_allocator(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_ALLOC_HELPER_SIZE]);
bool psvr2_build_umh_worker(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_UMH_WORKER_SIZE]);
bool psvr2_build_umh_trigger(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_UMH_TRIGGER_SIZE]);
bool psvr2_build_str_helper(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_STR_HELPER_SIZE]);
bool psvr2_build_tlbi_cleanup(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_TLBI_CLEANUP_SIZE], uint8_t endpoint_state);

#endif
