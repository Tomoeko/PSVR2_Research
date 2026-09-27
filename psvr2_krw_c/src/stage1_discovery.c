#include "psvr2/shellcode.h"

#include "stage1_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

bool psvr2_parse_stage1_mailbox(const char *status,
                                psvr2_stage1_mailbox *box) {
    if (!status || !box) return false;
    struct item {
        const char *key;
        uint64_t *value;
    };
    memset(box, 0, sizeof(*box));
    struct item items[] = {
        {"status=", &box->status},
        {"cmd_in=", &box->cmd_in},
        {"cmd_in_len=", &box->cmd_in_len},
        {"cmd_out=", &box->cmd_out},
        {"cmd_out_len=", &box->cmd_out_len},
        {"cmd_seq=", &box->cmd_seq},
        {"cmd_ret=", &box->cmd_ret}
    };
    for (unsigned i = 0; i < PSVR2_ARRAY_LEN(items); ++i) {
        const char *text = strstr(status, items[i].key);
        if (!text) return false;
        text += strlen(items[i].key);

        char value[32];
        size_t length = 0;
        while (isxdigit((unsigned char)text[length]) ||
               (length == 1 && text[0] == '0' &&
                (text[1] == 'x' || text[1] == 'X'))) {
            if (length + 1 >= sizeof(value)) return false;
            value[length] = text[length];
            ++length;
        }
        value[length] = '\0';
        errno = 0;
        char *end = NULL;
        unsigned long long parsed = strtoull(value, &end, 16);
        if (errno || end == value || !end || *end != '\0') return false;
        *items[i].value = (uint64_t)parsed;
    }
    box->valid = true;
    return true;
}

static bool module_writable_range_contains(
    const psvr2_module_layout *layout,
    uint64_t address, size_t length) {
    return address >= layout->base + layout->ro_size &&
           length <= layout->size &&
           address - layout->base <= layout->size - length;
}

static bool mailbox_within_module(
    const psvr2_module_layout *layout,
    const psvr2_stage1_mailbox *mailbox) {
    bool valid = mailbox->valid &&
           module_writable_range_contains(
               layout, mailbox->cmd_in,
               PSVR2_STAGE1_COMMAND_CAPACITY + 1U) &&
           module_writable_range_contains(
               layout, mailbox->cmd_in_len, sizeof(uint64_t)) &&
           module_writable_range_contains(
               layout, mailbox->cmd_out,
               PSVR2_STAGE1_OUTPUT_LIMIT + 1U) &&
           module_writable_range_contains(
               layout, mailbox->cmd_out_len, sizeof(uint64_t)) &&
           module_writable_range_contains(
               layout, mailbox->cmd_seq, sizeof(uint64_t)) &&
           module_writable_range_contains(
               layout, mailbox->status, PSVR2_STAGE1_STATUS_SIZE) &&
           module_writable_range_contains(
               layout, mailbox->cmd_ret, sizeof(uint64_t)) &&
           !(mailbox->cmd_in_len % sizeof(uint64_t)) &&
           !(mailbox->cmd_out_len % sizeof(uint64_t)) &&
           !(mailbox->cmd_seq % sizeof(uint64_t)) &&
           !(mailbox->cmd_ret % sizeof(uint64_t));
    if (!valid) return false;
    const struct {
        uint64_t address;
        size_t length;
    } ranges[] = {
        {mailbox->status, PSVR2_STAGE1_STATUS_SIZE},
        {mailbox->cmd_in, PSVR2_STAGE1_COMMAND_CAPACITY + 1U},
        {mailbox->cmd_in_len, sizeof(uint64_t)},
        {mailbox->cmd_out, PSVR2_STAGE1_OUTPUT_LIMIT + 1U},
        {mailbox->cmd_out_len, sizeof(uint64_t)},
        {mailbox->cmd_seq, sizeof(uint64_t)},
        {mailbox->cmd_ret, sizeof(uint64_t)}
    };
    /* Range checks above prove each end is representable. These distinct
     * variables must not alias a command buffer or another control field. */
    for (size_t left = 0; left < PSVR2_ARRAY_LEN(ranges); ++left)
        for (size_t right = left + 1U; right < PSVR2_ARRAY_LEN(ranges); ++right)
            if (ranges[left].address < ranges[right].address + ranges[right].length &&
                ranges[right].address < ranges[left].address + ranges[left].length)
                return false;
    return true;
}

static bool mailbox_descriptor_from_data(
    const psvr2_module_layout *layout, const uint8_t *data,
    size_t offset, size_t length, uint16_t expected_firmware,
    psvr2_stage1_mailbox *mailbox) {
    if (length - offset < PSVR2_STAGE1_MAILBOX_DESCRIPTOR_SIZE ||
        psvr2_load_le64(data + offset) !=
            PSVR2_STAGE1_MAILBOX_MAGIC ||
        psvr2_load_le16(data + offset + 8U) !=
            PSVR2_STAGE1_MAILBOX_ABI ||
        psvr2_load_le16(data + offset + 10U) !=
            expected_firmware ||
        psvr2_load_le32(data + offset + 12U) !=
            PSVR2_STAGE1_MAILBOX_DESCRIPTOR_SIZE ||
        psvr2_load_le32(data + offset + 16U) !=
            PSVR2_STAGE1_STATUS_SIZE ||
        psvr2_load_le32(data + offset + 20U) !=
            PSVR2_STAGE1_COMMAND_CAPACITY + 1U ||
        psvr2_load_le32(data + offset + 24U) !=
            PSVR2_STAGE1_OUTPUT_LIMIT + 1U ||
        psvr2_load_le32(data + offset + 28U) != 0U)
        return false;

    *mailbox = (psvr2_stage1_mailbox){
        .firmware = expected_firmware,
        .status = psvr2_load_le64(
            data + offset + PSVR2_STAGE1_MAILBOX_STATUS_OFFSET),
        .cmd_in = psvr2_load_le64(
            data + offset + PSVR2_STAGE1_MAILBOX_CMD_IN_OFFSET),
        .cmd_in_len = psvr2_load_le64(
            data + offset + PSVR2_STAGE1_MAILBOX_CMD_IN_LEN_OFFSET),
        .cmd_out = psvr2_load_le64(
            data + offset + PSVR2_STAGE1_MAILBOX_CMD_OUT_OFFSET),
        .cmd_out_len = psvr2_load_le64(
            data + offset + PSVR2_STAGE1_MAILBOX_CMD_OUT_LEN_OFFSET),
        .cmd_seq = psvr2_load_le64(
            data + offset + PSVR2_STAGE1_MAILBOX_CMD_SEQ_OFFSET),
        .cmd_ret = psvr2_load_le64(
            data + offset + PSVR2_STAGE1_MAILBOX_CMD_RET_OFFSET),
        .valid = true
    };
    if (mailbox_within_module(layout, mailbox))
        return true;
    memset(mailbox, 0, sizeof(*mailbox));
    return false;
}

