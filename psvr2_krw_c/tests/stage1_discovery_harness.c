/* Exercise current-only discovery/load decisions without USB or modules. */
#include "stage1_internal.h"
#include "psvr2/shellcode.h"

#include <assert.h>
#include <string.h>

static const psvr2_module_layout layout = {
    .base = UINT64_C(0xffffffbffc0d7000), .size = 0x96000U,
    .text_size = 0x3000U, .ro_size = 0x4000U
};
static uint8_t module_data[PSVR2_STAGE1_RW_SCAN_SIZE];
static bool present;
static size_t exec_calls, upload_calls;

static void publish_current_descriptor(void) {
    memset(module_data, 0, sizeof(module_data));
    uint64_t writable = layout.base + layout.ro_size;
    const size_t record = 0x30U;
    psvr2_store_le64(module_data + record, PSVR2_STAGE1_MAILBOX_MAGIC);
    psvr2_store_le16(module_data + record + 8U, PSVR2_STAGE1_MAILBOX_ABI);
    psvr2_store_le16(module_data + record + 10U,
                     PSVR2_STAGE1_MAILBOX_FIRMWARE_0600);
    psvr2_store_le32(module_data + record + 12U,
                     PSVR2_STAGE1_MAILBOX_DESCRIPTOR_SIZE);
    psvr2_store_le32(module_data + record + 16U, PSVR2_STAGE1_STATUS_SIZE);
    psvr2_store_le32(module_data + record + 20U, 512U);
    psvr2_store_le32(module_data + record + 24U, 65536U);
    const uint64_t offsets[] = {0x100U, 0x200U, 0x400U, 0x1000U,
                                0x500U, 0x508U, 0x510U};
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(offsets); ++index)
        psvr2_store_le64(module_data + record + 32U + index * 8U,
                         writable + offsets[index]);
    strcpy((char *)module_data + 0x100U, "ERR previous command completed");
    snprintf((char *)module_data + 0x800U, sizeof(module_data) - 0x800U,
        "OK ready status=%llx cmd_in=%llx cmd_in_len=%llx cmd_out=%llx "
        "cmd_out_len=%llx cmd_seq=%llx cmd_ret=%llx",
        (unsigned long long)(writable + offsets[0]),
        (unsigned long long)(writable + offsets[1]),
        (unsigned long long)(writable + offsets[2]),
        (unsigned long long)(writable + offsets[3]),
        (unsigned long long)(writable + offsets[4]),
        (unsigned long long)(writable + offsets[5]),
        (unsigned long long)(writable + offsets[6]));
}

uint64_t psvr2_find_module(psvr2_krw *krw, const char *name) {
    (void)krw;
    assert(!strcmp(name, "stage1"));
    return present ? 1U : 0U;
}

bool psvr2_module_core_layout(psvr2_krw *krw, uint64_t module,
                              psvr2_module_layout *out) {
    (void)krw;
    assert(present && module == 1U);
    *out = layout;
    return true;
}

bool psvr2_krw_read(psvr2_krw *krw, uint64_t address, void *out,
                    size_t length) {
    (void)krw;
    uint64_t writable = layout.base + layout.ro_size;
    if (!present || address < writable || length > sizeof(module_data) ||
        address - writable > sizeof(module_data) - length)
        return false;
    memcpy(out, module_data + (size_t)(address - writable), length);
    return true;
}

bool psvr2_krw_read_ex(psvr2_krw *krw, uint64_t address, void *out,
                       size_t length, bool reinit, bool fatal) {
    (void)reinit;
    (void)fatal;
    return psvr2_krw_read(krw, address, out, length);
}

bool psvr2_krw_write_u64_fast(psvr2_krw *krw, uint64_t address,
                              uint64_t value) {
    (void)krw; (void)address; (void)value;
    assert(0 && "discovery/load must not issue mailbox writes");
    return false;
}

void psvr2_runtime_mark_phase(psvr2_runtime *runtime,
                              psvr2_runtime_phase phase) {
    runtime->phase = phase;
}

