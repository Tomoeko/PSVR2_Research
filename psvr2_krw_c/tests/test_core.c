#include "kernel_bootstrap_internal.h"
#include "kernel_images_internal.h"
#include "firmware_0600_internal.h"
#include "persistence_internal.h"
#include "test_cases.h"

#include "psvr2/arm64.h"
#include "psvr2/auth.h"
#include "psvr2/device.h"
#include "psvr2/exploit.h"
#include "psvr2/gaze.h"
#include "psvr2/kernel_exec.h"
#include "psvr2/kernel_payload.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void test_endian(void) {
    uint8_t data[8];
    psvr2_store_le16(data, UINT16_C(0x2211));
    assert(psvr2_load_le16(data) == UINT16_C(0x2211));
    psvr2_store_le32(data, UINT32_C(0x44332211));
    assert(psvr2_load_le32(data) == UINT32_C(0x44332211));
    psvr2_store_le64(data, UINT64_C(0x8877665544332211));
    assert(memcmp(data, "\x11\x22\x33\x44\x55\x66\x77\x88", 8) == 0);
    assert(psvr2_load_le64(data) == UINT64_C(0x8877665544332211));
}

static void test_structured_results(void) {
    psvr2_result ok = psvr2_result_ok();
    assert(psvr2_result_is_ok(&ok));
    assert(strcmp(psvr2_status_name(ok.status), "ok") == 0);
    psvr2_result error = psvr2_result_error(
        PSVR2_STATUS_USB_ERROR, LIBUSB_ERROR_TIMEOUT,
        "transfer %u timed out", 7U);
    assert(!psvr2_result_is_ok(&error));
    assert(error.status == PSVR2_STATUS_USB_ERROR);
    assert(error.native_code == LIBUSB_ERROR_TIMEOUT);
    assert(strcmp(error.message, "transfer 7 timed out") == 0);
    assert(strcmp(
        psvr2_status_name(PSVR2_STATUS_RECOVERY_REQUIRED),
        "recovery required") == 0);
}

static void test_auth_reports(void) {
    static const uint8_t check[] = "123456789";
    uint8_t certificate[PSVR2_AUTH_CERTIFICATE_SIZE];
    uint8_t report[PSVR2_AUTH_REPORT_SIZE];
    uint8_t status[PSVR2_AUTH_STATUS_REPORT_SIZE] = {0};
    uint8_t authentication_id = 0;
    uint8_t state = 0;

    assert(psvr2_auth_crc32(check, sizeof(check) - 1U) ==
           UINT32_C(0xcbf43926));
    for (size_t index = 0; index < sizeof(certificate); ++index)
        certificate[index] = (uint8_t)index;
    assert(psvr2_auth_build_certificate_report(
        report, certificate, UINT8_C(0x5a), 0));
    assert(report[0] == UINT8_C(0xf0));
    assert(report[1] == 1 && report[2] == UINT8_C(0x5a) &&
           report[3] == 0);
    assert(memcmp(report + 4, "\0\0\0\0", 4) == 0);
    assert(memcmp(report + 8, certificate, 16) == 0);
    assert(memcmp(report + 24, certificate, 36) == 0);
    uint32_t crc = psvr2_auth_crc32(report, 60);
    assert(report[60] == (uint8_t)(crc >> 24));
    assert(report[61] == (uint8_t)(crc >> 16));
    assert(report[62] == (uint8_t)(crc >> 8));
    assert(report[63] == (uint8_t)crc);

    assert(psvr2_auth_build_certificate_report(
        report, certificate, 0, 3));
    assert(report[3] == 3);
    assert(memcmp(report + 4, certificate + 148, 12) == 0);
    for (size_t index = 16; index < 60; ++index)
        assert(report[index] == 0);
    assert(!psvr2_auth_build_certificate_report(
        report, certificate, 0, PSVR2_AUTH_CERTIFICATE_BLOCKS));
    assert(!psvr2_auth_build_certificate_report(NULL, certificate, 0, 0));
    assert(!psvr2_auth_build_certificate_report(report, NULL, 0, 0));

    status[0] = UINT8_C(0xf2);
    status[3] = PSVR2_AUTH_STATE_WAIT_HOST_RESPONSE;
    assert(psvr2_auth_parse_status_report(status, sizeof(status), &state));
    assert(state == PSVR2_AUTH_STATE_WAIT_HOST_RESPONSE);
    assert(!psvr2_auth_parse_status_report(
        status, sizeof(status) - 1U, &state));
    status[2] = UINT8_C(0x5a);
    assert(psvr2_auth_parse_status_report(status, sizeof(status), &state));
    assert(state == PSVR2_AUTH_STATE_WAIT_HOST_RESPONSE);
    assert(psvr2_auth_parse_status_report_id(
        status, sizeof(status), &authentication_id, &state));
    assert(authentication_id == UINT8_C(0x5a));
    assert(state == PSVR2_AUTH_STATE_WAIT_HOST_RESPONSE);
    assert(!psvr2_auth_parse_status_report(NULL, sizeof(status), &state));
    assert(!psvr2_auth_parse_status_report(status, sizeof(status), NULL));
    assert(!psvr2_auth_parse_status_report_id(
        status, sizeof(status) - 1U, &authentication_id, &state));
    status[0] = 0;
    assert(!psvr2_auth_parse_status_report_id(
        status, sizeof(status), &authentication_id, &state));
}

static void test_gaze_protocol(void) {
    uint8_t header[PSVR2_GAZE_CALIBRATION_HEADER_SIZE];
    psvr2_gaze_calibration_header parsed;
    uint16_t status = 0;
    uint8_t status_report[PSVR2_GAZE_STATUS_REPORT_SIZE] = {
        UINT8_C(0x8c), 0, UINT8_C(0x34), UINT8_C(0x12)
    };

    assert(psvr2_gaze_build_calibration_header(
        header, sizeof(header), PSVR2_GAZE_CHANNEL_CALIBRATION, 0));
    assert(memcmp(header, "GC\x00\x03\x01\x01\x03\x00", 8) == 0);
    for (size_t index = 8; index < sizeof(header); ++index)
        assert(header[index] == 0);
    psvr2_store_le32(header + 12, UINT32_C(0x11223344));
    header[16] = 2;
    assert(psvr2_gaze_parse_calibration_header(
        header, sizeof(header), PSVR2_GAZE_CHANNEL_CALIBRATION, &parsed));
    assert(parsed.channel == PSVR2_GAZE_CHANNEL_CALIBRATION);
    assert(parsed.payload_size == 0);
    assert(parsed.calibration_id == UINT32_C(0x11223344));
    assert(parsed.calibrated_eye == 2);
    assert(psvr2_gaze_parse_calibration_header(
        header, sizeof(header), PSVR2_GAZE_CHANNEL_CALIBRATION, NULL));
    assert(!psvr2_gaze_parse_calibration_header(
        header, sizeof(header), PSVR2_GAZE_CHANNEL_IMAGE, &parsed));
    assert(!psvr2_gaze_parse_calibration_header(
        header, sizeof(header) - 1U, PSVR2_GAZE_CHANNEL_CALIBRATION,
        &parsed));
    assert(!psvr2_gaze_build_calibration_header(
        header, sizeof(header), 5, 0));
    assert(!psvr2_gaze_build_calibration_header(
        NULL, sizeof(header), PSVR2_GAZE_CHANNEL_CALIBRATION, 0));

    assert(psvr2_gaze_parse_stream_status(
        status_report, sizeof(status_report), &status));
    assert(status == UINT16_C(0x1234));
    status_report[1] = 1;
    assert(!psvr2_gaze_parse_stream_status(
        status_report, sizeof(status_report), &status));
    assert(!psvr2_gaze_parse_stream_status(
        NULL, sizeof(status_report), &status));
}

