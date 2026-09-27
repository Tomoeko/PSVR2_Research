#ifndef PSVR2_RUNTIME_H
#define PSVR2_RUNTIME_H

#include "psvr2/vfs.h"

typedef struct psvr2_stage1_mailbox {
    uint16_t firmware;
    uint64_t status;
    uint64_t cmd_in;
    uint64_t cmd_in_len;
    uint64_t cmd_out;
    uint64_t cmd_out_len;
    uint64_t cmd_seq;
    uint64_t cmd_ret;
    bool valid;
} psvr2_stage1_mailbox;

typedef struct psvr2_umh_state {
    bool helpers_injected;
    bool static_data_initialized;
    uint64_t output_dentry;
} psvr2_umh_state;

typedef enum {
    PSVR2_RUNTIME_UNINITIALIZED,
    PSVR2_RUNTIME_KERNEL_READY,
    PSVR2_RUNTIME_HELPERS_READY,
    PSVR2_RUNTIME_WORKSPACE_READY,
    PSVR2_RUNTIME_STAGE1_READY
} psvr2_runtime_phase;

typedef struct psvr2_runtime {
    psvr2_krw *krw;
    psvr2_runtime_phase phase;
    uint64_t connection_generation;
    bool workspace_allocator_injected;
    bool stage1_loaded;
    bool rmmod_helper_missed_stage1;
    psvr2_umh_state umh;
    psvr2_stage1_mailbox mailbox;
} psvr2_runtime;

void psvr2_runtime_init(psvr2_runtime *runtime, psvr2_krw *krw);
void psvr2_runtime_reset_after_reconnect(psvr2_runtime *runtime);
bool psvr2_runtime_sync_connection(psvr2_runtime *runtime);
void psvr2_runtime_mark_phase(
    psvr2_runtime *runtime, psvr2_runtime_phase phase);

#endif
