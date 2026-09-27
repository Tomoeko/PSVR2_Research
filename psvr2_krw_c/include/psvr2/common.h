#ifndef PSVR2_COMMON_H
#define PSVR2_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32)
#  define PSVR2_PATH_SEP '\\'
#  define PSVR2_EXPORT __declspec(dllexport)
#else
#  define PSVR2_PATH_SEP '/'
#  define PSVR2_EXPORT
#endif

#define PSVR2_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
typedef enum {
    PSVR2_WALK_COMPLETE,
    PSVR2_WALK_CALLBACK_STOPPED,
    PSVR2_WALK_CYCLE,
    PSVR2_WALK_LIMIT,
    PSVR2_WALK_READ_ERROR,
    PSVR2_WALK_INVALID
} psvr2_walk_result;

typedef enum {
    PSVR2_STATUS_OK,
    PSVR2_STATUS_INVALID_ARGUMENT,
    PSVR2_STATUS_NOT_CONNECTED,
    PSVR2_STATUS_USB_ERROR,
    PSVR2_STATUS_SHORT_TRANSFER,
    PSVR2_STATUS_UNSUPPORTED,
    PSVR2_STATUS_VALIDATION_FAILED,
    PSVR2_STATUS_STATE_CHANGED,
    PSVR2_STATUS_RECOVERY_REQUIRED,
    PSVR2_STATUS_IO_ERROR,
    PSVR2_STATUS_OUT_OF_MEMORY,
    PSVR2_STATUS_INTERNAL_ERROR
} psvr2_status;

#define PSVR2_RESULT_MESSAGE_SIZE 192U

typedef struct {
    psvr2_status status;
    int native_code;
    char message[PSVR2_RESULT_MESSAGE_SIZE];
} psvr2_result;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} psvr2_buffer;

uint16_t psvr2_load_le16(const void *p);
uint32_t psvr2_load_le32(const void *p);
uint64_t psvr2_load_le64(const void *p);
void psvr2_store_le16(void *p, uint16_t v);
void psvr2_store_le32(void *p, uint32_t v);
void psvr2_store_le64(void *p, uint64_t v);
double psvr2_now(void);
void psvr2_report_timing(
    const char *label, double started, bool succeeded);
void psvr2_sleep_ms(unsigned ms);
size_t psvr2_min_size(size_t a, size_t b);
uint64_t psvr2_min_u64(uint64_t a, uint64_t b);
double psvr2_max_double(double a, double b);
const char *psvr2_walk_result_name(psvr2_walk_result result);
const char *psvr2_status_name(psvr2_status status);
psvr2_result psvr2_result_ok(void);
psvr2_result psvr2_result_error(
    psvr2_status status, int native_code, const char *format, ...);
bool psvr2_result_is_ok(const psvr2_result *result);
bool psvr2_buffer_reserve(psvr2_buffer *buf, size_t cap);
bool psvr2_buffer_append(psvr2_buffer *buf, const void *data, size_t len);
void psvr2_buffer_free(psvr2_buffer *buf);
char *psvr2_strdup(const char *s);
char *psvr2_trim(char *s);
bool psvr2_parse_u64(const char *s, uint64_t *out);
bool psvr2_decode_hex(const char *hex, psvr2_buffer *out);
bool psvr2_read_file(const char *path, psvr2_buffer *out);
bool psvr2_write_file(const char *path, const void *data, size_t len);
bool psvr2_sha256(const void *data, size_t len, uint8_t digest[32]);
bool psvr2_sha256_file(
    const char *path, uint8_t digest[32], uint64_t *size_out);
void psvr2_sha256_hex(const uint8_t digest[32], char output[65]);
const char *psvr2_basename(const char *path);
bool psvr2_file_exists(const char *path);
void psvr2_hexdump(FILE *out, uint64_t base, const uint8_t *data, size_t len);

#endif