static void test_gaze_generic_packet(void) {
    uint8_t packet[0x260] = {0};
    memcpy(packet, "VD", 2);
    psvr2_store_le16(packet + 2, PSVR2_GAZE_GENERIC_HEADER_SIZE);
    psvr2_store_le32(packet + 4, sizeof(packet));
    psvr2_store_le16(packet + 8, 2);

    psvr2_store_le16(packet + 0x20, PSVR2_GAZE_CHANNEL_CALIBRATION);
    psvr2_store_le16(packet + 0x22, 5);
    psvr2_store_le32(packet + 0x24, 33);
    psvr2_store_le32(packet + 0x28, 0x200);
    psvr2_store_le16(packet + 0x2c, PSVR2_GAZE_CHANNEL_IMAGE);
    psvr2_store_le16(packet + 0x2e, 5);
    psvr2_store_le32(packet + 0x30, 32);
    psvr2_store_le32(packet + 0x34, 0x240);
    assert(psvr2_gaze_build_calibration_header(
        packet + 0x200, 32, PSVR2_GAZE_CHANNEL_CALIBRATION, 1));
    packet[0x220] = UINT8_C(0xa5);
    assert(psvr2_gaze_build_calibration_header(
        packet + 0x240, 32, PSVR2_GAZE_CHANNEL_IMAGE, 0));

    psvr2_gaze_generic_record records[2];
    size_t count = 0;
    assert(psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 2, &count));
    assert(count == 2);
    assert(records[0].channel == PSVR2_GAZE_CHANNEL_CALIBRATION);
    assert(records[0].bank == 5);
    assert(records[0].size == 33);
    assert(records[0].offset == 0x200);
    assert(records[0].padded_size == 64);
    assert(records[1].channel == PSVR2_GAZE_CHANNEL_IMAGE);
    assert(records[1].bank == 5);
    assert(records[1].size == 32);
    assert(records[1].offset == 0x240);
    assert(records[1].padded_size == 32);

    assert(!psvr2_gaze_parse_generic_packet(
        NULL, sizeof(packet), records, 2, &count));
    assert(!psvr2_gaze_parse_generic_packet(
        packet, 0x1ff, records, 2, &count));
    assert(!psvr2_gaze_parse_generic_packet(
        packet, PSVR2_GAZE_GENERIC_MAX_PACKET_SIZE + 1U,
        records, 2, &count));
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), NULL, 2, &count));
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 2, NULL));
    packet[0] = 'X';
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 2, &count));
    packet[0] = 'V';
    packet[2] ^= 1U;
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 2, &count));
    packet[2] ^= 1U;
    packet[4] ^= 1U;
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 2, &count));
    packet[4] ^= 1U;
    psvr2_store_le16(packet + 8, 0);
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 2, &count));
    psvr2_store_le16(packet + 8, PSVR2_GAZE_GENERIC_MAX_RECORDS + 1U);
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 2, &count));
    psvr2_store_le16(packet + 8, 2);
    psvr2_store_le16(packet + 0x22, 8);
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 2, &count));
    psvr2_store_le16(packet + 0x22, 5);
    psvr2_store_le16(packet + 0x2e, 4);
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 2, &count));
    psvr2_store_le16(packet + 0x2e, 5);
    psvr2_store_le32(packet + 0x24, 0);
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 2, &count));
    psvr2_store_le32(packet + 0x24, UINT32_MAX);
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 2, &count));
    psvr2_store_le32(packet + 0x24, 33);
    psvr2_store_le32(packet + 0x28, 0x220);
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 2, &count));
    psvr2_store_le32(packet + 0x28, 0x200);
    psvr2_store_le32(packet + 0x34, 0x220);
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 2, &count));
    psvr2_store_le32(packet + 0x34, 0x240);
    psvr2_store_le32(packet + 0x30, 65);
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 2, &count));
    psvr2_store_le32(packet + 0x30, 32);
    assert(!psvr2_gaze_parse_generic_packet(
        packet, sizeof(packet), records, 1, &count));

    uint8_t max_packet[0x700] = {0};
    psvr2_gaze_generic_record max_records[PSVR2_GAZE_GENERIC_MAX_RECORDS];
    memcpy(max_packet, "VD", 2);
    psvr2_store_le16(
        max_packet + 2, PSVR2_GAZE_GENERIC_HEADER_SIZE);
    psvr2_store_le32(max_packet + 4, sizeof(max_packet));
    psvr2_store_le16(
        max_packet + 8, PSVR2_GAZE_GENERIC_MAX_RECORDS);
    for (size_t index = 0;
         index < PSVR2_GAZE_GENERIC_MAX_RECORDS; ++index) {
        size_t descriptor = 0x20U +
            index * PSVR2_GAZE_GENERIC_DESCRIPTOR_SIZE;
        psvr2_store_le16(max_packet + descriptor, (uint16_t)index);
        psvr2_store_le16(max_packet + descriptor + 2, 7);
        psvr2_store_le32(max_packet + descriptor + 4, 1);
        psvr2_store_le32(
            max_packet + descriptor + 8,
            (uint32_t)(PSVR2_GAZE_GENERIC_HEADER_SIZE + index * 32U));
    }
    assert(psvr2_gaze_parse_generic_packet(
        max_packet, sizeof(max_packet), max_records,
        PSVR2_GAZE_GENERIC_MAX_RECORDS, &count));
    assert(count == PSVR2_GAZE_GENERIC_MAX_RECORDS);
    assert(max_records[39].offset == 0x6e0);
}

