#include "psvr2/shellcode.h"

#include "stage1_internal.h"
#include "wait_status_internal.h"

#include <string.h>

static bool format_stage1_command(const char *command,
                                  char out[PSVR2_STAGE1_COMMAND_CAPACITY],
                                  size_t *length) {
    if (!command || !out || !length) return false;
    int written = snprintf(
        out, PSVR2_STAGE1_COMMAND_CAPACITY,
        PSVR2_STAGE1_COMMAND_PREFIX "%s", command);
    if (written < 0 ||
        (size_t)written >= PSVR2_STAGE1_COMMAND_CAPACITY)
        return false;
    *length = (size_t)written;
    return true;
}

static bool write_stage1_command(psvr2_runtime *runtime,
                                 const char *command, size_t *length) {
    char formatted[PSVR2_STAGE1_COMMAND_CAPACITY];
    if (!format_stage1_command(command, formatted, length)) return false;
    for (size_t offset = 0; offset <= *length; offset += 8) {
        uint8_t word[8] = {0};
        memcpy(word, formatted + offset,
               psvr2_min_size(8, *length + 1 - offset));
        if (!psvr2_krw_write_u64_fast(
                runtime->krw, runtime->mailbox.cmd_in + offset,
                psvr2_load_le64(word)))
            return false;
    }
    return true;
}

bool psvr2_stage1_exec(psvr2_runtime *runtime, const char *command,
                       double timeout, int64_t *retval,
                       psvr2_buffer *output) {
    if (!runtime || !command || !psvr2_stage1_discover(runtime))
        return false;
    psvr2_stage1_mailbox *mailbox = &runtime->mailbox;
    uint8_t value[8];
    if (!psvr2_krw_read_ex(
            runtime->krw, mailbox->cmd_seq, value, sizeof(value),
            false, true))
        return false;
    uint64_t old_sequence = psvr2_load_le64(value);
    if (old_sequence > 0x100000) {
        mailbox->valid = false;
        return false;
    }

    size_t command_length = 0;
    if (!write_stage1_command(runtime, command, &command_length) ||
        !psvr2_krw_write_u64_fast(
            runtime->krw, mailbox->cmd_out_len, 0) ||
        !psvr2_krw_write_u64_fast(
            runtime->krw, mailbox->cmd_in_len, command_length))
        return false;

    double start = psvr2_now();
    uint64_t sequence = old_sequence;
    while (psvr2_now() - start < timeout) {
        if (!psvr2_krw_read_ex(
                runtime->krw, mailbox->cmd_seq, value, sizeof(value),
                false, true))
            return false;
        sequence = psvr2_load_le64(value);
        if (sequence != old_sequence) break;
    }
    if (sequence == old_sequence) return false;

    uint64_t output_length =
        psvr2_krw_read(
            runtime->krw, mailbox->cmd_out_len, value, sizeof(value))
            ? psvr2_load_le64(value) : 0;
    output_length =
        psvr2_min_u64(
            output_length, (uint64_t)PSVR2_STAGE1_OUTPUT_LIMIT);
    if (output_length && output) {
        if (!psvr2_buffer_reserve(output, (size_t)output_length + 1) ||
            !psvr2_krw_read(
                runtime->krw, mailbox->cmd_out, output->data,
                (size_t)output_length))
            return false;
        output->len = (size_t)output_length;
        output->data[output->len] = '\0';
    }

    int64_t result = -1;
    if (mailbox->cmd_ret &&
        psvr2_krw_read(
            runtime->krw, mailbox->cmd_ret, value, sizeof(value)))
        result = psvr2_remote_exit_status(
            (int64_t)psvr2_load_le64(value));
    if (retval) *retval = result;
    return true;
}

bool psvr2_stage1_exec_blind(psvr2_runtime *runtime, const char *command) {
    if (!runtime || !command || !psvr2_stage1_discover(runtime))
        return false;
    size_t command_length = 0;
    return write_stage1_command(runtime, command, &command_length) &&
           psvr2_krw_write_u64_fast(
               runtime->krw, runtime->mailbox.cmd_in_len, command_length);
}

static void forget_stage1(psvr2_runtime *runtime) {
    memset(&runtime->mailbox, 0, sizeof(runtime->mailbox));
    runtime->stage1_loaded = false;
    if (runtime->phase == PSVR2_RUNTIME_STAGE1_READY)
        runtime->phase = PSVR2_RUNTIME_WORKSPACE_READY;
}

