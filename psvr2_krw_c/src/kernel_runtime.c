#include "shellcode_internal.h"
#include "firmware_0600_internal.h"
#include "kernel_bootstrap_internal.h"

#include "psvr2/kernel_exec.h"
#include "psvr2/kernel_payload.h"

#include <string.h>

void psvr2_runtime_init(psvr2_runtime *runtime, psvr2_krw *krw) {
    memset(runtime, 0, sizeof(*runtime));
    runtime->krw = krw;
    runtime->phase = krw ? PSVR2_RUNTIME_KERNEL_READY
                         : PSVR2_RUNTIME_UNINITIALIZED;
    if (krw && krw->ex && krw->ex->usb)
        runtime->connection_generation =
            krw->ex->usb->connection_generation;
}

void psvr2_runtime_reset_after_reconnect(psvr2_runtime *runtime) {
    if (!runtime) return;
    psvr2_krw *krw = runtime->krw;
    memset(runtime, 0, sizeof(*runtime));
    runtime->krw = krw;
    runtime->phase = krw ? PSVR2_RUNTIME_KERNEL_READY
                         : PSVR2_RUNTIME_UNINITIALIZED;
    if (krw) {
        if (krw->ex)
            psvr2_constants_invalidate_live_state(
                krw->ex->constants);
        if (krw->ex && krw->ex->usb)
            krw->connection_generation =
                krw->ex->usb->connection_generation;
        krw->spinlock = 0;
        memset(&krw->regs, 0, sizeof(krw->regs));
        if (krw->ex) {
            krw->ex->request_buffer = 0;
            krw->ex->direct_read_ready = false;
            if (krw->ex->usb) {
                krw->ex->connection_generation =
                    krw->ex->usb->connection_generation;
                runtime->connection_generation =
                    krw->ex->usb->connection_generation;
            }
        }
    }
}

bool psvr2_runtime_sync_connection(psvr2_runtime *runtime) {
    if (!runtime || !runtime->krw || !runtime->krw->ex ||
        !runtime->krw->ex->usb)
        return false;
    uint64_t generation =
        runtime->krw->ex->usb->connection_generation;
    if (runtime->connection_generation == generation)
        return true;
    psvr2_runtime_reset_after_reconnect(runtime);
    return false;
}

void psvr2_runtime_mark_phase(
    psvr2_runtime *runtime, psvr2_runtime_phase phase) {
    if (runtime && phase > runtime->phase)
        runtime->phase = phase;
}

bool psvr2_runtime_execution_safe(
    psvr2_runtime *runtime, bool explain) {
    if (!runtime || !runtime->krw ||
        !runtime->krw->ex || !runtime->krw->ex->constants)
        return false;
    if (!psvr2_runtime_sync_connection(runtime))
        return false;
    psvr2_constants *constants =
        runtime->krw->ex->constants;
    if (constants->certified_connection_generation !=
        runtime->krw->ex->usb->connection_generation) {
        psvr2_constants_invalidate_live_state(constants);
        return false;
    }
    if (psvr2_constants_shellcode_execution_safe(constants))
        return true;
    if (explain && !constants->live_profile_certified)
        fprintf(stderr,
                "[-] Refusing kernel code/UMH operation: the selected %s "
                "profile has not passed live exact-image certification.\n",
                psvr2_firmware_name(constants->version));
    else if (explain)
        fprintf(stderr,
                "[-] Refusing kernel code/UMH operation: "
                "the exact-image text patch path at 0x%llx "
                "is not certified for this firmware.\n",
                (unsigned long long)constants->str_helper);
    return false;
}

bool psvr2_runtime_workspace_safe(
    psvr2_runtime *runtime, bool explain) {
    if (psvr2_runtime_execution_safe(runtime, explain) &&
        psvr2_constants_runtime_workspace_safe(
            runtime->krw->ex->constants))
        return true;
    if (explain)
        fputs("[-] Refusing UMH operation: no validated runtime allocation.\n",
              stderr);
    return false;
}

static bool prepare_runtime_workspace(psvr2_runtime *runtime) {
    double started = psvr2_now();
    bool ready = psvr2_kernel_workspace_prepare(runtime);
    psvr2_report_timing(
        "kernel workspace preparation", started, ready);
    return ready;
}