static void test_gaze_probe_transfer_classifier(void) {
    const uint32_t nonce = UINT32_C(0x475a8501);
    uint8_t data[PSVR2_GAZE_PROBE_TRANSFER_SIZE];

    assert(psvr2_gaze_fill_probe_sentinel(data, sizeof(data), nonce));
    assert(!psvr2_gaze_fill_probe_sentinel(NULL, sizeof(data), nonce));
    assert(psvr2_gaze_fill_probe_sentinel(
        data, sizeof(data) - 1U, nonce));
    assert(!psvr2_gaze_fill_probe_sentinel(data, 0, nonce));
    assert(psvr2_gaze_classify_probe_transfer(
               true, true, false, 1, NULL, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_REJECTED);
    assert(psvr2_gaze_classify_probe_transfer(
               true, true, false, 0, data, 0, nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_REJECTED);
    assert(psvr2_gaze_classify_probe_transfer(
               true, true, false, (int)sizeof(data) + 1,
               data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_REJECTED);
    assert(psvr2_gaze_classify_probe_transfer(
               true, false, true, (int)sizeof(data), data, sizeof(data),
               nonce) == PSVR2_GAZE_PROBE_TRANSFER_SILENT);
    data[sizeof(data) - 1U] ^= 1U;
    assert(psvr2_gaze_classify_probe_transfer(
               true, false, true, (int)sizeof(data), data, sizeof(data),
               nonce) == PSVR2_GAZE_PROBE_TRANSFER_REJECTED);

    assert(psvr2_gaze_fill_probe_sentinel(data, sizeof(data), nonce));
    assert(psvr2_gaze_classify_probe_transfer(
               true, false, true, 1000, data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_REJECTED);
    assert(psvr2_gaze_classify_probe_transfer(
               true, false, true, 0, data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_SILENT);
    data[0] ^= 1U;
    assert(psvr2_gaze_classify_probe_transfer(
               true, false, true, 0, data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_REJECTED);
    assert(psvr2_gaze_fill_probe_sentinel(data, sizeof(data), nonce));
    assert(psvr2_gaze_classify_probe_transfer(
               true, true, false, 0, data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_SILENT);
    assert(psvr2_gaze_fill_probe_sentinel(data, sizeof(data), nonce));
    assert(psvr2_gaze_classify_probe_transfer(
               true, true, false, (int)sizeof(data), data, sizeof(data),
               nonce) == PSVR2_GAZE_PROBE_TRANSFER_REJECTED);
    data[0] ^= 1U;
    assert(psvr2_gaze_classify_probe_transfer(
               true, true, false, (int)sizeof(data), data, sizeof(data),
               nonce) == PSVR2_GAZE_PROBE_TRANSFER_REJECTED);
    data[0] ^= 1U;
    data[sizeof(data) / 2U] ^= 1U;
    assert(psvr2_gaze_classify_probe_transfer(
               true, true, false, 0, data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_REJECTED);

    assert(psvr2_gaze_fill_probe_sentinel(data, sizeof(data), nonce));
    assert(psvr2_gaze_classify_probe_transfer(
               true, true, false, PSVR2_GAZE_CALIBRATION_HEADER_SIZE,
               data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_REJECTED);
    memset(data, 0, PSVR2_GAZE_CALIBRATION_HEADER_SIZE);
    assert(psvr2_gaze_classify_probe_transfer(
               true, true, false, PSVR2_GAZE_CALIBRATION_HEADER_SIZE,
               data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_DEVICE_DATA);
    data[PSVR2_GAZE_CALIBRATION_HEADER_SIZE] ^= 1U;
    assert(psvr2_gaze_classify_probe_transfer(
               true, true, false, PSVR2_GAZE_CALIBRATION_HEADER_SIZE,
               data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_REJECTED);
    assert(psvr2_gaze_classify_probe_transfer(
               true, false, true, PSVR2_GAZE_CALIBRATION_HEADER_SIZE,
               data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_REJECTED);

    assert(psvr2_gaze_fill_probe_sentinel(data, sizeof(data), nonce));
    data[0] ^= 1U;
    assert(psvr2_gaze_classify_probe_transfer(
               false, false, true, 0, data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_SILENT);
    assert(psvr2_gaze_classify_probe_transfer(
               false, false, true, (int)sizeof(data), data, sizeof(data),
               nonce) == PSVR2_GAZE_PROBE_TRANSFER_REJECTED);
    assert(psvr2_gaze_classify_probe_transfer(
               false, true, false, (int)sizeof(data), data, sizeof(data),
               nonce) == PSVR2_GAZE_PROBE_TRANSFER_DEVICE_DATA);
    assert(psvr2_gaze_classify_probe_transfer(
               false, true, false, PSVR2_GAZE_CALIBRATION_HEADER_SIZE,
               data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_DEVICE_DATA);

    assert(psvr2_gaze_classify_probe_transfer(
               true, false, false, 0, data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_REJECTED);
    assert(psvr2_gaze_classify_probe_transfer(
               true, true, true, 0, data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_REJECTED);
    assert(psvr2_gaze_classify_probe_transfer(
               true, false, true, -1, data, sizeof(data), nonce) ==
           PSVR2_GAZE_PROBE_TRANSFER_REJECTED);
}

static void test_firmware(void) {
    psvr2_constants c;
    uint8_t response[PSVR2_FIRMWARE_REPORT_SIZE] = {0};
    uint8_t pc_time[PSVR2_PC_TIME_REPORT_SIZE] = {0};
    uint32_t version = 0;
    assert(PSVR2_INIT_TASK == UINT64_C(0xffffffc00059f530));
    assert(PSVR2_EMERGENCY_RESTART ==
           UINT64_C(0xffffffc0000aa468));
    assert(PSVR2_MACHINE_POWER_OFF ==
           UINT64_C(0xffffffc000085330));
    assert(PSVR2_OFF_TASK_TASKS == 0x1f8U);
    assert(PSVR2_OFF_TASK_MM == 0x248U);
    assert(PSVR2_OFF_TASK_COMM == 0x4a8U);
    assert(PSVR2_OFF_TASK_FS == 0x4e0U);
    assert(PSVR2_OFF_TASK_NSPROXY == 0x4f0U);
    assert(PSVR2_MOD_OFF_CORE_BASE == 0x188U);
    assert(PSVR2_MOD_OFF_CORE_SIZE == 0x194U);
    assert(PSVR2_MOD_OFF_CORE_TEXT_SIZE == 0x19cU);
    assert(PSVR2_MOD_OFF_CORE_RO_SIZE == 0x1a4U);
    assert(psvr2_constants_set_firmware(&c, PSVR2_FW_0600, false));
    assert(strcmp(
               c.fw.artifact_path,
               "06.00/kernel.bin") == 0);
    assert(strcmp(
               c.fw.artifact_sha256,
               "5eea9d6da46efc8230183baf70023bef675f8dd09145d575b48f88a6337ca347") ==
           0);
    assert(c.fw.exact_image_baseline_present);
    assert(c.fw.init_task == PSVR2_INIT_TASK);
    assert(c.fw.task_tasks_offset == PSVR2_OFF_TASK_TASKS);
    assert(c.fw.aes_switch_address ==
           UINT64_C(0xffffffc000668a10));
    assert(c.fw.kernel_bss_stop == UINT64_C(0xffffffc000668f6c));
    assert(c.fw.kernel_page_tables_start ==
           UINT64_C(0xffffffc000669000));
    assert(c.fw.kernel_probe_byte == UINT64_C(0xffffffc000668f80));
    assert(c.fw.user_va_bits == 39);
    assert(!c.live_profile_certified);
    assert(!psvr2_constants_kernel_probe_safe(&c));
    assert(c.fw.injected_execution_certified);
    assert(c.str_helper == PSVR2_STR_SC);
    assert(c.umh_trigger == PSVR2_EXEC_SC);
    assert(c.umh_worker == PSVR2_WORK_SC);
    assert(!psvr2_constants_shellcode_execution_safe(&c));
    c.live_profile_certified = true;
    assert(psvr2_constants_kernel_probe_safe(&c));
    assert(psvr2_constants_shellcode_execution_safe(&c));
    c.firmware_forced = true;
    c.live_profile_certified = false;
    assert(!psvr2_constants_kernel_probe_safe(&c));
    assert(!psvr2_constants_shellcode_execution_safe(&c));
    c.live_profile_certified = true;
    assert(psvr2_constants_kernel_probe_safe(&c));
    assert(c.firmware_forced);
    assert(!psvr2_constants_runtime_workspace_safe(&c));

    c.runtime_work = UINT64_C(0xffffffc012340000);
    c.runtime_data =
        c.runtime_work + PSVR2_RUNTIME_DATA_OFFSET;
    assert(psvr2_constants_runtime_workspace_safe(&c));
    assert(psvr2_constants_shellcode_execution_safe(&c));
    c.runtime_work += 0xff0;
    c.runtime_data =
        c.runtime_work + PSVR2_RUNTIME_DATA_OFFSET;
    assert(!psvr2_constants_runtime_workspace_safe(&c));
    assert(psvr2_constants_set_firmware(&c, PSVR2_FW_0600, false));
    c.str_helper += 4;
    assert(!psvr2_constants_shellcode_execution_safe(&c));
    assert(psvr2_constants_set_firmware(&c, PSVR2_FW_0600, false));
    c.fw.kernel_probe_byte = c.fw.kernel_page_tables_start;
    assert(!psvr2_constants_kernel_probe_safe(&c));

    assert(psvr2_constants_set_firmware(&c, PSVR2_FW_0110, false));
    assert(strcmp(psvr2_firmware_name(PSVR2_FW_0110), "01.10") == 0);
    assert(strcmp(
               c.fw.artifact_path,
               "01.10/kernel.bin") == 0);
    assert(strcmp(
               c.fw.artifact_sha256,
               "a6d3cfcbf7e1f490aa043464c14230e927ad9c4ca54d81df8f927ee48ce88773") ==
           0);
    assert(c.fw.clean_return == UINT64_C(0xffffffc000305750));
    assert(c.fw.cold_static_stage == UINT64_C(0xffffffbffc0b12e8));
    assert(c.fw.mtu3_complete_resume == UINT64_C(0xffffffc00030465c));
    assert(c.fw.mtu3_ep0_isr_epilogue == UINT64_C(0xffffffc000303c3c));
    assert(c.fw.tlbi_gadget == UINT64_C(0xffffffc000112ba8));
    assert(c.fw.direct_read_helper == UINT64_C(0xffffffc00036ef28));
    assert(c.fw.raw_spin_lock == UINT64_C(0xffffffc00036d588));
    assert(c.fw.call_umh == UINT64_C(0xffffffc0000a0f40));
    assert(c.fw.queue_work_on == UINT64_C(0xffffffc0000a2c1c));
    assert(c.fw.system_wq == UINT64_C(0xffffffc00059e870));
    assert(c.fw.kmalloc == UINT64_C(0xffffffc00011ba84));
    assert(c.fw.kfree == UINT64_C(0xffffffc00011cc7c));
    assert(c.fw.module_list_head == UINT64_C(0xffffffc0005a83f0));
    assert(c.fw.aes_switch_address ==
           UINT64_C(0xffffffc000668a00));
    assert(c.fw.task_tasks_offset == 0x3a8U);
    assert(c.fw.exact_image_baseline_present);
    assert(c.fw.injected_execution_certified);
    assert(!c.live_profile_certified);
    c.live_profile_certified = true;
    assert(psvr2_constants_kernel_probe_safe(&c));
    assert(psvr2_constants_shellcode_execution_safe(&c));
    psvr2_kernel_anchor anchors[PSVR2_KERNEL_ANCHOR_COUNT];
    psvr2_kernel_execution_anchors(&c, anchors);
    assert(anchors[0].address == UINT64_C(0xffffffc000112ba8));
    assert(anchors[1].address == UINT64_C(0xffffffc00030465c));
    assert(anchors[1].length == 12U && anchors[1].bytes[4] == 0xca);

    assert(!psvr2_constants_set_firmware(
        &c, UINT32_C(0x02000101), false));
    assert(!psvr2_constants_set_firmware(&c, 0, false));
    response[0] = UINT8_C(0x81);
    psvr2_store_le16(response + 2, UINT16_C(0x01));
    psvr2_store_le16(response + 4, UINT16_C(0x54));
    psvr2_store_le32(response + 8, UINT32_C(0x02000101));
    assert(psvr2_parse_firmware_response(response, sizeof(response), &version));
    assert(version == UINT32_C(0x02000101));
    assert(!psvr2_parse_firmware_response(response, 12, &version));
    response[0] = UINT8_C(0x80);
    assert(!psvr2_parse_firmware_response(response, sizeof(response), &version));
    response[0] = UINT8_C(0x81);
    response[4] = UINT8_C(0x53);
    assert(!psvr2_parse_firmware_response(response, sizeof(response), &version));
    response[4] = UINT8_C(0x54);
    assert(!psvr2_parse_firmware_response(NULL, sizeof(response), &version));
    assert(!psvr2_parse_firmware_response(response, sizeof(response), NULL));

    pc_time[0] = PSVR2_PC_TIME_REPORT_ID;
    psvr2_store_le16(pc_time + 2, UINT16_C(1));
    psvr2_store_le16(pc_time + 4, UINT16_C(24));
    psvr2_store_le32(pc_time + 12, UINT32_C(0x12345600));
    psvr2_store_le32(pc_time + 16, UINT32_C(0x89abcdef));
    psvr2_store_le64(pc_time + 24, UINT64_C(0x1122334455667788));
    uint32_t vts = 0, dp_counter = 0;
    uint64_t stc = 0;
    assert(psvr2_parse_pc_time_response(
        pc_time, sizeof(pc_time), &vts, &dp_counter, &stc));
    assert(vts == UINT32_C(0x12345600));
    assert(dp_counter == UINT32_C(0x89abcdef));
    assert(stc == UINT64_C(0x1122334455667788));
    assert(psvr2_parse_pc_time_response(
        pc_time, sizeof(pc_time), NULL, NULL, NULL));
    assert(!psvr2_parse_pc_time_response(
        NULL, sizeof(pc_time), &vts, &dp_counter, &stc));
    assert(!psvr2_parse_pc_time_response(
        pc_time, sizeof(pc_time) - 1, &vts, &dp_counter, &stc));
    pc_time[7] = UINT8_C(0x55);
    assert(psvr2_parse_pc_time_response(
        pc_time, sizeof(pc_time), &vts, &dp_counter, &stc));
    pc_time[6] = UINT8_C(0xaa);
    assert(!psvr2_parse_pc_time_response(
        pc_time, sizeof(pc_time), &vts, &dp_counter, &stc));
    pc_time[6] = 0;
    pc_time[1] = 1;
    assert(!psvr2_parse_pc_time_response(
        pc_time, sizeof(pc_time), &vts, &dp_counter, &stc));
    pc_time[1] = 0;
    psvr2_store_le16(pc_time + 2, UINT16_C(2));
    assert(!psvr2_parse_pc_time_response(
        pc_time, sizeof(pc_time), &vts, &dp_counter, &stc));
    psvr2_store_le16(pc_time + 2, UINT16_C(1));
    psvr2_store_le16(pc_time + 4, UINT16_C(23));
    assert(!psvr2_parse_pc_time_response(
        pc_time, sizeof(pc_time), &vts, &dp_counter, &stc));
    psvr2_store_le16(pc_time + 4, UINT16_C(24));
    pc_time[8] = 1;
    assert(!psvr2_parse_pc_time_response(
        pc_time, sizeof(pc_time), &vts, &dp_counter, &stc));
    pc_time[8] = 0;
    pc_time[0] = UINT8_C(0xdf);
    assert(!psvr2_parse_pc_time_response(
        pc_time, sizeof(pc_time), &vts, &dp_counter, &stc));
    assert(strcmp(psvr2_firmware_name(PSVR2_FW_0600), "06.00") == 0);
    assert(strcmp(psvr2_firmware_name(UINT32_C(0xffffffff)), "unknown") == 0);
}

static void test_payloads(void) {
    assert(PSVR2_AUTH_COOKIE_PROBE_SIZE == 72U);
    psvr2_constants c;
    uint8_t payload[PSVR2_WRITE_PAYLOAD_SIZE];
    assert(psvr2_constants_set_firmware(&c, PSVR2_FW_0600, false));
    assert(psvr2_build_strb_payload(&c, payload,
            UINT64_C(0xffffffc000123456), 0xab,
            UINT64_C(0xffffffc000999999)) == 200);
    assert(payload[0] == 0xf0 && payload[1] == 0x01);
    assert(psvr2_load_le64(payload + 64) == PSVR2_STACK_COOKIE);
    assert(psvr2_load_le64(payload + 80) == c.fw.clean_return);
    assert(psvr2_load_le64(payload + 96) == UINT64_C(0xffffffc000123456) - 0xcd);
    assert(psvr2_load_le64(payload + 112) == 0xab);
    assert(psvr2_load_le64(payload + 192) == PSVR2_STACK_COOKIE);

    assert(psvr2_build_fast_write_payload(
               &c, payload, 0x1000, 0x2000, 0x3000, 0x5a, 0x4000) ==
           200);
    assert(psvr2_load_le64(payload + 80) == 0x4000);
    assert(psvr2_load_le64(payload + 88) == 0x2000);
    assert(psvr2_load_le64(payload + 96) == 0x1000);
    assert(psvr2_load_le64(payload + 104) == 0x3000);
    assert(psvr2_load_le64(payload + 112) == 0x5a);
    for (size_t i = 2; i < 64; ++i) assert(payload[i] == 0);
    for (size_t i = 120; i < 192; ++i) assert(payload[i] == 0);

    uint8_t patch_payload[PSVR2_TEXT_PATCH_PAYLOAD_SIZE];
    psvr2_registers registers = {
        .req = UINT64_C(0xffffffc010001000),
        .mep = UINT64_C(0xffffffc0100030a8),
        .x21 = UINT64_C(0xffffffc010003000),
        .x22 = UINT8_C(0x5a),
        .valid = true
    };
    assert(psvr2_build_text_patch_payload(
               &c, patch_payload, PSVR2_STR_SC,
               UINT32_C(0xd503201f), &registers) ==
           sizeof(patch_payload));
    assert(patch_payload[0] == 0xf0 && patch_payload[1] == 0x01);
    assert(psvr2_load_le64(patch_payload + 64) == PSVR2_STACK_COOKIE);
    assert(psvr2_load_le64(patch_payload + 80) == PSVR2_PATCH_SC);
    assert(psvr2_load_le64(patch_payload + 88) ==
           UINT32_C(0xd503201f));
    assert(psvr2_load_le64(patch_payload + 96) == PSVR2_STR_SC);
    assert(psvr2_load_le64(patch_payload + 104) == registers.x21);
    assert(psvr2_load_le64(patch_payload + 112) == registers.x22);
    assert(psvr2_load_le64(patch_payload + 192) == PSVR2_STACK_COOKIE);

    /*
     * Mode 1 ends at the sub_410 frame boundary.  The patch helper restores
     * X20/X22 and clean_return resumes the untouched completion unwind.
     */
    assert(sizeof(patch_payload) == 0xc8U);

    uint8_t patch[PSVR2_PATCH_SC_SIZE];
    assert(psvr2_build_patch_helper(&c, patch));
    assert(psvr2_load_le32(patch + 0) == UINT32_C(0x2a1303e1));
    assert(psvr2_load_le32(patch + 4) == UINT32_C(0xaa1403e0));
    uint32_t patch_call;
    uint32_t patch_return;
    assert(psvr2_arm64_branch(
        PSVR2_PATCH_SC + 8, PSVR2_PATCH_TEXT, true, &patch_call));
    assert(psvr2_arm64_branch(
        PSVR2_PATCH_SC + 20, c.fw.clean_return, false, &patch_return));
    assert(psvr2_load_le32(patch + 8) == patch_call);
    assert(psvr2_load_le32(patch + 12) == UINT32_C(0xf9403eb4));
    assert(psvr2_load_le32(patch + 16) == UINT32_C(0xd503201f));
    assert(psvr2_load_le32(patch + 20) == patch_return);

    uint8_t batch_patch[PSVR2_BATCH_PATCH_SC_SIZE];
    assert(psvr2_build_batch_patch_helper(&c, batch_patch));
    const uint32_t batch_words[] = {
        UINT32_C(0xb9400261), UINT32_C(0x91001273),
        UINT32_C(0xaa1403e0), 0,
        UINT32_C(0x91001294), UINT32_C(0xd10402d6),
        UINT32_C(0xd348fec0), UINT32_C(0xb5ffff20),
        UINT32_C(0xf9403eb4), UINT32_C(0xd503201f),
        0, UINT32_C(0xd503201f),
        UINT32_C(0xd503201f), UINT32_C(0xd503201f)
    };
    uint32_t batch_call;
    uint32_t batch_return;
    assert(psvr2_arm64_branch(
        PSVR2_BATCH_PATCH_SC + 12,
        PSVR2_PATCH_TEXT, true, &batch_call));
    assert(psvr2_arm64_branch(
        PSVR2_BATCH_PATCH_SC + 40,
        c.fw.clean_return, false, &batch_return));
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(batch_words); ++index) {
        uint32_t expected = batch_words[index];
        if (index == 3) expected = batch_call;
        if (index == 10) expected = batch_return;
        assert(psvr2_load_le32(batch_patch + index * 4U) == expected);
    }

    uint8_t read_payload[PSVR2_DIRECT_READ_PAYLOAD_SIZE];
    const uint64_t read_target = UINT64_C(0xffffffc000305778);
    assert(psvr2_build_direct_read_payload(
               &c, read_payload, read_target,
               PSVR2_DIRECT_READ_BLOCK_SC) ==
           sizeof(read_payload));
    assert(read_payload[0] == 0xf0 && read_payload[1] == 0x02);
    assert(psvr2_load_le64(read_payload + 64) == PSVR2_STACK_COOKIE);
    assert(psvr2_load_le64(read_payload + 72) == read_target);
    assert(psvr2_load_le64(read_payload + 80) ==
           PSVR2_DIRECT_READ_BLOCK_SC);
    for (size_t index = 2; index < 64; ++index)
        assert(read_payload[index] == 0);
    assert(!psvr2_build_direct_read_payload(
        &c, read_payload, 0, PSVR2_DIRECT_READ_BYTE_SC));
    assert(!psvr2_build_direct_read_payload(
        &c, read_payload, read_target, PSVR2_PATCH_SC));

    uint8_t recorded_cookie[PSVR2_AUTH_COOKIE_PROBE_SIZE];
    uint8_t wrong_cookie[PSVR2_AUTH_COOKIE_PROBE_SIZE];
    assert(psvr2_build_auth_cookie_probe(
               &c, false, recorded_cookie) == sizeof(recorded_cookie));
    assert(psvr2_build_auth_cookie_probe(
               &c, true, wrong_cookie) == sizeof(wrong_cookie));
    assert(recorded_cookie[0] == 0xf0 && recorded_cookie[1] == 0x02);
    for (size_t index = 2; index < 64; ++index) {
        assert(recorded_cookie[index] == 0);
        assert(wrong_cookie[index] == 0);
    }
    assert(psvr2_load_le64(recorded_cookie + 64) == PSVR2_STACK_COOKIE);
    assert(psvr2_load_le64(wrong_cookie + 64) ==
           (PSVR2_STACK_COOKIE ^ UINT64_C(1)));
    assert(memcmp(recorded_cookie, wrong_cookie, 64) == 0);
    assert(recorded_cookie[64] == (uint8_t)(wrong_cookie[64] ^ 1U));
    assert(memcmp(recorded_cookie + 65, wrong_cookie + 65,
                  sizeof(recorded_cookie) - 65U) == 0);
    assert(!psvr2_build_auth_cookie_probe(NULL, false, recorded_cookie));
    assert(!psvr2_build_auth_cookie_probe(&c, false, NULL));

    uint8_t direct_read[PSVR2_DIRECT_READ_SC_SIZE];
    assert(psvr2_build_direct_read_helper(&c, direct_read));
    assert(PSVR2_AUTH_RESPONSE_HEADER_SIZE +
               PSVR2_DIRECT_READ_BLOCK_SIZE ==
           PSVR2_COMPOSITE_EP0_TRANSFER_LIMIT);
    assert(PSVR2_DIRECT_READ_BLOCK_SIZE == 63U * 16U);
    const uint32_t direct_words[] = {
        UINT32_C(0xf9400260), UINT32_C(0x91004000),
        UINT32_C(0x394003a1), UINT32_C(0x39000001), 0,
        UINT32_C(0xf9400260), UINT32_C(0x91004000),
        UINT32_C(0xf94003a1), UINT32_C(0xf9000001), 0,
        UINT32_C(0xf9400260), UINT32_C(0x91004000),
        UINT32_C(0xaa1d03e1), UINT32_C(0xd28007e2),
        UINT32_C(0xa8c11023), UINT32_C(0xa8811003),
        UINT32_C(0xf1000442), UINT32_C(0x54ffffa1),
        0, 0, 0
    };
    uint32_t byte_resume;
    uint32_t qword_resume;
    uint32_t block_resume;
    assert(psvr2_arm64_branch(
        PSVR2_DIRECT_READ_BYTE_SC + 16,
        PSVR2_MTU3_COMPLETE_RESUME, false, &byte_resume));
    assert(psvr2_arm64_branch(
        PSVR2_DIRECT_READ_QWORD_SC + 16,
        PSVR2_MTU3_COMPLETE_RESUME, false, &qword_resume));
    assert(psvr2_arm64_branch(
        PSVR2_DIRECT_READ_BLOCK_SC + 32,
        PSVR2_MTU3_COMPLETE_RESUME, false, &block_resume));
    for (size_t index = 0;
         index < PSVR2_ARRAY_LEN(direct_words); ++index) {
        uint32_t expected = direct_words[index];
        if (index == 4) expected = byte_resume;
        if (index == 9) expected = qword_resume;
        if (index == 18) expected = block_resume;
        assert(psvr2_load_le32(direct_read + index * 4U) == expected);
    }
    assert(PSVR2_BATCH_PATCH_SC + PSVR2_BATCH_PATCH_SC_SIZE ==
           PSVR2_DIRECT_READ_SC);
    assert(PSVR2_DIRECT_READ_SC + PSVR2_DIRECT_READ_SC_SIZE <=
           PSVR2_PATCH_SC);

    uint8_t direct_context[PSVR2_DIRECT_CONTEXT_SC_SIZE];
    assert(psvr2_build_direct_context_helper(&c, direct_context));
    const uint32_t context_words[] = {
        UINT32_C(0xf9400260), UINT32_C(0xa9015013),
        UINT32_C(0xa9025815), 0
    };
    uint32_t context_resume;
    assert(psvr2_arm64_branch(
        PSVR2_DIRECT_CONTEXT_SC + 12,
        PSVR2_MTU3_COMPLETE_RESUME, false, &context_resume));
    for (size_t index = 0;
         index < PSVR2_ARRAY_LEN(context_words); ++index) {
        uint32_t expected = context_words[index];
        if (index == 3) expected = context_resume;
        assert(psvr2_load_le32(direct_context + index * 4U) == expected);
    }
    assert(PSVR2_DIRECT_CONTEXT_SC + PSVR2_DIRECT_CONTEXT_SC_SIZE <=
           PSVR2_WORKSPACE_SLOT);

    uint8_t allocator[PSVR2_ALLOC_HELPER_SIZE];
    assert(psvr2_build_workspace_allocator(&c, allocator));
    const uint32_t allocator_words[] = {
        UINT32_C(0xaa1303e0), UINT32_C(0x52800401),
        UINT32_C(0x72a04101), 0,
        UINT32_C(0xaa0003f3), UINT32_C(0x58000134),
        UINT32_C(0xaa1403e0), UINT32_C(0x2a1303e1),
        0, UINT32_C(0x91001280),
        UINT32_C(0xd360fe61), 0,
        UINT32_C(0xf9403eb4), 0
    };
    uint32_t allocator_kmalloc;
    uint32_t allocator_patch_low;
    uint32_t allocator_patch_high;
    uint32_t allocator_return;
    assert(psvr2_arm64_branch(
        PSVR2_ALLOC_SC + 12, PSVR2_KMALLOC,
        true, &allocator_kmalloc));
    assert(psvr2_arm64_branch(
        PSVR2_ALLOC_SC + 32, PSVR2_PATCH_TEXT,
        true, &allocator_patch_low));
    assert(psvr2_arm64_branch(
        PSVR2_ALLOC_SC + 44, PSVR2_PATCH_TEXT,
        true, &allocator_patch_high));
    assert(psvr2_arm64_branch(
        PSVR2_ALLOC_SC + 52, c.fw.clean_return,
        false, &allocator_return));
    for (size_t index = 0;
         index < PSVR2_ARRAY_LEN(allocator_words); ++index) {
        uint32_t expected = allocator_words[index];
        if (index == 3) expected = allocator_kmalloc;
        if (index == 8) expected = allocator_patch_low;
        if (index == 11) expected = allocator_patch_high;
        if (index == 13) expected = allocator_return;
        assert(psvr2_load_le32(allocator + index * 4U) == expected);
    }
    assert(psvr2_load_le64(allocator + 56) == PSVR2_WORKSPACE_SLOT);

    c.runtime_work = UINT64_C(0xffffffc012340000);
    c.runtime_data =
        c.runtime_work + PSVR2_RUNTIME_DATA_OFFSET;
    uint8_t worker[PSVR2_UMH_WORKER_SIZE];
    assert(psvr2_build_umh_worker(&c, worker));
    uint32_t worker_data_literal;
    uint32_t worker_call;
    assert(psvr2_arm64_ldr_literal(
        19, c.umh_worker + 12, c.umh_worker + 56,
        &worker_data_literal));
    assert(psvr2_arm64_branch(
        c.umh_worker + 32, PSVR2_CALL_UMH, true, &worker_call));
    assert(psvr2_load_le32(worker + 0) == UINT32_C(0xa9be7bfd));
    assert(psvr2_load_le32(worker + 12) == worker_data_literal);
    assert(psvr2_load_le32(worker + 28) == UINT32_C(0xd2800043));
    assert(psvr2_load_le32(worker + 32) == worker_call);
    assert(psvr2_load_le32(worker + 36) == UINT32_C(0xf9007a60));
    assert(psvr2_load_le32(worker + 48) == UINT32_C(0xd65f03c0));
    assert(psvr2_load_le64(worker + 56) == c.runtime_data);

    uint8_t trigger[PSVR2_UMH_TRIGGER_SIZE];
    assert(psvr2_build_umh_trigger(&c, trigger));
    uint32_t trigger_work_literal;
    uint32_t trigger_queue_literal;
    uint32_t trigger_queue_call;
    uint32_t trigger_return;
    assert(psvr2_arm64_ldr_literal(
        2, c.umh_trigger, c.umh_trigger + 40,
        &trigger_work_literal));
    assert(psvr2_arm64_ldr_literal(
        1, c.umh_trigger + 4, c.umh_trigger + 48,
        &trigger_queue_literal));
    assert(psvr2_arm64_branch(
        c.umh_trigger + 16, PSVR2_QUEUE_WORK_ON,
        true, &trigger_queue_call));
    assert(psvr2_arm64_branch(
        c.umh_trigger + 28, c.fw.clean_return,
        false, &trigger_return));
    assert(psvr2_load_le32(trigger + 0) == trigger_work_literal);
    assert(psvr2_load_le32(trigger + 4) == trigger_queue_literal);
    assert(psvr2_load_le32(trigger + 16) == trigger_queue_call);
    assert(psvr2_load_le32(trigger + 20) == UINT32_C(0xf9403eb4));
    assert(psvr2_load_le32(trigger + 28) == trigger_return);
    assert(psvr2_load_le64(trigger + 40) == c.runtime_work);
    assert(psvr2_load_le64(trigger + 48) == c.fw.system_wq);

    uint8_t str[PSVR2_STR_HELPER_SIZE];
    assert(psvr2_build_str_helper(&c, str));
    assert(psvr2_load_le32(str + 0) == UINT32_C(0xf9000293));
    assert(psvr2_load_le32(str + 4) == UINT32_C(0xf9403eb4));
    assert(psvr2_load_le32(str + 8) == UINT32_C(0xd503201f));
    uint32_t str_return;
    assert(psvr2_arm64_branch(
        c.fw.str_helper + 12, c.fw.clean_return, false, &str_return));
    assert(psvr2_load_le32(str + 12) == str_return);

    uint8_t cleanup[PSVR2_TLBI_CLEANUP_SIZE];
    assert(psvr2_build_tlbi_cleanup(
        &c, cleanup, UINT8_C(0x5a)));
    assert(psvr2_load_le32(cleanup + 0) == UINT32_C(0xf9403eb4));
    assert(psvr2_load_le32(cleanup + 4) == UINT32_C(0xaa1503e0));
    uint32_t cleanup_call;
    uint32_t cleanup_return;
    assert(psvr2_arm64_branch(
        c.fw.tlbi_cleanup_helper + 8,
        c.fw.raw_spin_lock, true, &cleanup_call));
    assert(psvr2_arm64_branch(
        c.fw.tlbi_cleanup_helper + 16,
        c.fw.mtu3_ep0_isr_epilogue, false, &cleanup_return));
    assert(psvr2_load_le32(cleanup + 8) == cleanup_call);
    assert(psvr2_load_le32(cleanup + 12) == UINT32_C(0x39033696));
    assert(psvr2_load_le32(cleanup + 16) == cleanup_return);

    uint8_t tlbi_payload[PSVR2_TLBI_PAYLOAD_SIZE];
    assert(psvr2_build_tlbi_payload(
               &c, tlbi_payload, UINT64_C(0xffffffc012340000),
               UINT64_C(0xffffffc012341000),
               UINT64_C(0xffffffc012342000),
               UINT64_C(0xffffffc012343000)) ==
           sizeof(tlbi_payload));
    assert(tlbi_payload[0] == 0xf0 && tlbi_payload[1] == 0x02);
    assert(psvr2_load_le64(tlbi_payload + 64) == PSVR2_STACK_COOKIE);
    assert(psvr2_load_le64(tlbi_payload + 80) == PSVR2_TLBI_GADGET);
    assert(psvr2_load_le64(tlbi_payload + 96) ==
           UINT64_C(0xffffffc012340000));
    assert(psvr2_load_le64(tlbi_payload + 104) ==
           UINT64_C(0xffffffc012341000));
    assert(psvr2_load_le64(tlbi_payload + 112) ==
           UINT64_C(0xffffffc012342000));
    assert(psvr2_load_le64(tlbi_payload + 120) ==
           UINT64_C(0xffffffc012343000));
    assert(sizeof(tlbi_payload) == 0x88U);
}

static void test_bootstrap_payload(void) {
    assert(!psvr2_kernel_certify_live_profile(NULL));
    psvr2_constants constants;
    assert(psvr2_constants_set_firmware(
        &constants, PSVR2_FW_0600, false));
    const uint64_t address = UINT64_C(0xffffffc000668f80);
    const uint64_t mep = UINT64_C(0xffffffc0391f5c00);
    const uint64_t pte_address = UINT64_C(0xffffffc000669006);
    uint8_t helper[PSVR2_TEMP_HELPER_SIZE];
    assert(psvr2_build_temporary_patch_helper(
        &constants, address, mep, pte_address,
        UINT8_C(0x60), UINT8_C(0x5a),
        helper));

    uint32_t instruction;
    assert(psvr2_load_le32(helper + 0) == UINT32_C(0x2a1303e1));
    assert(psvr2_arm64_branch(
        address + 8, constants.fw.patch_text, true, &instruction));
    assert(psvr2_load_le32(helper + 8) == instruction);
    assert(psvr2_load_le32(helper + 24) == UINT32_C(0x52800b41));
    assert(psvr2_load_le32(helper + PSVR2_TEMP_FINALIZER_OFFSET) ==
           UINT32_C(0xaa1503e0));
    assert(psvr2_load_le32(helper + 88) == UINT32_C(0x52800c01));
    assert(psvr2_load_le32(helper + 100) == UINT32_C(0xd503201f));
    assert(psvr2_load_le64(helper + 104) ==
           constants.fw.mtu3_ep0_isr_epilogue);
    assert(psvr2_load_le64(helper + 112) == pte_address);
    assert(psvr2_load_le64(helper + 120) == mep);

    assert(!psvr2_build_temporary_patch_helper(
        &constants, address + 1, mep, pte_address, 0, 0, helper));
    assert(!psvr2_build_temporary_patch_helper(
        &constants, address, mep, pte_address, 0, 0, NULL));
}

static void test_cold_bootstrap_payload(void) {
    enum {
        instructions_offset = 0x04,
        targets_offset = 0xa0,
        descriptor_offset = 0x1d8,
        cleanup_words = 8,
        context_words = PSVR2_DIRECT_CONTEXT_SC_SIZE / 4,
        read_words = PSVR2_DIRECT_READ_SC_SIZE / 4 - 2,
        patch_words = PSVR2_PATCH_SC_SIZE / 4,
        patch_count =
            cleanup_words + context_words + read_words + patch_words + 1
    };
    psvr2_constants constants;
    uint8_t stage[PSVR2_COLD_BOOTSTRAP_STAGE_SIZE];
    uint8_t payload[PSVR2_COLD_BOOTSTRAP_PAYLOAD_SIZE];

    assert(psvr2_constants_set_firmware(
        &constants, PSVR2_FW_0600, false));
    assert(psvr2_build_cold_bootstrap(
        &constants, stage, payload));

    assert(PSVR2_SIEUSB_COMMAND_SLOTS ==
           UINT64_C(0xffffffbffc0af1f0));
    assert(PSVR2_COLD_STATIC_STAGE ==
           PSVR2_SIEUSB_COMMAND_SLOTS + UINT64_C(0x2100));
    assert(stage[0] == PSVR2_COLD_STATIC_SLOT);
    assert(payload[0] == UINT8_C(0xf0));
    assert(payload[1] == UINT8_C(0x02));
    assert(psvr2_load_le64(payload + 64) == PSVR2_STACK_COOKIE);
    assert(psvr2_load_le64(payload + 80) == PSVR2_COLD_PATCH_EPILOGUE);
    assert(psvr2_load_le64(payload + 96) == PSVR2_COLD_PATCH_LOOP);
    assert(psvr2_load_le64(payload + 104) ==
           PSVR2_COLD_STATIC_STAGE + descriptor_offset);
    assert(psvr2_load_le64(payload + 112) == 0);

    assert(psvr2_load_le64(stage + descriptor_offset) ==
           PSVR2_COLD_STATIC_STAGE + targets_offset);
    assert(psvr2_load_le64(stage + descriptor_offset + 8) ==
           PSVR2_COLD_STATIC_STAGE + instructions_offset);
    assert(psvr2_load_le32(stage + descriptor_offset + 0x10) ==
           patch_count);
    assert(psvr2_load_le64(stage + descriptor_offset + 0x18) ==
           PSVR2_COLD_PATCH_EXIT);
    assert(psvr2_load_le32(stage + descriptor_offset + 0x20) ==
           PSVR2_COLD_PATCH_EXIT_ORIGINAL);
    assert(patch_count == 38);

    size_t final_index = patch_count - 1U;
    assert(psvr2_load_le64(stage + targets_offset) ==
           PSVR2_COLD_CLEANUP_SC);
    size_t patch_index =
        cleanup_words + context_words + read_words;
    assert(psvr2_load_le64(
               stage + targets_offset +
               patch_index * sizeof(uint64_t)) ==
           PSVR2_PATCH_SC);
    uint8_t expected_patch[PSVR2_PATCH_SC_SIZE];
    assert(psvr2_build_patch_helper(&constants, expected_patch));
    assert(psvr2_load_le32(
               stage + instructions_offset +
               patch_index * sizeof(uint32_t)) ==
           psvr2_load_le32(expected_patch));
    assert(psvr2_load_le64(
               stage + targets_offset + final_index * sizeof(uint64_t)) ==
           PSVR2_COLD_PATCH_EXIT);
    uint32_t enter_cleanup;
    assert(psvr2_arm64_branch(
        PSVR2_COLD_PATCH_EXIT, PSVR2_COLD_CLEANUP_SC,
        false, &enter_cleanup));
    assert(psvr2_load_le32(
               stage + instructions_offset +
               final_index * sizeof(uint32_t)) == enter_cleanup);

    uint32_t restore_exit;
    assert(psvr2_arm64_branch(
        PSVR2_COLD_CLEANUP_SC + 8, PSVR2_PATCH_TEXT,
        true, &restore_exit));
    assert(psvr2_load_le32(
               stage + instructions_offset + 2 * sizeof(uint32_t)) ==
           restore_exit);

    uint8_t previous_cleanup[PSVR2_COLD_CLEANUP_SIZE];
    assert(psvr2_build_cold_cleanup_helper(
        &constants, PSVR2_BATCH_PATCH_SC, previous_cleanup));
    assert(psvr2_load_le32(previous_cleanup) == UINT32_C(0xf9400e60));
    assert(!psvr2_build_cold_cleanup_helper(
        &constants, PSVR2_STR_SC, previous_cleanup));

    assert(psvr2_constants_set_firmware(
        &constants, PSVR2_FW_0600, false));
    constants.version = 0;
    assert(!psvr2_build_cold_bootstrap(
        &constants, stage, payload));
    assert(!psvr2_build_cold_bootstrap(NULL, stage, payload));

    assert(psvr2_constants_set_firmware(
        &constants, PSVR2_FW_0110, false));
    assert(psvr2_build_cold_bootstrap(
        &constants, stage, payload));
    assert(stage[0] == UINT8_C(0x10));
    assert(psvr2_load_le64(payload + 80) ==
           constants.fw.cold_patch_epilogue);
    assert(psvr2_load_le64(payload + 96) ==
           constants.fw.cold_patch_loop);
    assert(psvr2_load_le64(payload + 104) ==
           constants.fw.cold_static_stage + descriptor_offset);
    assert(psvr2_load_le64(stage + targets_offset) ==
           constants.fw.cold_cleanup_helper);
}

static void test_arm64_encoders(void) {
    uint32_t instruction = 0;
    assert(psvr2_arm64_branch(
        PSVR2_STR_SC + 12, UINT64_C(0xffffffc000305778),
        false, &instruction));
    assert(instruction == UINT32_C(0x17fe5a5f));
    assert(psvr2_arm64_branch(0x1000, 0x1004, true, &instruction));
    assert(instruction == UINT32_C(0x94000001));
    assert(!psvr2_arm64_branch(0x1001, 0x1004, false, &instruction));
    assert(!psvr2_arm64_branch(
        0x1000, UINT64_C(0x10000000), false, &instruction));
    assert(psvr2_arm64_branch(
        PSVR2_MTU3_COMPLETE_EPILOGUE,
        UINT64_C(0xffffffc000305778), false, &instruction));

    assert(psvr2_arm64_ldr_literal(19, 0x100c, 0x1038, &instruction));
    assert(instruction == UINT32_C(0x58000173));
    assert(!psvr2_arm64_ldr_literal(32, 0x100c, 0x1038, &instruction));
    assert(!psvr2_arm64_ldr_literal(1, 0x1001, 0x1038, &instruction));
}

static void test_vendor_reports(void) {
    uint8_t report[16];
    const uint8_t body[] = {0xaa, 0xbb, 0xcc};
    memset(report, 0xff, sizeof(report));
    assert(psvr2_build_vendor_report(report, sizeof(report), 0x17, 0x1234,
                                     body, sizeof(body)) == 11);
    assert(memcmp(report,
        "\x17\x00\x34\x12\x03\x00\x00\x00\xaa\xbb\xcc", 11) == 0);
    assert(psvr2_build_vendor_report(report, 8, 0x0c, 1, NULL, 0) == 8);
    assert(memcmp(report, "\x0c\x00\x01\x00\x00\x00\x00\x00", 8) == 0);
    assert(psvr2_build_vendor_report(report, 10, 1, 2,
                                     body, sizeof(body)) == 0);
    assert(psvr2_build_vendor_report(report, sizeof(report), 1, 2,
                                     NULL, 1) == 0);
    assert(psvr2_build_vendor_report(NULL, sizeof(report), 1, 2,
                                     body, sizeof(body)) == 0);
}

static void test_request_buffer_discovery(void) {
    const uint64_t base = UINT64_C(0xffffffc012340400);
    uint8_t disclosure[4096] = {0};
    const size_t self_offsets[] = {0x100, 0x188, 0x2f0, 0x3f8};
    uint64_t found = 1;
    unsigned votes = 99;

    for (size_t index = 0;
         index < PSVR2_ARRAY_LEN(self_offsets); ++index) {
        size_t offset = self_offsets[index];
        psvr2_store_le64(disclosure + offset, base + offset);
    }
    assert(psvr2_exploit_find_request_buffer(
        disclosure, sizeof(disclosure), &found, &votes));
    assert(found == base);
    assert(votes == PSVR2_ARRAY_LEN(self_offsets));

    /*
     * Observed false-positive shape: six exact relations in neighboring
     * objects begin beyond 4 KiB, but the derived base is not a 1 KiB slab
     * address and must fail closed.
     */
    uint8_t late_disclosure[16384] = {0};
    const uint64_t late_base = UINT64_C(0xffffffc0392073c8);
    const size_t late_offsets[] = {
        0x1468, 0x1c98, 0x24c8, 0x2cf8, 0x3528, 0x3d58
    };
    psvr2_store_le64(
        late_disclosure + 0x408,
        UINT64_C(0xffffffc03a9aea70));
    for (size_t index = 0;
         index < PSVR2_ARRAY_LEN(late_offsets); ++index) {
        size_t offset = late_offsets[index];
        psvr2_store_le64(
            late_disclosure + offset, late_base + offset);
    }
    found = 1;
    votes = 99;
    assert(!psvr2_exploit_find_request_buffer(
        late_disclosure, 4096, &found, &votes));
    assert(found == 0);
    assert(votes == 0);
    assert(!psvr2_exploit_find_request_buffer(
        late_disclosure, sizeof(late_disclosure), &found, &votes));
    assert(found == 0);
    assert(votes == 0);

    /* Even many exact relations cannot authorize a non-slab-aligned base. */
    memset(late_disclosure, 0, sizeof(late_disclosure));
    const uint64_t false_base = UINT64_C(0xffffffc03a113380);
    for (size_t index = 0; index < 23; ++index) {
        size_t offset = 0x100 + index * 0x80;
        psvr2_store_le64(
            late_disclosure + offset, false_base + offset);
    }
    found = 1;
    votes = 99;
    assert(!psvr2_exploit_find_request_buffer(
        late_disclosure, sizeof(late_disclosure), &found, &votes));
    assert(found == 0);
    assert(votes == 0);

    memset(disclosure, 0, sizeof(disclosure));
    for (size_t index = 0;
         index < PSVR2_ARRAY_LEN(self_offsets); ++index) {
        size_t offset = self_offsets[index];
        psvr2_store_le64(
            disclosure + offset, base + 8U + offset);
    }
    found = 1;
    votes = 99;
    assert(!psvr2_exploit_find_request_buffer(
        disclosure, sizeof(disclosure), &found, &votes));
    assert(found == 0);
    assert(votes == 0);

    /*
     * An empty list_head duplicates each self-pointer.  The second word of
     * every pair creates a base-8 candidate with the same raw vote count;
     * the first-word pair anchors must select the real base.
     */
    memset(disclosure, 0, sizeof(disclosure));
    for (size_t index = 0;
         index < PSVR2_ARRAY_LEN(self_offsets); ++index) {
        size_t offset = self_offsets[index];
        psvr2_store_le64(disclosure + offset, base + offset);
        psvr2_store_le64(
            disclosure + offset + sizeof(uint64_t),
            base + offset);
    }
    found = 1;
    votes = 99;
    assert(psvr2_exploit_find_request_buffer(
        disclosure, sizeof(disclosure), &found, &votes));
    assert(found == base);
    assert(votes == PSVR2_ARRAY_LEN(self_offsets));

    psvr2_store_le64(disclosure + self_offsets[3], 0);
    found = 1;
    votes = 99;
    assert(!psvr2_exploit_find_request_buffer(
        disclosure, sizeof(disclosure), &found, &votes));
    assert(found == 0);
    assert(votes == 3);
    assert(!psvr2_exploit_find_request_buffer(
        NULL, sizeof(disclosure), &found, &votes));
    assert(!psvr2_exploit_find_request_buffer(
        disclosure, sizeof(disclosure), NULL, &votes));

    /*
     * A run of pointers into one page does not locate the disclosure
     * itself. The pointer-bearing object may reference a separate slab
     * page, so this shape must never authorize the cold bootstrap.
     */
    memset(disclosure, 0, sizeof(disclosure));
    const size_t table_offset = 0x400;
    const uint64_t referenced_page = UINT64_C(0xffffffc03a111000);
    for (size_t index = 0; index < 10; ++index) {
        psvr2_store_le64(
            disclosure + table_offset + index * sizeof(uint64_t),
            referenced_page + 0x430 + index * 0x40);
    }
    found = 1;
    votes = 99;
    assert(!psvr2_exploit_find_request_buffer(
        disclosure, sizeof(disclosure), &found, &votes));
    assert(found == 0);
    assert(votes == 0);

    /* Two equally supported self-maps are unsafe: fail closed. */
    memset(disclosure, 0, sizeof(disclosure));
    const uint64_t other_base = UINT64_C(0xffffffc012350400);
    const size_t other_offsets[] = {0x600, 0x788, 0x8f0, 0x9f8};
    for (size_t index = 0;
         index < PSVR2_ARRAY_LEN(self_offsets); ++index) {
        size_t offset = self_offsets[index];
        psvr2_store_le64(disclosure + offset, base + offset);
        offset = other_offsets[index];
        psvr2_store_le64(
            disclosure + offset, other_base + offset);
    }
    found = 1;
    votes = 99;
    assert(!psvr2_exploit_find_request_buffer(
        disclosure, sizeof(disclosure), &found, &votes));
    assert(found == 0);
    assert(votes == PSVR2_ARRAY_LEN(self_offsets));
}

static void test_parsers(void) {
    uint32_t fw;
    uint64_t value;
    assert(psvr2_parse_firmware("01.10", &fw) && fw == PSVR2_FW_0110);
    assert(psvr2_parse_firmware("0x01100103", &fw) &&
           fw == PSVR2_FW_0110);
    assert(psvr2_parse_firmware("06.00", &fw) && fw == PSVR2_FW_0600);
    assert(psvr2_parse_firmware("0x06000102", &fw) && fw == PSVR2_FW_0600);
    assert(!psvr2_parse_firmware("not-a-version", &fw));
    assert(psvr2_parse_u64("0x1234", &value) && value == 0x1234);
    assert(psvr2_parse_u64("42  ", &value) && value == 42);
    assert(psvr2_parse_u64("  +42", &value) && value == 42);
    assert(!psvr2_parse_u64("-1", &value));
    assert(!psvr2_parse_u64("  -1", &value));
    assert(!psvr2_parse_u64("12oops", &value));
    assert(!psvr2_parse_u64("", &value));
    assert(!psvr2_parse_u64("18446744073709551616", &value));
}

static void test_user_memory_guards(void) {
    psvr2_constants constants;
    psvr2_device device = {.connection_generation = 1};
    assert(psvr2_constants_set_firmware(
        &constants, PSVR2_FW_0600, false));
    psvr2_exploit exploit;
    psvr2_exploit_init(&exploit, &device, &constants);
    psvr2_krw krw;
    psvr2_krw_init(&krw, &exploit);
    uint8_t byte = 0;
    const uint64_t pgd =
        constants.fw.page_offset_base + UINT64_C(0x1000);
    const uint64_t task_size =
        UINT64_C(1) << constants.fw.user_va_bits;

    assert(psvr2_user_ptwalk(&krw, pgd, task_size) == 0);
    assert(psvr2_user_ptwalk(&krw, pgd + 1, 0) == 0);
    assert(!psvr2_user_read(&krw, pgd, task_size, &byte, 1));
    assert(!psvr2_user_read(
        &krw, pgd, task_size - 1, &byte, 2));
    assert(!psvr2_user_read(
        &krw, constants.fw.page_offset_base - PSVR2_PAGE_SIZE,
        0, &byte, 1));
}

static void test_buffers_and_paths(void) {
    psvr2_buffer buf = {0};
    assert(!psvr2_buffer_reserve(NULL, 1));
    assert(psvr2_buffer_append(&buf, NULL, 0));
    assert(buf.data == NULL && buf.len == 0 && buf.cap == 0);
    assert(!psvr2_buffer_append(NULL, "x", 1));
    assert(!psvr2_buffer_append(&buf, NULL, 1));
    assert(psvr2_buffer_append(&buf, "abc", 3));
    assert(psvr2_buffer_append(&buf, "def", 3));
    assert(buf.len == 6 && memcmp(buf.data, "abcdef", 6) == 0);
    assert(buf.cap >= buf.len);
    psvr2_buffer_free(&buf);
    assert(buf.data == NULL && buf.len == 0 && buf.cap == 0);

    assert(psvr2_decode_hex("00aF10", &buf));
    assert(buf.len == 3);
    assert(memcmp(buf.data, "\x00\xaf\x10", 3) == 0);
    assert(psvr2_decode_hex("42", &buf));
    assert(buf.len == 4 && buf.data[3] == 0x42);
    assert(!psvr2_decode_hex("abc", &buf));
    assert(!psvr2_decode_hex("xx", &buf));
    assert(buf.len == 4 && buf.data[3] == 0x42);
    assert(!psvr2_decode_hex(NULL, &buf));
    assert(!psvr2_decode_hex("00", NULL));
    assert(!psvr2_read_file(NULL, &buf));
    assert(!psvr2_read_file("does-not-exist", NULL));
    psvr2_buffer_free(&buf);

    assert(strcmp(psvr2_basename(NULL), "") == 0);
    assert(strcmp(psvr2_basename("plain"), "plain") == 0);
    assert(strcmp(psvr2_basename("/tmp/file.bin"), "file.bin") == 0);
    assert(strcmp(psvr2_basename("C:\\tmp\\file.bin"), "file.bin") == 0);
    assert(strcmp(psvr2_basename("/mixed\\last.bin"), "last.bin") == 0);
}

static void test_sha256_and_persistence_parsers(void) {
    uint8_t digest[32];
    char hex[65];
    assert(psvr2_sha256("", 0, digest));
    psvr2_sha256_hex(digest, hex);
    assert(strcmp(
        hex,
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855") == 0);
    assert(psvr2_sha256("abc", 3, digest));
    psvr2_sha256_hex(digest, hex);
    assert(strcmp(
        hex,
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad") == 0);
    assert(!psvr2_sha256(NULL, 1, digest));
    assert(!psvr2_sha256("", 0, NULL));

    uint64_t fixture_size = 0;
    assert(psvr2_sha256_file(
        PSVR2_TEST_FIXTURES_DIR "/sha256-abc.txt", digest, &fixture_size));
    assert(fixture_size == 3);
    psvr2_sha256_hex(digest, hex);
    assert(strcmp(hex,
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad") == 0);
    assert(!psvr2_sha256_file("does-not-exist", digest, &fixture_size));

    assert(psvr2_persist_name_valid("stage1.ko"));
    assert(psvr2_persist_name_valid("custom-module_2.ko"));
    assert(!psvr2_persist_name_valid(""));
    assert(!psvr2_persist_name_valid("."));
    assert(!psvr2_persist_name_valid(".."));
    assert(!psvr2_persist_name_valid("../stage1.ko"));
    assert(!psvr2_persist_name_valid("stage1.ko;reboot"));
    assert(!psvr2_persist_name_valid("directory/stage1.ko"));

    char parsed[65];
    assert(psvr2_persist_parse_sha256(
        "BA7816BF8F01CFEA414140DE5DAE2223"
        "B00361A396177A9CB410FF61F20015AD  file\n",
        parsed));
    assert(strcmp(
        parsed,
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad") == 0);
    assert(!psvr2_persist_parse_sha256("abc", parsed));
    assert(!psvr2_persist_parse_sha256(
        "za7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad",
        parsed));

    assert(psvr2_persist_mount_matches(
        "/dev/mmcblk0p3 /tmp/.psvr2-persist-mount "
        "vfat ro,nosuid,nodev,noexec 0 0\n",
        true));
    assert(psvr2_persist_mount_matches(
        "tmpfs /tmp tmpfs rw 0 0\n"
        "/dev/mmcblk0p3 /tmp/.psvr2-persist-mount "
        "vfat rw,nosuid,nodev,noexec 0 0\n",
        false));
    assert(!psvr2_persist_mount_matches(
        "/dev/mmcblk0p15 /tmp/.psvr2-persist-mount "
        "vfat ro 0 0\n",
        true));
    assert(!psvr2_persist_mount_matches(
        "/dev/mmcblk0p3 /tmp/wrong vfat ro 0 0\n",
        true));
    assert(!psvr2_persist_mount_matches(
        "/dev/mmcblk0p3 /tmp/.psvr2-persist-mount "
        "ext4 ro 0 0\n",
        true));
    assert(!psvr2_persist_mount_matches(
        "/dev/mmcblk0p3 /tmp/.psvr2-persist-mount "
        "vfat rw 0 0\n",
        true));

    assert(psvr2_persist_firmware_supported(
        PSVR2_FW_0110, false));
    assert(psvr2_persist_firmware_supported(
        PSVR2_FW_0600, false));
    assert(!psvr2_persist_firmware_supported(
        PSVR2_FW_0110, true));
    assert(!psvr2_persist_firmware_supported(
        PSVR2_FW_0600, true));
    assert(!psvr2_persist_firmware_supported(
        UINT32_C(0x02000100), false));
}

int main(void) {
    test_endian();
    test_structured_results();
    test_auth_reports();
    test_gaze_protocol();
    test_gaze_generic_packet();
    test_gaze_probe_transfer_classifier();
    test_firmware();
    test_payloads();
    test_bootstrap_payload();
    test_cold_bootstrap_payload();
    test_arm64_encoders();
    test_vendor_reports();
    test_request_buffer_discovery();
    test_mailbox_and_pte();
    test_parsers();
    test_user_memory_guards();
    test_buffers_and_paths();
    test_sha256_and_persistence_parsers();
    test_shell_dispatch();
    test_line_history();
    test_shellcode_safety_gate();
    test_maintenance_helpers();
    puts("all tests passed");
    return 0;
}
