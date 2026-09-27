#ifndef PSVR2_AUTH_H
#define PSVR2_AUTH_H

#include "psvr2/common.h"

#define PSVR2_AUTH_REPORT_SIZE 64U
#define PSVR2_AUTH_CERTIFICATE_SIZE 160U
#define PSVR2_AUTH_CERTIFICATE_BLOCKS 4U
#define PSVR2_AUTH_BLOCK_DATA_SIZE 56U
#define PSVR2_AUTH_STATUS_REPORT_SIZE 16U

#define PSVR2_AUTH_STATE_WAIT_FIRST_CHALLENGE 0x10U
#define PSVR2_AUTH_STATE_READY_FIRST_RESPONSE 0x12U
#define PSVR2_AUTH_STATE_WAIT_HOST_RESPONSE 0x20U

uint32_t psvr2_auth_crc32(const void *data, size_t length);
void psvr2_auth_finish_report(
    uint8_t report[PSVR2_AUTH_REPORT_SIZE]);
bool psvr2_auth_build_certificate_report(
    uint8_t report[PSVR2_AUTH_REPORT_SIZE],
    const uint8_t certificate[PSVR2_AUTH_CERTIFICATE_SIZE],
    uint8_t authentication_id, unsigned block);
bool psvr2_auth_parse_status_report(
    const void *data, size_t length, uint8_t *state);
bool psvr2_auth_parse_status_report_id(
    const void *data, size_t length, uint8_t *authentication_id,
    uint8_t *state);

#endif
