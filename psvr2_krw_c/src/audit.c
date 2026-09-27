#include "psvr2/audit.h"
#include "kernel_images_internal.h"

#include "psvr2/kernel_exec.h"
#include "psvr2/kernel_payload.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    uint64_t address;
    size_t length;
    bool symbol_backed;
} audit_region;

typedef struct {
    bool read_ok;
    bool stable;
    size_t changed;
    size_t nonzero;
    uint64_t digest;
} region_result;

typedef enum {
    HELPER_IMAGES_INVALID,
    HELPER_IMAGES_PRISTINE,
    HELPER_IMAGES_COLD_READY,
    HELPER_IMAGES_INSTALLED
} helper_image_state;

static uint64_t fnv1a64(const uint8_t *data, size_t length) {
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t index = 0; index < length; ++index) {
        value ^= data[index];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static region_result inspect_region(psvr2_krw *krw,
                                    const audit_region *region) {
    region_result result = {0};
    uint8_t *first = malloc(region->length);
    uint8_t *second = malloc(region->length);
    if (!first || !second) goto done;
    if (!psvr2_krw_read_ex(
            krw, region->address, first, region->length, true, false))
        goto done;
    psvr2_sleep_ms(100);
    if (!psvr2_krw_read_ex(
            krw, region->address, second, region->length, true, false))
        goto done;

    result.read_ok = true;
    result.digest = fnv1a64(second, region->length);
    for (size_t index = 0; index < region->length; ++index) {
        if (first[index] != second[index]) ++result.changed;
        if (second[index] != 0) ++result.nonzero;
    }
    result.stable = result.changed == 0;

done:
    free(first);
    free(second);
    return result;
}

static bool inspect_mapping(psvr2_krw *krw, const char *name,
                            uint64_t address, psvr2_pte_level expected_level,
                            const char *expected_permissions,
                            psvr2_pte *pte_out) {
    psvr2_pte pte;
    if (!psvr2_get_pte_ex(krw, address, &pte, true, false)) {
        printf("[-] %-20s unmapped\n", name);
        return false;
    }
    char permissions[4];
    char detail[96];
    psvr2_decode_pte(
        pte.value, pte.level, permissions, detail, sizeof(detail));
    printf("[*] %-20s entry=0x%llx %s %s\n",
           name, (unsigned long long)pte.address,
           permissions, detail);
    if (pte_out) *pte_out = pte;
    return pte.level == expected_level &&
           strcmp(permissions, expected_permissions) == 0;
}

static void inspect_live_completion_state(psvr2_krw *krw) {
    if (!psvr2_krw_setup_write_ex(krw, false, false, false)) {
        puts("[-] live completion state unavailable");
        return;
    }

    uint8_t callback_bytes[8];
    uint8_t endpoint_bytes[8];
    if (!psvr2_krw_read_ex(
            krw, krw->regs.req + UINT64_C(0x30),
            callback_bytes, sizeof(callback_bytes), true, false) ||
        !psvr2_krw_read_ex(
            krw, krw->regs.mep + UINT64_C(0x10),
            endpoint_bytes, sizeof(endpoint_bytes), true, false)) {
        puts("[-] live completion pointers unreadable");
        return;
    }

    uint64_t callback = psvr2_load_le64(callback_bytes);
    uint64_t endpoint_slot = psvr2_load_le64(endpoint_bytes);
    printf("[*] completion request   req=0x%llx callback=0x%llx\n",
           (unsigned long long)krw->regs.req,
           (unsigned long long)callback);
    printf("[*] completion endpoint  mep=0x%llx slot+0x10=0x%llx\n",
           (unsigned long long)krw->regs.mep,
           (unsigned long long)endpoint_slot);
    printf("[*] completion unwind    spinlock=0x%llx busy=0x%02x\n",
           (unsigned long long)krw->regs.x21, krw->regs.x22);

    uint8_t callback_code[32];
    if (psvr2_krw_read_ex(
            krw, callback, callback_code, sizeof(callback_code),
            true, false)) {
        fputs("[*] completion callback bytes   ", stdout);
        for (size_t index = 0; index < sizeof(callback_code); ++index)
            printf("%02x", callback_code[index]);
        fputc('\n', stdout);
    }

    psvr2_pte callback_pte;
    if (psvr2_get_pte_ex(krw, callback, &callback_pte, true, false)) {
        char permissions[4];
        char detail[96];
        psvr2_decode_pte(
            callback_pte.value, callback_pte.level,
            permissions, detail, sizeof(detail));
        printf("[*] completion callback mapping entry=0x%llx %s %s\n",
               (unsigned long long)callback_pte.address,
               permissions, detail);
    } else {
        puts("[-] completion callback mapping unavailable");
    }
}

static bool exact_bytes(psvr2_krw *krw, const char *name,
                        uint64_t address, const void *expected,
                        size_t length) {
    uint8_t observed[32];
    if (!expected || length > sizeof(observed) ||
        !psvr2_krw_read_ex(
            krw, address, observed, length, true, false)) {
        printf("[-] %-20s unreadable\n", name);
        return false;
    }
    bool matches = memcmp(observed, expected, length) == 0;
    printf(matches ? "[+] %-20s exact\n" : "[-] %-20s mismatch\n", name);
    return matches;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t length) {
    for (size_t index = 0; index < length; ++index)
        if (bytes[index] != 0) return false;
    return true;
}

static helper_image_state inspect_helper_images(
    psvr2_krw *krw, const psvr2_constants *constants) {
    uint8_t expected_patch[PSVR2_PATCH_SC_SIZE];
    uint8_t expected_batch[PSVR2_BATCH_PATCH_SC_SIZE];
    uint8_t expected_cleanup[PSVR2_TLBI_CLEANUP_SIZE];
    uint8_t expected_context[PSVR2_DIRECT_CONTEXT_SC_SIZE];
    uint8_t expected_reader[PSVR2_DIRECT_READ_SC_SIZE];
    uint8_t expected_cold_cleanup[PSVR2_COLD_CLEANUP_SIZE];
    uint8_t expected_allocator[PSVR2_ALLOC_HELPER_SIZE];
    uint8_t expected_str[PSVR2_STR_HELPER_SIZE];
    uint8_t observed_patch[PSVR2_PATCH_SC_SIZE];
    uint8_t observed_batch[PSVR2_BATCH_PATCH_SC_SIZE];
    uint8_t observed_cleanup[PSVR2_TLBI_CLEANUP_SIZE];
    uint8_t observed_context[PSVR2_DIRECT_CONTEXT_SC_SIZE];
    uint8_t observed_reader[PSVR2_DIRECT_READ_SC_SIZE];
    uint8_t observed_cold_cleanup[PSVR2_COLD_CLEANUP_SIZE];
    uint8_t observed_allocator[PSVR2_ALLOC_HELPER_SIZE];
    uint8_t observed_str[PSVR2_STR_HELPER_SIZE];
    if (!psvr2_build_patch_helper(constants, expected_patch) ||
        !psvr2_build_batch_patch_helper(constants, expected_batch) ||
        !psvr2_build_tlbi_cleanup(
            constants, expected_cleanup, krw->regs.x22) ||
        !psvr2_build_direct_context_helper(constants, expected_context) ||
        !psvr2_build_direct_read_helper(constants, expected_reader) ||
        !psvr2_build_cold_cleanup_helper(
            constants, constants->fw.cold_cleanup_helper,
            expected_cold_cleanup) ||
        !psvr2_build_workspace_allocator(
            constants, expected_allocator) ||
        !psvr2_build_str_helper(constants, expected_str) ||
        !psvr2_krw_read_ex(
            krw, constants->fw.patch_helper, observed_patch,
            sizeof(observed_patch), true, false) ||
        !psvr2_krw_read_ex(
            krw, constants->fw.batch_patch_helper, observed_batch,
            sizeof(observed_batch), true, false) ||
        !psvr2_krw_read_ex(
            krw, constants->fw.tlbi_cleanup_helper, observed_cleanup,
            sizeof(observed_cleanup), true, false) ||
        !psvr2_krw_read_ex(
            krw, constants->fw.free_helper, observed_context,
            sizeof(observed_context), true, false) ||
        !psvr2_krw_read_ex(
            krw, constants->fw.direct_read_helper, observed_reader,
            sizeof(observed_reader), true, false) ||
        !psvr2_krw_read_ex(
            krw, constants->fw.cold_cleanup_helper,
            observed_cold_cleanup,
            sizeof(observed_cold_cleanup), true, false) ||
        !psvr2_krw_read_ex(
            krw, constants->fw.alloc_helper, observed_allocator,
            sizeof(observed_allocator), true, false) ||
        !psvr2_krw_read_ex(
            krw, constants->fw.str_helper, observed_str,
            sizeof(observed_str), true, false))
        return HELPER_IMAGES_INVALID;

    bool all_zero =
        bytes_are_zero(observed_patch, sizeof(observed_patch)) &&
        bytes_are_zero(observed_batch, sizeof(observed_batch)) &&
        bytes_are_zero(observed_cleanup, sizeof(observed_cleanup)) &&
        bytes_are_zero(observed_context, sizeof(observed_context)) &&
        bytes_are_zero(observed_reader, sizeof(observed_reader)) &&
        bytes_are_zero(
            observed_cold_cleanup, sizeof(observed_cold_cleanup)) &&
        bytes_are_zero(observed_allocator, sizeof(observed_allocator)) &&
        bytes_are_zero(observed_str, sizeof(observed_str));
    if (all_zero) return HELPER_IMAGES_PRISTINE;

    bool cold_helpers_exact =
        memcmp(observed_patch, expected_patch, sizeof(observed_patch)) == 0 &&
        memcmp(
            observed_context, expected_context,
            sizeof(observed_context)) == 0 &&
        memcmp(observed_reader, expected_reader, sizeof(observed_reader)) == 0 &&
        memcmp(
            observed_cold_cleanup, expected_cold_cleanup,
            sizeof(observed_cold_cleanup)) == 0;
    if (cold_helpers_exact &&
        bytes_are_zero(observed_batch, sizeof(observed_batch)) &&
        bytes_are_zero(observed_cleanup, sizeof(observed_cleanup)) &&
        bytes_are_zero(observed_allocator, sizeof(observed_allocator)) &&
        bytes_are_zero(observed_str, sizeof(observed_str)))
        return HELPER_IMAGES_COLD_READY;

    if (cold_helpers_exact &&
        memcmp(observed_batch, expected_batch, sizeof(observed_batch)) == 0 &&
        memcmp(
            observed_cleanup, expected_cleanup,
            sizeof(observed_cleanup)) == 0 &&
        memcmp(
            observed_allocator, expected_allocator,
            sizeof(observed_allocator)) == 0 &&
        memcmp(observed_str, expected_str, sizeof(observed_str)) == 0)
        return HELPER_IMAGES_INSTALLED;
    return HELPER_IMAGES_INVALID;
}

static bool inspect_execution_anchors(
    psvr2_krw *krw, const psvr2_constants *constants,
    helper_image_state *helper_state_out) {
    psvr2_kernel_anchor anchors[PSVR2_KERNEL_ANCHOR_COUNT];
    psvr2_kernel_execution_anchors(constants, anchors);
    bool anchors_ok = true;
    for (size_t index = 0;
         index < PSVR2_KERNEL_ANCHOR_COUNT; ++index)
        anchors_ok =
            exact_bytes(
                krw, anchors[index].name, anchors[index].address,
                anchors[index].bytes, anchors[index].length) &&
            anchors_ok;

    helper_image_state helper_state =
        inspect_helper_images(krw, constants);
    if (helper_state_out) *helper_state_out = helper_state;
    if (helper_state == HELPER_IMAGES_PRISTINE)
        puts("[+] execution helpers    pristine");
    else if (helper_state == HELPER_IMAGES_COLD_READY)
        puts("[+] execution helpers    exact cold-bootstrap set");
    else if (helper_state == HELPER_IMAGES_INSTALLED)
        puts("[+] execution helpers    exact installed images");
    else
        puts("[-] execution helpers    unrecognized or corrupted");
    return anchors_ok && helper_state != HELPER_IMAGES_INVALID;
}

bool psvr2_memory_safety_audit(psvr2_runtime *runtime,
                               psvr2_safety_report *report) {
    if (report) memset(report, 0, sizeof(*report));
    if (!runtime || !runtime->krw || !runtime->krw->ex ||
        !runtime->krw->ex->constants)
        return false;

    psvr2_krw *krw = runtime->krw;
    const psvr2_constants *constants = krw->ex->constants;
    const psvr2_firmware_profile *profile = &constants->fw;
    const uint64_t previous_workspace = profile->previous_workspace;
    puts("[*] Exact-image memory safety audit (read-only)");
    if (!psvr2_constants_kernel_probe_safe(constants)) {
        printf("[-] No exact %s memory baseline is active.\n",
               psvr2_firmware_name(constants->version));
        return false;
    }

    const audit_region regions[] = {
        {"text patch cave", profile->text_cave_start,
         (size_t)(profile->text_cave_end -
                  profile->text_cave_start), false},
        {"probe padding", constants->fw.kernel_bss_stop,
         (size_t)(constants->fw.kernel_page_tables_start -
                  constants->fw.kernel_bss_stop), false},
        {"previous workspace", previous_workspace, 0x600, false},
        {"gsCwq object", profile->gscwq_address, 0x430, true},
        {"gsMMUFaultData", profile->gs_mmu_fault_data, 0x880, true}
    };

    bool reads_ok = true;
    region_result results[PSVR2_ARRAY_LEN(regions)];
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(regions); ++index) {
        results[index] = inspect_region(krw, &regions[index]);
        reads_ok = reads_ok && results[index].read_ok;
        printf("%s %-20s addr=0x%llx size=0x%zx "
               "changed=%zu nonzero=%zu digest=%016llx%s\n",
               results[index].read_ok ? "[*]" : "[-]",
               regions[index].name,
               (unsigned long long)regions[index].address,
               regions[index].length, results[index].changed,
               results[index].nonzero,
               (unsigned long long)results[index].digest,
               regions[index].symbol_backed
                   ? " [ACTIVE SYMBOL: NEVER A WORKSPACE]"
                   : "");
    }

    uint8_t probe_a = 0;
    uint8_t probe_b = 0;
    bool probe_bytes_ok = psvr2_krw_read_ex(
        krw, constants->fw.kernel_probe_byte,
        &probe_a, 1, true, false);
    psvr2_sleep_ms(50);
    probe_bytes_ok =
        probe_bytes_ok &&
        psvr2_krw_read_ex(
            krw, constants->fw.kernel_probe_byte,
            &probe_b, 1, true, false) &&
        probe_a == UINT8_C(0xff) && probe_b == probe_a;

    psvr2_pte text_pte = {0};
    psvr2_pte probe_pte = {0};
    psvr2_pte previous_pte = {0};
    bool text_mapping_ok = inspect_mapping(
        krw, "text patch cave", profile->text_cave_start,
        PSVR2_PTE_L2_BLOCK, "r-x", &text_pte);
    bool probe_mapping_ok = inspect_mapping(
        krw, "probe padding", constants->fw.kernel_probe_byte,
        PSVR2_PTE_L2_BLOCK, "rw-", &probe_pte);
    bool previous_mapping_ok = inspect_mapping(
        krw, "previous workspace", previous_workspace,
        PSVR2_PTE_L2_BLOCK, "rw-", &previous_pte);
    bool mappings_ok =
        text_mapping_ok && probe_mapping_ok && previous_mapping_ok;

    inspect_live_completion_state(krw);
    bool execution_anchors_ok =
        inspect_execution_anchors(krw, constants, NULL);

    char init_name[17] = {0};
    bool tasks_ok = psvr2_krw_read_ex(
                        krw, profile->init_task +
                                 profile->task_comm_offset,
                        init_name, sizeof(init_name) - 1, true, false) &&
                    strcmp(init_name, "swapper/0") == 0;

    bool layout_ok =
        reads_ok && mappings_ok && tasks_ok &&
        results[0].stable &&
        results[1].stable && probe_bytes_ok &&
        previous_workspace >= constants->fw.kernel_image_end &&
        probe_pte.address == previous_pte.address &&
        text_pte.level == PSVR2_PTE_L2_BLOCK;

    bool injection_safe =
        mappings_ok && execution_anchors_ok &&
        psvr2_constants_shellcode_execution_safe(constants) &&
        text_pte.level == PSVR2_PTE_L2_BLOCK;
    if (report) {
        report->exact_memory_layout = layout_ok;
        report->bounded_probe_safe = layout_ok && probe_bytes_ok;
        report->injected_execution_safe = injection_safe;
    }

    printf(tasks_ok
               ? "[+] init_task offsets resolve to swapper/0.\n"
               : "[-] init_task/task offsets did not validate.\n");
    printf(probe_bytes_ok
               ? "[+] Probe byte is stable exact-capture padding (0xff).\n"
               : "[-] Probe byte does not match the stable exact baseline.\n");
    puts(probe_mapping_ok && previous_mapping_ok
             ? "[+] BSS/previous workspace uses its expected NX 2 MiB "
               "block mapping."
             : "[-] BSS/previous workspace mapping did not match the "
               "exact baseline.");
    puts(results[0].nonzero == 0
             ? "[+] Exact text patch cave is pristine."
             : "[*] Exact text patch cave contains stable runtime state; "
               "individual patches remain zero-or-identical gated.");
    puts(execution_anchors_ok
             ? "[+] Live exact-image execution anchors match."
             : "[BLOCKED] Live exact-image execution anchors did not validate.");
    if (!injection_safe)
        puts("[*] Broader persistent shellcode remains firmware-gated.");
    return layout_ok && execution_anchors_ok;
}