bool psvr2_stage1_mailbox_from_module_data(
    const psvr2_module_layout *layout, uint64_t writable_start,
    const uint8_t *data, size_t length, uint16_t expected_firmware,
    psvr2_stage1_mailbox *mailbox) {
    if (mailbox) memset(mailbox, 0, sizeof(*mailbox));
    if (!layout || !data || !mailbox || !layout->base ||
        layout->ro_size >= layout->size ||
        layout->text_size > layout->ro_size ||
        layout->base > UINT64_MAX - layout->size ||
        writable_start != layout->base + layout->ro_size ||
        writable_start % sizeof(uint64_t) ||
        length > (size_t)layout->size - layout->ro_size ||
        (expected_firmware != PSVR2_STAGE1_MAILBOX_FIRMWARE_0110 &&
         expected_firmware != PSVR2_STAGE1_MAILBOX_FIRMWARE_0600))
        return false;

    for (size_t index = 0;
         index + PSVR2_STAGE1_MAILBOX_DESCRIPTOR_SIZE <= length;
        index += sizeof(uint64_t)) {
        if (mailbox_descriptor_from_data(
                layout, data, index, length, expected_firmware,
                mailbox))
            return true;
    }

    return false;
}

static uint16_t expected_firmware(const psvr2_runtime *runtime) {
    if (!runtime || !runtime->krw || !runtime->krw->ex ||
        !runtime->krw->ex->constants)
        return 0;
    switch (runtime->krw->ex->constants->version) {
        case PSVR2_FW_0110: return PSVR2_STAGE1_MAILBOX_FIRMWARE_0110;
        case PSVR2_FW_0600: return PSVR2_STAGE1_MAILBOX_FIRMWARE_0600;
        default: return 0;
    }
}

static bool discover_mailbox_in_module(
    psvr2_runtime *runtime, const psvr2_module_layout *layout) {
    size_t writable_size =
        (size_t)layout->size - layout->ro_size;
    size_t scan =
        psvr2_min_size(
            writable_size, (size_t)PSVR2_STAGE1_RW_SCAN_SIZE);
    uint64_t scan_start = layout->base + layout->ro_size;
    if (!scan) return false;

    uint8_t data[PSVR2_STAGE1_RW_SCAN_SIZE];
    if (!psvr2_krw_read(
            runtime->krw, scan_start, data, scan))
        return false;
    uint16_t firmware = expected_firmware(runtime);
    if (!firmware) return false;
    return psvr2_stage1_mailbox_from_module_data(
        layout, scan_start, data, scan, firmware,
        &runtime->mailbox);
}

bool psvr2_stage1_discover_from_module(
    psvr2_runtime *runtime, bool *layout_ready) {
    if (layout_ready) *layout_ready = false;
    if (!runtime) return false;
    memset(&runtime->mailbox, 0, sizeof(runtime->mailbox));
    if (!expected_firmware(runtime)) return false;
    uint64_t module = psvr2_find_module(runtime->krw, "stage1");
    if (!module) return false;

    psvr2_module_layout layout;
    if (!psvr2_module_core_layout(
            runtime->krw, module, &layout))
        return false;
    if (layout_ready) *layout_ready = true;
    return discover_mailbox_in_module(runtime, &layout);
}

bool psvr2_stage1_discover(psvr2_runtime *runtime) {
    if (!runtime) return false;
    uint16_t firmware = expected_firmware(runtime);
    if (firmware && runtime->mailbox.valid &&
        runtime->mailbox.firmware == firmware)
        return true;
    bool ok =
        psvr2_stage1_discover_from_module(runtime, NULL);
    runtime->stage1_loaded = ok;
    if (ok)
        psvr2_runtime_mark_phase(
            runtime, PSVR2_RUNTIME_STAGE1_READY);
    return ok;
}

bool psvr2_stage1_read_status(psvr2_runtime *runtime,
                              char *output, size_t output_size) {
    if (!runtime || !output || output_size < 2U ||
        !psvr2_stage1_discover(runtime) ||
        !runtime->mailbox.status)
        return false;
    size_t length = psvr2_min_size(
        output_size - 1U, (size_t)PSVR2_STAGE1_STATUS_SIZE - 1U);
    if (!psvr2_krw_read(
            runtime->krw, runtime->mailbox.status, output, length))
        return false;
    output[length] = '\0';
    size_t end = strnlen(output, length);
    output[end] = '\0';
    return !strncmp(output, "OK ", 3) ||
           !strncmp(output, "ERR ", 4) ||
           !strncmp(output, "WAIT ", 5);
}
