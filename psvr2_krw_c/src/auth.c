#include "psvr2/auth.h"

#include <string.h>

enum {
    AUTH_RECORD_SIZE = 180,
    AUTH_CHALLENGE_OFFSET = 4,
    AUTH_CHALLENGE_SIZE = 16,
    AUTH_CERTIFICATE_OFFSET = 20,
    AUTH_REPORT_DATA_OFFSET = 4,
    AUTH_REPORT_CRC_OFFSET = 60
};

_Static_assert(
    PSVR2_AUTH_CERTIFICATE_BLOCKS * PSVR2_AUTH_BLOCK_DATA_SIZE >=
        AUTH_RECORD_SIZE,
    "authentication reports must cover the challenge record");
_Static_assert(
    AUTH_REPORT_CRC_OFFSET + sizeof(uint32_t) == PSVR2_AUTH_REPORT_SIZE,
    "authentication CRC must end the report");

uint32_t psvr2_auth_crc32(const void *data, size_t length) {
    if (!data && length) return 0;
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^
                  (UINT32_C(0xedb88320) &
                   (uint32_t)-(int32_t)(crc & 1U));
    }
    return ~crc;
}

void psvr2_auth_finish_report(
    uint8_t report[PSVR2_AUTH_REPORT_SIZE]) {
    if (!report) return;
    uint32_t crc = psvr2_auth_crc32(report, AUTH_REPORT_CRC_OFFSET);
    report[AUTH_REPORT_CRC_OFFSET + 0] = (uint8_t)(crc >> 24);
    report[AUTH_REPORT_CRC_OFFSET + 1] = (uint8_t)(crc >> 16);
    report[AUTH_REPORT_CRC_OFFSET + 2] = (uint8_t)(crc >> 8);
    report[AUTH_REPORT_CRC_OFFSET + 3] = (uint8_t)crc;
}

bool psvr2_auth_build_certificate_report(
    uint8_t report[PSVR2_AUTH_REPORT_SIZE],
    const uint8_t certificate[PSVR2_AUTH_CERTIFICATE_SIZE],
    uint8_t authentication_id, unsigned block) {
    if (!report || !certificate ||
        block >= PSVR2_AUTH_CERTIFICATE_BLOCKS)
        return false;

    uint8_t record[AUTH_RECORD_SIZE] = {0};
    memcpy(record + AUTH_CHALLENGE_OFFSET, certificate,
           AUTH_CHALLENGE_SIZE);
    memcpy(record + AUTH_CERTIFICATE_OFFSET, certificate,
           PSVR2_AUTH_CERTIFICATE_SIZE);

    memset(report, 0, PSVR2_AUTH_REPORT_SIZE);
    report[0] = UINT8_C(0xf0);
    report[1] = 1;
    report[2] = authentication_id;
    report[3] = (uint8_t)block;
    size_t source_offset = block * PSVR2_AUTH_BLOCK_DATA_SIZE;
    size_t remaining = AUTH_RECORD_SIZE -
        psvr2_min_size(source_offset, AUTH_RECORD_SIZE);
    size_t copied = psvr2_min_size(
        remaining, PSVR2_AUTH_BLOCK_DATA_SIZE);
    if (copied)
        memcpy(report + AUTH_REPORT_DATA_OFFSET,
               record + source_offset, copied);
    psvr2_auth_finish_report(report);
    memset(record, 0, sizeof(record));
    return true;
}

bool psvr2_auth_parse_status_report(
    const void *data, size_t length, uint8_t *state) {
    return psvr2_auth_parse_status_report_id(
        data, length, NULL, state);
}

bool psvr2_auth_parse_status_report_id(
    const void *data, size_t length, uint8_t *authentication_id,
    uint8_t *state) {
    if (!data || !state || length != PSVR2_AUTH_STATUS_REPORT_SIZE)
        return false;
    const uint8_t *bytes = data;
    if (bytes[0] != UINT8_C(0xf2) || bytes[1] != 0)
        return false;
    if (authentication_id) *authentication_id = bytes[2];
    *state = bytes[3];
    return true;
}
