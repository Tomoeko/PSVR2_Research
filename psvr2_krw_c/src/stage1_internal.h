#ifndef PSVR2_STAGE1_INTERNAL_H
#define PSVR2_STAGE1_INTERNAL_H

#include "psvr2/memory.h"
#include "psvr2/stage1.h"

#define PSVR2_STAGE1_RW_SCAN_SIZE 0x1000U
#define PSVR2_STAGE1_STATUS_SIZE 256U
#define PSVR2_STAGE1_MAILBOX_MAGIC UINT64_C(0x3158424d31565350)
#define PSVR2_STAGE1_MAILBOX_ABI 1U
#define PSVR2_STAGE1_MAILBOX_FIRMWARE_0110 0x0110U
#define PSVR2_STAGE1_MAILBOX_FIRMWARE_0600 0x0600U
#define PSVR2_STAGE1_MAILBOX_DESCRIPTOR_SIZE 88U
#define PSVR2_STAGE1_MAILBOX_STATUS_OFFSET 32U
#define PSVR2_STAGE1_MAILBOX_CMD_IN_OFFSET 40U
#define PSVR2_STAGE1_MAILBOX_CMD_IN_LEN_OFFSET 48U
#define PSVR2_STAGE1_MAILBOX_CMD_OUT_OFFSET 56U
#define PSVR2_STAGE1_MAILBOX_CMD_OUT_LEN_OFFSET 64U
#define PSVR2_STAGE1_MAILBOX_CMD_SEQ_OFFSET 72U
#define PSVR2_STAGE1_MAILBOX_CMD_RET_OFFSET 80U
#define PSVR2_STAGE1_COMMAND_PREFIX \
    "export PATH=/tmp/bin:/tmp:$PATH; "

enum {
    PSVR2_STAGE1_COMMAND_CAPACITY = 511,
    PSVR2_STAGE1_OUTPUT_LIMIT = 65535
};

bool psvr2_stage1_mailbox_from_module_data(
    const psvr2_module_layout *layout, uint64_t writable_start,
    const uint8_t *data, size_t length, uint16_t expected_firmware,
    psvr2_stage1_mailbox *mailbox);
bool psvr2_stage1_discover_from_module(
    psvr2_runtime *runtime, bool *layout_ready);

#endif