bool psvr2_exec(psvr2_runtime *runtime, const char *command,
                 bool capture, int64_t *result, psvr2_buffer *output) {
    (void)runtime; (void)output;
    assert(!capture);
    ++exec_calls;
    if (!strcmp(command, "rmmod stage1")) {
        assert(present);
        present = false;
    } else {
        assert(!strncmp(command, "insmod '", 8U) && !present);
        publish_current_descriptor();
        present = true;
    }
    *result = 0;
    return true;
}

void psvr2_vfs_init(psvr2_vfs *vfs, psvr2_krw *krw) { vfs->krw = krw; }
uint64_t psvr2_vfs_root(psvr2_vfs *vfs) { (void)vfs; return 1U; }
uint64_t psvr2_vfs_resolve_path(psvr2_vfs *vfs, uint64_t root,
                                const char *path) {
    (void)vfs; (void)root; (void)path;
    return 1U;
}
bool psvr2_upload_file(psvr2_runtime *runtime, const char *local,
                       const char *remote) {
    (void)runtime;
    assert(local && !strcmp(remote, "stage1.ko"));
    ++upload_calls;
    return true;
}

int main(int argc, char **argv) {
    assert(argc >= 1);
    psvr2_constants constants = {.version = PSVR2_FW_0600};
    psvr2_exploit exploit = {.constants = &constants};
    psvr2_krw krw = {.ex = &exploit};
    psvr2_runtime runtime = {.krw = &krw};
    publish_current_descriptor();
    present = true;
    assert(psvr2_stage1_discover(&runtime));
    assert(runtime.stage1_loaded && exec_calls == 0U);
    char status[80];
    assert(psvr2_stage1_read_status(&runtime, status, sizeof(status)));
    assert(!strcmp(status, "ERR previous command completed"));

    /* Text parsing may be useful for diagnostics but cannot seed a trusted
     * cached mailbox; only a matching versioned descriptor supplies firmware. */
    psvr2_store_le64(module_data + 0x30U, 0U);
    assert(psvr2_parse_stage1_mailbox(
        (char *)module_data + 0x800U, &runtime.mailbox));
    assert(!psvr2_stage1_discover(&runtime));
    assert(!runtime.mailbox.valid && exec_calls == 0U);

    /* A fresh host must use the descriptor, even with a valid previous string
     * or a proc fallback that would have invoked the injected UMH worker. */
    const size_t header_offsets[] = {0U, 8U, 10U};
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(header_offsets); ++index) {
        publish_current_descriptor();
        module_data[0x30U + header_offsets[index]] ^= 1U;
        runtime = (psvr2_runtime){.krw = &krw};
        assert(!psvr2_stage1_discover(&runtime));
        assert(!runtime.mailbox.valid && !runtime.stage1_loaded);
        assert(exec_calls == 0U);
        assert(!psvr2_load_stage1(&runtime, argv[0], true, false));
        assert(exec_calls == 0U && upload_calls == 0U && present);
    }

    /* A missing result field is incompatible, not an optional older ABI. */
    publish_current_descriptor();
    psvr2_store_le64(module_data + 0x30U + 80U, 0U);
    runtime = (psvr2_runtime){.krw = &krw};
    assert(!psvr2_load_stage1(&runtime, NULL, false, false));
    assert(exec_calls == 0U && upload_calls == 0U && present);

    /* Absence still permits ordinary loading. Explicit replacement remains
     * the only path that unloads a present incompatible module. */
    present = false;
    assert(psvr2_load_stage1(&runtime, NULL, false, false));
    assert(exec_calls == 1U && upload_calls == 0U && present);
    publish_current_descriptor();
    psvr2_store_le64(module_data + 0x30U, 0U);
    runtime = (psvr2_runtime){.krw = &krw};
    assert(psvr2_load_stage1(&runtime, argv[0], true, true));
    assert(exec_calls == 3U && upload_calls == 1U && present);
    assert(runtime.mailbox.valid && runtime.stage1_loaded);
    puts("current-only Stage1 discovery/load regressions passed");
    return 0;
}