static bool unload_stage1(psvr2_runtime *runtime) {
    if (!psvr2_find_module(runtime->krw, "stage1")) {
        forget_stage1(runtime);
        return true;
    }

    /*
     * Never ask Stage1's own command thread to unload its module: rmmod waits
     * for module_exit(), while module_exit() waits for that thread to stop.
     * The injected UMH worker is independent of Stage1 and can unload it
     * without creating that cycle.
     */
    forget_stage1(runtime);
    int64_t result = -1;
    bool unloaded =
        psvr2_exec(
            runtime, "rmmod stage1", false, &result, NULL) &&
        !psvr2_find_module(runtime->krw, "stage1");
    if (!unloaded) {
        /*
         * The PSVR2 4.4 kernel may be built without CONFIG_MODULE_UNLOAD.
         * If the project's helper is already loaded, use its cleanup-aware
         * proc interface rather than its raw teardown mode. A Stage1 that
         * predates the helper cannot have its dependencies proven, so an
         * explicit replacement may accept incomplete history. The helper
         * still rejects every dependent module it has actually observed.
         */
        result = -1;
        unloaded =
            psvr2_exec(
                runtime,
                "echo stage1 > /proc/rmmod_helper",
                false, &result, NULL) &&
            !psvr2_find_module(runtime->krw, "stage1");
    }
    if (!unloaded && runtime->rmmod_helper_missed_stage1) {
        result = -1;
        unloaded =
            psvr2_exec(
                runtime,
                "echo !stage1 > /proc/rmmod_helper",
                false, &result, NULL) &&
            !psvr2_find_module(runtime->krw, "stage1");
    }
    if (!unloaded) {
        fprintf(stderr, "[-] Could not unload Stage1 (exit %lld).\n",
                (long long)result);
        (void)psvr2_stage1_discover(runtime);
        return false;
    }
    runtime->rmmod_helper_missed_stage1 = false;
    puts("[+] Previous Stage1 module unloaded.");
    return true;
}

bool psvr2_load_stage1(psvr2_runtime *runtime, const char *ko_path,
                       bool force_tmp, bool force) {
    if (!runtime) return false;
    if (!force && psvr2_stage1_discover(runtime)) return true;
    if (!force && psvr2_find_module(runtime->krw, "stage1")) {
        fputs("[-] Stage1 is already loaded but does not expose the current "
              "firmware-matched mailbox ABI. Automatic loading refused; "
              "build and explicitly load the matching current Stage1 "
              "with --stage1 FILE or stage1 FILE.\n", stderr);
        return false;
    }
    if (force && !unload_stage1(runtime)) return false;

    psvr2_vfs vfs;
    psvr2_vfs_init(&vfs, runtime->krw);
    uint64_t root = psvr2_vfs_root(&vfs);
    const char *remote = NULL;
    if (!force_tmp &&
        psvr2_vfs_resolve_path(
            &vfs, root, "/data/modules/stage1.ko"))
        remote = "/data/modules/stage1.ko";
    if (!remote) {
        remote = "/tmp/stage1.ko";
        if (ko_path) {
            if (!psvr2_file_exists(ko_path) ||
                !psvr2_upload_file(runtime, ko_path, "stage1.ko"))
                return false;
        } else if (!psvr2_vfs_resolve_path(&vfs, root, remote)) {
            fputs("[-] No Stage1 module was found; pass --stage1 FILE.\n",
                  stderr);
            return false;
        }
    }

    char command[1200];
    snprintf(command, sizeof(command), "insmod '%s'", remote);
    double load_started = psvr2_now();
    int64_t result = -1;
    bool ok = psvr2_exec(runtime, command, false, &result, NULL);
    if (!ok) {
        /*
         * Avoid capturing /tmp/.kout on the normal load path. Resolving that
         * file requires a costly VFS walk through the descriptor-leak reader.
         * A failed insmod can still mean EEXIST, so confirm the module through
         * modlist before treating the command as an error.
         */
        runtime->mailbox.valid = false;
        if (psvr2_stage1_discover_from_module(runtime, NULL)) {
            runtime->stage1_loaded = true;
            psvr2_runtime_mark_phase(
                runtime, PSVR2_RUNTIME_STAGE1_READY);
            printf("[+] Stage1 ready in %.3fs.\n",
                   psvr2_now() - load_started);
            return true;
        }
        fprintf(stderr, "[-] insmod %s failed (exit %lld).\n",
                remote, (long long)result);
        return false;
    }

    /*
     * UMH_WAIT_PROC confirms insmod exited, but module initialization and
     * mailbox publication are separate observations. Poll the module list
     * directly instead of relying on a fixed delay.
     */
    double deadline = psvr2_now() + 1.0;
    do {
        runtime->mailbox.valid = false;
        if (psvr2_stage1_discover_from_module(runtime, NULL)) {
            runtime->stage1_loaded = true;
            psvr2_runtime_mark_phase(
                runtime, PSVR2_RUNTIME_STAGE1_READY);
            printf("[+] Stage1 ready in %.3fs.\n",
                   psvr2_now() - load_started);
            return true;
        }
    } while (psvr2_now() < deadline);

    runtime->mailbox.valid = false;
    if (psvr2_stage1_discover(runtime)) {
        printf("[+] Stage1 ready in %.3fs.\n",
               psvr2_now() - load_started);
        return true;
    }
    fputs("[-] Stage1 is present but its mailbox could not be discovered.\n",
          stderr);
    return false;
}
