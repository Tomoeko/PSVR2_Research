#include "psvr2/common.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

uint16_t psvr2_load_le16(const void *p) {
    const uint8_t *b = p;
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

uint32_t psvr2_load_le32(const void *p) {
    const uint8_t *b = p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

uint64_t psvr2_load_le64(const void *p) {
    const uint8_t *b = p;
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)b[i] << (i * 8U);
    return v;
}

void psvr2_store_le16(void *p, uint16_t v) {
    uint8_t *b = p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

void psvr2_store_le32(void *p, uint32_t v) {
    uint8_t *b = p;
    for (unsigned i = 0; i < 4; ++i) b[i] = (uint8_t)(v >> (i * 8U));
}

void psvr2_store_le64(void *p, uint64_t v) {
    uint8_t *b = p;
    for (unsigned i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (i * 8U));
}

double psvr2_now(void) {
#if defined(_WIN32)
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#endif
}

void psvr2_report_timing(
    const char *label, double started, bool succeeded) {
    double elapsed = psvr2_now() - started;
    if (elapsed < 0.0) elapsed = 0.0;
    printf("    [time] %s: %.3fs (%s)\n",
           label ? label : "step", elapsed,
           succeeded ? "ok" : "failed");
    fflush(stdout);
}

void psvr2_sleep_ms(unsigned ms) {
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts = {(time_t)(ms / 1000U), (long)(ms % 1000U) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
#endif
}

size_t psvr2_min_size(size_t a, size_t b) {
    return a < b ? a : b;
}

uint64_t psvr2_min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

double psvr2_max_double(double a, double b) {
    return a > b ? a : b;
}

const char *psvr2_walk_result_name(psvr2_walk_result result) {
    switch (result) {
        case PSVR2_WALK_COMPLETE: return "complete";
        case PSVR2_WALK_CALLBACK_STOPPED: return "callback stopped";
        case PSVR2_WALK_CYCLE: return "cycle";
        case PSVR2_WALK_LIMIT: return "limit";
        case PSVR2_WALK_READ_ERROR: return "read error";
        case PSVR2_WALK_INVALID: return "invalid";
    }
    return "unknown";
}

const char *psvr2_status_name(psvr2_status status) {
    switch (status) {
        case PSVR2_STATUS_OK: return "ok";
        case PSVR2_STATUS_INVALID_ARGUMENT: return "invalid argument";
        case PSVR2_STATUS_NOT_CONNECTED: return "not connected";
        case PSVR2_STATUS_USB_ERROR: return "USB error";
        case PSVR2_STATUS_SHORT_TRANSFER: return "short transfer";
        case PSVR2_STATUS_UNSUPPORTED: return "unsupported";
        case PSVR2_STATUS_VALIDATION_FAILED: return "validation failed";
        case PSVR2_STATUS_STATE_CHANGED: return "state changed";
        case PSVR2_STATUS_RECOVERY_REQUIRED: return "recovery required";
        case PSVR2_STATUS_IO_ERROR: return "I/O error";
        case PSVR2_STATUS_OUT_OF_MEMORY: return "out of memory";
        case PSVR2_STATUS_INTERNAL_ERROR: return "internal error";
    }
    return "unknown";
}

psvr2_result psvr2_result_ok(void) {
    return (psvr2_result){.status = PSVR2_STATUS_OK};
}

psvr2_result psvr2_result_error(
    psvr2_status status, int native_code, const char *format, ...) {
    psvr2_result result = {
        .status = status == PSVR2_STATUS_OK
                      ? PSVR2_STATUS_INTERNAL_ERROR : status,
        .native_code = native_code
    };
    if (format) {
        va_list arguments;
        va_start(arguments, format);
        (void)vsnprintf(
            result.message, sizeof(result.message), format, arguments);
        va_end(arguments);
    }
    return result;
}

bool psvr2_result_is_ok(const psvr2_result *result) {
    return result && result->status == PSVR2_STATUS_OK;
}

bool psvr2_buffer_reserve(psvr2_buffer *buf, size_t cap) {
    if (!buf) return false;
    if (cap <= buf->cap) return true;
    size_t next = buf->cap ? buf->cap : 256;
    while (next < cap) {
        if (next > SIZE_MAX / 2) { next = cap; break; }
        next *= 2;
    }
    void *p = realloc(buf->data, next);
    if (!p) return false;
    buf->data = p;
    buf->cap = next;
    return true;
}

bool psvr2_buffer_append(psvr2_buffer *buf, const void *data, size_t len) {
    if (!buf || (len && !data)) return false;
    if (!len) return true;
    if (len > SIZE_MAX - buf->len || !psvr2_buffer_reserve(buf, buf->len + len))
        return false;
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return true;
}

void psvr2_buffer_free(psvr2_buffer *buf) {
    if (!buf) return;
    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

char *psvr2_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *psvr2_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return s;
}

bool psvr2_parse_u64(const char *s, uint64_t *out) {
    if (!s || !*s || !out) return false;
    while (isspace((unsigned char)*s)) ++s;
    if (!*s || *s == '-') return false;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    while (end && *end && isspace((unsigned char)*end)) ++end;
    if (errno || end == s || !end || *end != '\0') return false;
    *out = (uint64_t)v;
    return true;
}

bool psvr2_decode_hex(const char *hex, psvr2_buffer *out) {
    if (!hex || !out) return false;
    size_t length = strlen(hex);
    size_t original_length = out->len;
    if ((length & 1U) != 0 ||
        length / 2U > SIZE_MAX - out->len ||
        !psvr2_buffer_reserve(out, out->len + length / 2U))
        return false;
    for (size_t i = 0; i < length; i += 2) {
        int hi = isdigit((unsigned char)hex[i])
                     ? hex[i] - '0'
                     : isxdigit((unsigned char)hex[i])
                           ? 10 + tolower((unsigned char)hex[i]) - 'a'
                           : -1;
        int lo = isdigit((unsigned char)hex[i + 1])
                     ? hex[i + 1] - '0'
                     : isxdigit((unsigned char)hex[i + 1])
                           ? 10 + tolower((unsigned char)hex[i + 1]) - 'a'
                           : -1;
        if (hi < 0 || lo < 0) {
            out->len = original_length;
            return false;
        }
        out->data[out->len++] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

bool psvr2_read_file(const char *path, psvr2_buffer *out) {
    if (!path || !out) return false;
    size_t original_length = out->len;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t block[65536];
    bool ok = true;
    for (;;) {
        size_t n = fread(block, 1, sizeof(block), f);
        if (n && !psvr2_buffer_append(out, block, n)) { ok = false; break; }
        if (n < sizeof(block)) {
            if (ferror(f)) ok = false;
            break;
        }
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) out->len = original_length;
    return ok;
}

bool psvr2_write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    return ok;
}

typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
} psvr2_sha256_context;

static uint32_t sha256_rotr(uint32_t value, unsigned shift) {
    return (value >> shift) | (value << (32U - shift));
}

static void sha256_transform(
    psvr2_sha256_context *context, const uint8_t block[64]) {
    static const uint32_t constants[64] = {
        UINT32_C(0x428a2f98), UINT32_C(0x71374491),
        UINT32_C(0xb5c0fbcf), UINT32_C(0xe9b5dba5),
        UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
        UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5),
        UINT32_C(0xd807aa98), UINT32_C(0x12835b01),
        UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
        UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe),
        UINT32_C(0x9bdc06a7), UINT32_C(0xc19bf174),
        UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
        UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc),
        UINT32_C(0x2de92c6f), UINT32_C(0x4a7484aa),
        UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
        UINT32_C(0x983e5152), UINT32_C(0xa831c66d),
        UINT32_C(0xb00327c8), UINT32_C(0xbf597fc7),
        UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
        UINT32_C(0x06ca6351), UINT32_C(0x14292967),
        UINT32_C(0x27b70a85), UINT32_C(0x2e1b2138),
        UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
        UINT32_C(0x650a7354), UINT32_C(0x766a0abb),
        UINT32_C(0x81c2c92e), UINT32_C(0x92722c85),
        UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
        UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3),
        UINT32_C(0xd192e819), UINT32_C(0xd6990624),
        UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
        UINT32_C(0x19a4c116), UINT32_C(0x1e376c08),
        UINT32_C(0x2748774c), UINT32_C(0x34b0bcb5),
        UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
        UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3),
        UINT32_C(0x748f82ee), UINT32_C(0x78a5636f),
        UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
        UINT32_C(0x90befffa), UINT32_C(0xa4506ceb),
        UINT32_C(0xbef9a3f7), UINT32_C(0xc67178f2)
    };
    uint32_t words[64];
    for (unsigned index = 0; index < 16; ++index) {
        size_t offset = (size_t)index * 4U;
        words[index] =
            ((uint32_t)block[offset] << 24) |
            ((uint32_t)block[offset + 1] << 16) |
            ((uint32_t)block[offset + 2] << 8) |
            (uint32_t)block[offset + 3];
    }
    for (unsigned index = 16; index < 64; ++index) {
        uint32_t a = words[index - 15];
        uint32_t b = words[index - 2];
        uint32_t s0 =
            sha256_rotr(a, 7) ^ sha256_rotr(a, 18) ^ (a >> 3);
        uint32_t s1 =
            sha256_rotr(b, 17) ^ sha256_rotr(b, 19) ^ (b >> 10);
        words[index] =
            words[index - 16] + s0 + words[index - 7] + s1;
    }

    uint32_t a = context->state[0];
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    uint32_t e = context->state[4];
    uint32_t f = context->state[5];
    uint32_t g = context->state[6];
    uint32_t h = context->state[7];
    for (unsigned index = 0; index < 64; ++index) {
        uint32_t sum1 =
            sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^
            sha256_rotr(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t temporary1 =
            h + sum1 + choice + constants[index] + words[index];
        uint32_t sum0 =
            sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^
            sha256_rotr(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void sha256_init(psvr2_sha256_context *context) {
    *context = (psvr2_sha256_context){
        .state = {
            UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85),
            UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
            UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
            UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)
        }
    };
}

static bool sha256_update(
    psvr2_sha256_context *context, const void *data, size_t len) {
    if (len && !data) return false;
    if (context->bytes > UINT64_MAX / 8U ||
        len > UINT64_MAX / 8U - context->bytes)
        return false;
    context->bytes += len;
    const uint8_t *cursor = data;
    while (len) {
        size_t available = sizeof(context->block) - context->used;
        size_t take = psvr2_min_size(available, len);
        memcpy(context->block + context->used, cursor, take);
        context->used += take;
        cursor += take;
        len -= take;
        if (context->used == sizeof(context->block)) {
            sha256_transform(context, context->block);
            context->used = 0;
        }
    }
    return true;
}

static void sha256_final(
    psvr2_sha256_context *context, uint8_t digest[32]) {
    uint64_t bits = context->bytes * UINT64_C(8);
    context->block[context->used++] = UINT8_C(0x80);
    if (context->used > 56) {
        memset(
            context->block + context->used, 0,
            sizeof(context->block) - context->used);
        sha256_transform(context, context->block);
        context->used = 0;
    }
    memset(context->block + context->used, 0, 56 - context->used);
    for (unsigned index = 0; index < 8; ++index)
        context->block[63U - index] =
            (uint8_t)(bits >> (index * 8U));
    sha256_transform(context, context->block);
    for (unsigned index = 0; index < 8; ++index) {
        digest[index * 4U] =
            (uint8_t)(context->state[index] >> 24);
        digest[index * 4U + 1] =
            (uint8_t)(context->state[index] >> 16);
        digest[index * 4U + 2] =
            (uint8_t)(context->state[index] >> 8);
        digest[index * 4U + 3] =
            (uint8_t)context->state[index];
    }
}

bool psvr2_sha256(const void *data, size_t len, uint8_t digest[32]) {
    if (!digest || (len && !data)) return false;
    psvr2_sha256_context context;
    sha256_init(&context);
    if (!sha256_update(&context, data, len)) return false;
    sha256_final(&context, digest);
    return true;
}

bool psvr2_sha256_file(
    const char *path, uint8_t digest[32], uint64_t *size_out) {
    if (!path || !digest) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    psvr2_sha256_context context;
    sha256_init(&context);
    uint8_t block[65536];
    bool ok = true;
    for (;;) {
        size_t count = fread(block, 1, sizeof(block), file);
        if (count && !sha256_update(&context, block, count)) {
            ok = false;
            break;
        }
        if (count < sizeof(block)) {
            if (ferror(file)) ok = false;
            break;
        }
    }
    if (fclose(file) != 0) ok = false;
    if (!ok) return false;
    sha256_final(&context, digest);
    if (size_out) *size_out = context.bytes;
    return true;
}

void psvr2_sha256_hex(const uint8_t digest[32], char output[65]) {
    static const char hex[] = "0123456789abcdef";
    if (!digest || !output) return;
    for (unsigned index = 0; index < 32; ++index) {
        output[index * 2U] = hex[digest[index] >> 4];
        output[index * 2U + 1] = hex[digest[index] & 0xfU];
    }
    output[64] = '\0';
}

const char *psvr2_basename(const char *path) {
    if (!path) return "";
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    const char *p = !a ? b : !b ? a : (a > b ? a : b);
    return p ? p + 1 : path;
}

bool psvr2_file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

void psvr2_hexdump(FILE *out, uint64_t base, const uint8_t *data, size_t len) {
    for (size_t off = 0; off < len; off += 16) {
        fprintf(out, "%016llx: ", (unsigned long long)(base + off));
        for (size_t i = 0; i < 16; ++i)
            fprintf(out, i + off < len ? "%02x " : "   ", i + off < len ? data[off + i] : 0);
        fputs(" |", out);
        for (size_t i = 0; i < 16 && off + i < len; ++i) {
            unsigned char c = data[off + i];
            fputc(c >= 32 && c < 127 ? c : '.', out);
        }
        fputs("|\n", out);
    }
}