bool psvr2_verify_shellcode(psvr2_runtime *runtime, bool fatal) {
    if (!psvr2_runtime_execution_safe(runtime, fatal)) return false;
    uint8_t shellcode[PSVR2_STR_HELPER_SIZE];
    uint8_t expected[PSVR2_STR_HELPER_SIZE];
    const psvr2_constants *constants = runtime->krw->ex->constants;
    if (!psvr2_build_str_helper(constants, expected))
        return false;
    if (!psvr2_krw_read_ex(runtime->krw, constants->str_helper,
                           shellcode, sizeof(shellcode), true, fatal))
        return false;
    if (memcmp(shellcode, expected, sizeof(shellcode)) != 0)
        return false;

    return true;
}

static bool enable_module_loading(psvr2_runtime *runtime) {
    psvr2_constants *constants = runtime->krw->ex->constants;
    if (!constants->fw.modules_disabled)
        return true;
    uint8_t observed[sizeof(uint32_t)];
    if (!psvr2_krw_read_ex(
            runtime->krw, constants->fw.modules_disabled,
            observed, sizeof(observed), true, false))
        return false;
    uint32_t disabled = psvr2_load_le32(observed);
    if (!disabled)
        return true;
    puts("    Enabling module loading (modules_disabled = 0)...");
    if (!psvr2_krw_write_u32_fast(
            runtime->krw, constants->fw.modules_disabled, 0))
        return false;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (psvr2_krw_read_ex(
                runtime->krw, constants->fw.modules_disabled,
                observed, sizeof(observed), true, false) &&
            psvr2_load_le32(observed) == 0)
            return true;
    }
    fputs("[-] modules_disabled write did not verify.\n", stderr);
    return false;
}

bool psvr2_inject_str_shellcode(psvr2_runtime *runtime) {
    double total_started = psvr2_now();
    if (!psvr2_runtime_execution_safe(runtime, true)) return false;
    psvr2_krw *krw = runtime->krw;
    psvr2_constants *constants = krw->ex->constants;
    if (psvr2_verify_shellcode(runtime, false)) {
        double started = psvr2_now();
        bool bootstrapped = psvr2_kernel_bootstrap_helpers(krw);
        psvr2_report_timing(
            "installed helper validation/extension", started,
            bootstrapped);
        if (!bootstrapped)
            return false;
        puts("[+] Shellcode already present.");
        bool ready = enable_module_loading(runtime) &&
                     prepare_runtime_workspace(runtime);
        psvr2_report_timing(
            "STR injection total", total_started, ready);
        return ready;
    }

    puts("[*] Installing exact-image STR helper...");
    uint8_t shellcode[PSVR2_STR_HELPER_SIZE];
    uint8_t allocator[PSVR2_ALLOC_HELPER_SIZE];
    if (!psvr2_build_str_helper(constants, shellcode) ||
        !psvr2_build_workspace_allocator(constants, allocator))
        return false;
    printf("    Patching %zu bytes at 0x%llx through kernel text helper\n",
           sizeof(shellcode),
           (unsigned long long)constants->str_helper);
    const psvr2_kernel_text_patch_request requests[] = {
        {
            .target = constants->str_helper,
            .data = shellcode,
            .length = sizeof(shellcode)
        },
        {
            .target = constants->fw.alloc_helper,
            .data = allocator,
            .length = sizeof(allocator)
        }
    };
    double patch_started = psvr2_now();
    bool patched = psvr2_kernel_text_patch_many(
        krw, requests, PSVR2_ARRAY_LEN(requests));
    psvr2_report_timing(
        "STR/allocator text patch", patch_started, patched);
    if (!patched)
        return false;
    runtime->workspace_allocator_injected = true;
    psvr2_runtime_mark_phase(
        runtime, PSVR2_RUNTIME_HELPERS_READY);

    double verify_started = psvr2_now();
    bool ok = psvr2_verify_shellcode(runtime, true);
    psvr2_report_timing(
        "STR helper verification", verify_started, ok);
    if (ok) ok = enable_module_loading(runtime);
    if (ok) ok = prepare_runtime_workspace(runtime);
    puts(ok ? "    Verified." : "    MISMATCH!");
    psvr2_report_timing(
        "STR injection total", total_started, ok);
    return ok;
}
