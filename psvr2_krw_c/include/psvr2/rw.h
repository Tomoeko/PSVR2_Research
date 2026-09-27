#ifndef PSVR2_RW_H
#define PSVR2_RW_H

#include "psvr2/exploit.h"

#define PSVR2_WRITE_PAYLOAD_SIZE 200U

typedef struct psvr2_registers {
    uint64_t req;
    uint64_t mep;
    uint64_t x21;
    uint8_t x22;
    bool valid;
} psvr2_registers;

typedef struct psvr2_krw {
    psvr2_exploit *ex;
    uint64_t connection_generation;
    uint64_t spinlock;
    psvr2_registers regs;
    psvr2_result last_result;
} psvr2_krw;

void psvr2_krw_init(psvr2_krw *krw, psvr2_exploit *ex);
bool psvr2_krw_trigger_overflow(psvr2_krw *krw, const void *payload,
                                uint16_t length, unsigned timeout_ms);
/* attempted is false when the live-state preflight blocks USB dispatch. */
bool psvr2_krw_trigger_overflow_ex(
    psvr2_krw *krw, const void *payload,
    uint16_t length, unsigned timeout_ms, bool *attempted);
size_t psvr2_build_strb_payload(
    const psvr2_constants *c,
    uint8_t out[PSVR2_WRITE_PAYLOAD_SIZE],
    uint64_t target, uint8_t value, uint64_t x21);
size_t psvr2_build_fast_write_payload(
    const psvr2_constants *c,
    uint8_t out[PSVR2_WRITE_PAYLOAD_SIZE],
    uint64_t target, uint64_t value,
    uint64_t x21, uint64_t x22,
    uint64_t shellcode_addr);
bool psvr2_krw_setup_write(psvr2_krw *krw, bool verbose, bool force);
/* Compatibility wrapper; fatal is retained but never terminates the process. */
bool psvr2_krw_setup_write_ex(psvr2_krw *krw, bool verbose, bool force,
                              bool fatal);
psvr2_result psvr2_krw_setup_write_result(
    psvr2_krw *krw, bool verbose, bool force);
bool psvr2_krw_read(psvr2_krw *krw, uint64_t addr, void *out, size_t length);
bool psvr2_krw_read_ex(psvr2_krw *krw, uint64_t addr, void *out, size_t length,
                       bool reinit, bool fatal);
psvr2_result psvr2_krw_read_result(
    psvr2_krw *krw, uint64_t addr, void *out,
    size_t length, bool reinit);
uint64_t psvr2_krw_read_ptr(psvr2_krw *krw, uint64_t addr);
uint32_t psvr2_krw_read_u32(psvr2_krw *krw, uint64_t addr);
bool psvr2_krw_read_string(psvr2_krw *krw, uint64_t addr, char *out, size_t max_len);
bool psvr2_krw_write_byte(psvr2_krw *krw, uint64_t addr, uint8_t value);
bool psvr2_krw_write_byte_blind(psvr2_krw *krw, uint64_t addr, uint8_t value);
bool psvr2_krw_write_u64_slow(psvr2_krw *krw, uint64_t addr, uint64_t value);
bool psvr2_krw_write_u64_blind(psvr2_krw *krw, uint64_t addr, uint64_t value);
bool psvr2_krw_write_u64_fast(psvr2_krw *krw, uint64_t addr, uint64_t value);
bool psvr2_krw_write_u32_fast(psvr2_krw *krw, uint64_t addr, uint32_t value);
bool psvr2_krw_write_buffer(psvr2_krw *krw, uint64_t addr,
                            const void *data, size_t length, bool fast);
bool psvr2_krw_flush_tlb(psvr2_krw *krw);
bool psvr2_krw_kernel_execution_probe(psvr2_krw *krw, bool verbose);
psvr2_result psvr2_krw_kernel_execution_probe_result(
    psvr2_krw *krw, bool verbose);
const psvr2_result *psvr2_krw_last_result(const psvr2_krw *krw);

#endif
