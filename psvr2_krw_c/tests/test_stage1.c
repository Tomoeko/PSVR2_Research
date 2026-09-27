#include "test_cases.h"

#include "stage1_internal.h"
#include "wait_status_internal.h"

#include <assert.h>
#include <string.h>

void test_mailbox_and_pte(void) {
    /* Process exit codes occupy the high byte of Linux wait status. */
    for (int64_t code = 0; code <= 255; ++code)
        assert(psvr2_remote_exit_status(code * 256) == code);
    assert(psvr2_remote_exit_status(256) == 1);
    assert(psvr2_remote_exit_status(32512) == 127);

    /* Signal identity survives both ordinary and core-dumping termination. */
    for (int64_t signal = 1; signal <= 64; ++signal) {
        assert(psvr2_remote_exit_status(signal) == 128 + signal);
        assert(psvr2_remote_exit_status(signal | 0x80) == 128 + signal);
    }
    assert(psvr2_remote_exit_status(15) == 143);
    assert(psvr2_remote_exit_status(0x8b) == 139);

    /* Preserve native kernel errors and zero-extended AArch64 int errors. */
    assert(psvr2_remote_exit_status(-1) == -1);
    assert(psvr2_remote_exit_status(-2) == -2);
    assert(psvr2_remote_exit_status(-4095) == -4095);
    assert(psvr2_remote_exit_status(INT64_MIN) == INT64_MIN);
    assert(psvr2_remote_exit_status(INT64_C(0xffffffff)) == -1);
    assert(psvr2_remote_exit_status(INT64_C(0xfffffffe)) == -2);
    assert(psvr2_remote_exit_status(INT64_C(0xfffff001)) == -4095);

    /* These are not completed process exits; do not manufacture exit codes. */
    assert(psvr2_remote_exit_status(0x137f) == 0x137f);
    assert(psvr2_remote_exit_status(0xffff) == 0xffff);
    assert(psvr2_remote_exit_status(0x80) == 0x80);
    assert(psvr2_remote_exit_status(0xdead) == 0xdead);
    assert(psvr2_remote_exit_status(0x10000) == 0x10000);

    psvr2_stage1_mailbox box;
    const char *status =
        "OK ready status=0xffffffc01233ff00 "
        "cmd_in=0xffffffc012340000 cmd_in_len=0xffffffc012340200 "
        "cmd_out=0xffffffc012341000 cmd_out_len=0xffffffc012342000 "
        "cmd_seq=0xffffffc012342008 cmd_ret=0xffffffc012342010";
    assert(psvr2_parse_stage1_mailbox(status, &box));
    assert(box.valid);
    assert(box.status == UINT64_C(0xffffffc01233ff00));
    assert(box.cmd_in == UINT64_C(0xffffffc012340000));
    assert(box.cmd_seq == UINT64_C(0xffffffc012342008));
    assert(box.cmd_ret == UINT64_C(0xffffffc012342010));
    status =
        "OK ready cmd_in=ffffffc012340000 cmd_in_len=ffffffc012340200 "
        "cmd_out=ffffffc012341000 cmd_out_len=ffffffc012342000 "
        "cmd_seq=ffffffc012342008";
    assert(!psvr2_parse_stage1_mailbox(status, &box));
    assert(!box.valid);
    assert(!psvr2_parse_stage1_mailbox("OK ready cmd_in=1", &box));
    assert(!box.valid);
    assert(!psvr2_parse_stage1_mailbox(NULL, &box));
    assert(!psvr2_parse_stage1_mailbox(status, NULL));

    psvr2_module_layout layout = {
        .base = UINT64_C(0xffffffbffc0d7000),
        .size = UINT32_C(0x96000),
        .text_size = UINT32_C(0x3000),
        .ro_size = UINT32_C(0x4000)
    };
    uint8_t module_data[PSVR2_STAGE1_RW_SCAN_SIZE] = {0};
    const size_t descriptor = 0x30U;
    const uint64_t writable = layout.base + layout.ro_size;
    const uint64_t status_address = writable + 0x100U;
    const uint64_t input_address = writable + 0x200U;
    const uint64_t input_length_address = writable + 0x400U;
    const uint64_t output_address = writable + 0x1000U;
    const uint64_t output_length_address = writable + 0x500U;
    const uint64_t sequence_address = writable + 0x508U;
    const uint64_t result_address = writable + 0x510U;
    psvr2_store_le64(
        module_data + descriptor, PSVR2_STAGE1_MAILBOX_MAGIC);
    psvr2_store_le16(
        module_data + descriptor + 8U, PSVR2_STAGE1_MAILBOX_ABI);
    psvr2_store_le16(
        module_data + descriptor + 10U,
        PSVR2_STAGE1_MAILBOX_FIRMWARE_0600);
    psvr2_store_le32(
        module_data + descriptor + 12U,
        PSVR2_STAGE1_MAILBOX_DESCRIPTOR_SIZE);
    psvr2_store_le32(
        module_data + descriptor + 16U, PSVR2_STAGE1_STATUS_SIZE);
    psvr2_store_le32(module_data + descriptor + 20U, 512U);
    psvr2_store_le32(module_data + descriptor + 24U, 65536U);
    psvr2_store_le64(module_data + descriptor + 32U, status_address);
    psvr2_store_le64(module_data + descriptor + 40U, input_address);
    psvr2_store_le64(
        module_data + descriptor + 48U, input_length_address);
    psvr2_store_le64(module_data + descriptor + 56U, output_address);
    psvr2_store_le64(
        module_data + descriptor + 64U, output_length_address);
    psvr2_store_le64(
        module_data + descriptor + 72U, sequence_address);
    psvr2_store_le64(module_data + descriptor + 80U, result_address);
    assert(psvr2_stage1_mailbox_from_module_data(
        &layout, layout.base + layout.ro_size,
        module_data, sizeof(module_data),
        PSVR2_STAGE1_MAILBOX_FIRMWARE_0600, &box));
    assert(box.firmware == PSVR2_STAGE1_MAILBOX_FIRMWARE_0600);
    assert(box.cmd_in == input_address);
    assert(box.cmd_in_len == input_length_address);
    assert(box.cmd_out == output_address);
    assert(box.cmd_out_len == output_length_address);
    assert(box.cmd_seq == sequence_address);
    assert(box.cmd_ret == result_address);
    assert(box.status == status_address);

    /* The descriptor remains authoritative after normal command activity has
     * replaced the mutable status text. Legacy pointer strings are irrelevant. */
    memcpy(module_data + 0x100U, "ERR completed command", 22U);
    snprintf((char *)module_data + 0x800U, sizeof(module_data) - 0x800U,
        "OK ready status=%llx cmd_in=%llx cmd_in_len=%llx "
        "cmd_out=%llx cmd_out_len=%llx cmd_seq=%llx cmd_ret=%llx",
        (unsigned long long)status_address,
        (unsigned long long)input_address,
        (unsigned long long)input_length_address,
        (unsigned long long)output_address,
        (unsigned long long)output_length_address,
        (unsigned long long)sequence_address,
        (unsigned long long)result_address);
    assert(psvr2_stage1_mailbox_from_module_data(
        &layout, writable, module_data, sizeof(module_data),
        PSVR2_STAGE1_MAILBOX_FIRMWARE_0600, &box));

    psvr2_store_le64(module_data + descriptor, 0U);
    assert(!psvr2_stage1_mailbox_from_module_data(
        &layout, writable, module_data, sizeof(module_data),
        PSVR2_STAGE1_MAILBOX_FIRMWARE_0600, &box));
    assert(!box.valid);
    psvr2_store_le64(module_data + descriptor, PSVR2_STAGE1_MAILBOX_MAGIC);
    psvr2_store_le16(module_data + descriptor + 8U,
                     PSVR2_STAGE1_MAILBOX_ABI + 1U);
    assert(!psvr2_stage1_mailbox_from_module_data(
        &layout, writable, module_data, sizeof(module_data),
        PSVR2_STAGE1_MAILBOX_FIRMWARE_0600, &box));
    assert(!box.valid);
    psvr2_store_le16(module_data + descriptor + 8U, PSVR2_STAGE1_MAILBOX_ABI);

    const size_t pointer_offsets[] = {32U, 40U, 48U, 56U, 64U, 72U, 80U};
    const uint64_t pointer_values[] = {
        status_address, input_address, input_length_address, output_address,
        output_length_address, sequence_address, result_address
    };
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(pointer_offsets); ++index) {
        /* No mailbox pointer may address module text/rodata or be absent. */
        const uint64_t invalid[] = {0U, layout.base + 16U};
        for (size_t choice = 0; choice < PSVR2_ARRAY_LEN(invalid); ++choice) {
            psvr2_store_le64(module_data + descriptor + pointer_offsets[index],
                             invalid[choice]);
            assert(!psvr2_stage1_mailbox_from_module_data(
                &layout, writable, module_data, sizeof(module_data),
                PSVR2_STAGE1_MAILBOX_FIRMWARE_0600, &box));
            assert(!box.valid);
        }
        psvr2_store_le64(module_data + descriptor + pointer_offsets[index],
                         pointer_values[index]);
    }
    const size_t scalar_indexes[] = {2U, 4U, 5U, 6U};
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(scalar_indexes); ++index) {
        size_t pointer = scalar_indexes[index];
        psvr2_store_le64(module_data + descriptor + pointer_offsets[pointer],
                         pointer_values[pointer] + 1U);
        assert(!psvr2_stage1_mailbox_from_module_data(
            &layout, writable, module_data, sizeof(module_data),
            PSVR2_STAGE1_MAILBOX_FIRMWARE_0600, &box));
        psvr2_store_le64(module_data + descriptor + pointer_offsets[pointer],
                         pointer_values[pointer]);
    }
    /* Aligned pointers inside the writable core still cannot alias buffers
     * or another scalar, which would overwrite live mailbox state. */
    const uint64_t aliases[] = {input_address, result_address};
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(aliases); ++index) {
        psvr2_store_le64(module_data + descriptor + 72U, aliases[index]);
        assert(!psvr2_stage1_mailbox_from_module_data(
            &layout, writable, module_data, sizeof(module_data),
            PSVR2_STAGE1_MAILBOX_FIRMWARE_0600, &box));
    }
    psvr2_store_le64(module_data + descriptor + 72U, sequence_address);

    psvr2_store_le16(
        module_data + descriptor + 10U,
        PSVR2_STAGE1_MAILBOX_FIRMWARE_0110);
    assert(psvr2_stage1_mailbox_from_module_data(
        &layout, writable, module_data, sizeof(module_data),
        PSVR2_STAGE1_MAILBOX_FIRMWARE_0110, &box));
    assert(box.firmware == PSVR2_STAGE1_MAILBOX_FIRMWARE_0110);
    assert(!psvr2_stage1_mailbox_from_module_data(
        &layout, writable, module_data, sizeof(module_data),
        PSVR2_STAGE1_MAILBOX_FIRMWARE_0600, &box));

    psvr2_store_le16(
        module_data + descriptor + 10U, UINT16_C(0x0200));
    assert(!psvr2_stage1_mailbox_from_module_data(
        &layout, writable, module_data, sizeof(module_data),
        PSVR2_STAGE1_MAILBOX_FIRMWARE_0600, &box));
    assert(!box.valid);
    psvr2_store_le16(
        module_data + descriptor + 10U,
        PSVR2_STAGE1_MAILBOX_FIRMWARE_0600);

    psvr2_store_le32(module_data + descriptor + 20U, 511U);
    assert(!psvr2_stage1_mailbox_from_module_data(
        &layout, writable, module_data, sizeof(module_data),
        PSVR2_STAGE1_MAILBOX_FIRMWARE_0600, &box));
    assert(!box.valid);
    psvr2_store_le32(module_data + descriptor + 20U, 512U);

    psvr2_store_le64(
        module_data + descriptor + 56U,
        layout.base + layout.size);
    assert(!psvr2_stage1_mailbox_from_module_data(
        &layout, layout.base + layout.ro_size,
        module_data, sizeof(module_data),
        PSVR2_STAGE1_MAILBOX_FIRMWARE_0600, &box));
    assert(!box.valid);

    char permissions[4];
    char detail[80];
    uint64_t pte = UINT64_C(1) | (UINT64_C(1) << 10) |
                   (UINT64_C(1) << 53) | (UINT64_C(1) << 54);
    psvr2_decode_pte(
        pte, PSVR2_PTE_L3_PAGE,
        permissions, detail, sizeof(detail));
    assert(strcmp(permissions, "rw-") == 0);
    assert(strstr(detail, "AF=1") && strstr(detail, "L3_PAGE"));

    pte |= UINT64_C(2) << 6;
    pte &= ~(UINT64_C(1) << 53);
    psvr2_decode_pte(
        pte, PSVR2_PTE_L2_BLOCK,
        permissions, detail, sizeof(detail));
    assert(strcmp(permissions, "r-x") == 0);
    assert(strstr(detail, "AP=2") && strstr(detail, "L2_BLOCK"));
}
