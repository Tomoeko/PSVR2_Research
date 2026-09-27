#include "psvr2/module_helper.h"

#include "psvr2/memory.h"
#include "psvr2/shellcode.h"

static bool remote_path_exists(psvr2_vfs *vfs, uint64_t root,
                               const char *path) {
    return psvr2_vfs_resolve_path(vfs, root, path) != 0;
}

static bool helper_proc_ready(psvr2_runtime *runtime) {
    int64_t result = -1;
    return psvr2_exec(
               runtime, "test -r /proc/rmmod_helper",
               false, &result, NULL) &&
           result == 0;
}

static bool helper_ready(psvr2_runtime *runtime) {
    return psvr2_find_module(runtime->krw, "rmmod_helper") != 0 &&
           helper_proc_ready(runtime);
}

psvr2_rmmod_helper_state psvr2_ensure_rmmod_helper(
    psvr2_runtime *runtime) {
    if (!runtime || !runtime->krw)
        return PSVR2_RMMOD_HELPER_ERROR;

    runtime->rmmod_helper_missed_stage1 = false;
    bool stage1_preexisting =
        psvr2_find_module(runtime->krw, "stage1") != 0;
    psvr2_vfs vfs;
    psvr2_vfs_init(&vfs, runtime->krw);
    uint64_t root = psvr2_vfs_root(&vfs);
    if (!root) {
        fputs("[-] Cannot inspect the device filesystem for "
              "rmmod_helper.\n", stderr);
        return PSVR2_RMMOD_HELPER_ERROR;
    }

    bool module_present =
        psvr2_find_module(runtime->krw, "rmmod_helper") != 0;
    bool proc_present = helper_proc_ready(runtime);
    if (module_present && proc_present) {
        puts("[+] rmmod_helper is ready.");
        return PSVR2_RMMOD_HELPER_READY;
    }
    if (module_present || proc_present) {
        fprintf(stderr,
                "[-] Inconsistent rmmod_helper state: module=%s, proc=%s. "
                "Automatic insmod was blocked.\n",
                module_present ? "present" : "absent",
                proc_present ? "present" : "absent");
        return PSVR2_RMMOD_HELPER_ERROR;
    }
    if (!remote_path_exists(
            &vfs, root, "/data/modules/rmmod_helper.ko"))
        return PSVR2_RMMOD_HELPER_UNAVAILABLE;

    puts("[*] Loading /data/modules/rmmod_helper.ko...");
    int64_t result = -1;
    bool executed = psvr2_exec(
        runtime, "insmod '/data/modules/rmmod_helper.ko'",
        false, &result, NULL);

    double deadline = psvr2_now() + 1.0;
    do {
        if (helper_ready(runtime)) {
            runtime->rmmod_helper_missed_stage1 = stage1_preexisting;
            puts("[+] rmmod_helper loaded and verified.");
            return PSVR2_RMMOD_HELPER_READY;
        }
        psvr2_sleep_ms(10);
    } while (psvr2_now() < deadline);

    fprintf(stderr,
            "[-] Could not activate /data/modules/rmmod_helper.ko "
            "(command=%s, exit=%lld).\n",
            executed ? "completed" : "failed",
            (long long)result);
    return PSVR2_RMMOD_HELPER_ERROR;
}
