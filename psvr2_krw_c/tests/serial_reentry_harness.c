/* Exercise actual serial command decisions without USB or kernel writes. */
#include "stage1_internal.h"
#include "../src/shell_stage1.c"

#include <assert.h>

static bool stage3_present, serial_dependencies, probe_ok, probe_transport;
static bool status_transport;
static int64_t status_result;
static const char *last_status;
static size_t probes, deploys, unloads, recoveries;
static bool expected_reset, expected_reload;

static void reset_case(psvr2_shell *shell) {
    stage3_present = serial_dependencies = probe_ok = probe_transport = true;
    status_transport = true;
    status_result = 0;
    last_status = "OK acm x1 active after USB reset iface_next=12";
    probes = deploys = unloads = recoveries = 0;
    expected_reset = expected_reload = false;
    psvr2_device *device = shell->runtime->krw->ex->usb;
    device->serial_safe = true;
    device->connection_generation = 10U;
    psvr2_constants *constants = shell->runtime->krw->ex->constants;
    constants->live_profile_certified = true;
    constants->certified_connection_generation = 10U;
    constants->fw.exact_image_baseline_present = true;
}

bool psvr2_shell_busybox_available(psvr2_shell *shell) {
    assert(!strcmp(shell->busybox_path, "/data/modules/busybox"));
    return true;
}

uint64_t psvr2_find_module(psvr2_krw *krw, const char *name) {
    (void)krw;
    if (!strcmp(name, "stage3_serial")) return stage3_present ? 1U : 0U;
    assert(!strcmp(name, "u_serial") || !strcmp(name, "usb_f_acm"));
    return serial_dependencies ? 1U : 0U;
}

bool psvr2_stage1_exec(psvr2_runtime *runtime, const char *command,
                       double timeout, int64_t *result, psvr2_buffer *output) {
    assert(runtime);
    assert(strlen(command) + strlen(PSVR2_STAGE1_COMMAND_PREFIX) <
           PSVR2_STAGE1_COMMAND_CAPACITY);
    if (strstr(command, "read P </tmp/.stage3_shell_pid")) {
        assert(timeout == 5.0 && !output);
        assert(strstr(command, "test -r /proc/stage3 && test -w /proc/stage3"));
        assert(strstr(command, "test -c /dev/ttyGS0"));
        assert(strstr(command, "test ! -e /tmp/.stage3_shell_stop"));
        assert(strstr(command, "read C </tmp/.stage3_shell_child"));
        assert(strstr(command, "[ \"$P\" -gt 1 ] && [ \"$C\" -gt 1 ]"));
        assert(strstr(command, "kill -0 \"$P\" \"$C\""));
        assert(strstr(command, "for F in 0 1 2"));
        assert(strstr(command, "readlink /proc/$C/fd/$F"));
        ++probes;
        *result = probe_ok ? 0 : 1;
        return probe_transport;
    }
    if (!strcmp(command, "/data/modules/busybox cat /proc/stage3 2>/dev/null")) {
        assert(timeout == 5.0 && output);
        assert(psvr2_buffer_append(output, last_status, strlen(last_status) + 1U));
        --output->len;
        *result = status_result;
        return status_transport;
    }
    if (!strcmp(command, "test -w /proc/rmmod_helper")) {
        assert(expected_reload && !output);
        *result = 0;
        return true;
    }
    assert(timeout == 15.0 && output);
    assert(strstr(command, "insmod /data/modules/stage3_serial.ko"));
    assert(strstr(command, expected_reset ? "usb_reset=1" : "usb_reset=0"));
    assert(strstr(command, "noevict=1"));
    if (expected_reload) {
        assert(strstr(command, "echo stage3_serial > /proc/rmmod_helper"));
        assert(!strstr(command, "insmod /data/modules/u_serial.ko"));
        assert(!strstr(command, "insmod /data/modules/usb_f_acm.ko"));
        ++unloads;
    } else {
        assert(strstr(command, "insmod /data/modules/u_serial.ko"));
        assert(strstr(command, "insmod /data/modules/usb_f_acm.ko"));
    }
    ++deploys;
    stage3_present = serial_dependencies = probe_ok = true;
    last_status = expected_reset
        ? "OK acm x1 injected iface_next=12 (USB reset pending)"
        : "OK acm x1 injected iface_next=12";
    *result = 0;
    return true;
}

uint64_t psvr2_vfs_root(psvr2_vfs *vfs) { (void)vfs; return 1U; }
uint64_t psvr2_vfs_resolve_path(psvr2_vfs *vfs, uint64_t root,
                                const char *path) {
    (void)vfs;
    assert(root == 1U && strstr(path, "/data/modules/"));
    return 1U;
}
void psvr2_vfs_init(psvr2_vfs *vfs, psvr2_krw *krw) { vfs->krw = krw; }

bool psvr2_device_present(psvr2_device *device) { (void)device; return false; }
bool psvr2_device_wait_ready(psvr2_device *device, unsigned timeout) {
    assert(timeout == 10000U);
    ++recoveries;
    device->serial_safe = true;
    ++device->connection_generation;
    last_status = "OK acm x1 active after USB reset iface_next=12";
    return true;
}
void psvr2_runtime_reset_after_reconnect(psvr2_runtime *runtime) {
    runtime->mailbox.valid = false;
}
psvr2_result psvr2_exploit_recover_installed_reader(psvr2_exploit *exploit) {
    assert(exploit);
    return psvr2_result_ok();
}
psvr2_result psvr2_exploit_initialize_result(psvr2_exploit *exploit, bool force) {
    (void)exploit; (void)force;
    assert(0 && "installed reader recovery should suffice");
    return psvr2_result_ok();
}
bool psvr2_kernel_certify_live_profile(psvr2_krw *krw) {
    krw->ex->constants->live_profile_certified = true;
    krw->ex->constants->certified_connection_generation =
        krw->ex->usb->connection_generation;
    return true;
}
psvr2_result psvr2_krw_setup_write_result(psvr2_krw *krw, bool verbose,
                                         bool force) {
    (void)krw;
    assert(!verbose && !force);
    return psvr2_result_ok();
}
bool psvr2_inject_str_shellcode(psvr2_runtime *runtime) { (void)runtime; return true; }
bool psvr2_stage1_discover(psvr2_runtime *runtime) { (void)runtime; return true; }

static int serial_command(psvr2_shell *shell, const char *argument) {
    char *arguments[] = {"serial", (char *)argument};
    return cmd_serial(shell, argument ? 2 : 1, arguments);
}

int main(void) {
    psvr2_device device = {.handle = (libusb_device_handle *)(uintptr_t)1U};
    psvr2_constants constants = {.version = PSVR2_FW_0600};
    psvr2_exploit exploit = {.usb = &device, .constants = &constants};
    psvr2_krw krw = {.ex = &exploit};
    psvr2_runtime runtime = {.krw = &krw};
    psvr2_shell shell = {.runtime = &runtime};
    psvr2_vfs_init(&shell.vfs, &krw);
    strcpy(shell.busybox_path, "/data/modules/busybox");

    const char *operations[] = {
        "OK acm x1 active after USB reset iface_next=12",
        "OK audiopatch prep=1 port=0 ts=0", "OK takeover usb_data4 flags=0x0",
        "ERR audiopatch: symbols missing"
    };
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(operations); ++index) {
        reset_case(&shell); last_status = operations[index];
        assert(serial_command(&shell, NULL) == 0);
        assert(probes == 1U && !deploys && !unloads && !recoveries);
    }
    reset_case(&shell); last_status = operations[1];
    assert(serial_command(&shell, "noreset") == 0);
    assert(!deploys && !unloads && !recoveries);

    /* A generic successful last operation is never evidence of a reset. */
    reset_case(&shell); device.serial_safe = false; last_status = operations[1];
    assert(serial_command(&shell, NULL) == 1);
    assert(!probes && !deploys && !unloads && !recoveries);
    reset_case(&shell); probe_ok = false;
    assert(serial_command(&shell, NULL) == 1);
    assert(probes == 1U && !deploys && !unloads && !recoveries);
    reset_case(&shell); probe_transport = false;
    assert(serial_command(&shell, NULL) == 1);
    assert(!deploys && !unloads && !recoveries);
    reset_case(&shell); status_transport = false;
    assert(serial_command(&shell, NULL) == 1);
    assert(probes == 1U && !deploys && !unloads && !recoveries);
    reset_case(&shell); serial_dependencies = false;
    assert(serial_command(&shell, NULL) == 1);
    assert(!probes && !deploys && !unloads && !recoveries);
    reset_case(&shell); stage3_present = false;
    assert(serial_command(&shell, NULL) == 1);
    assert(!deploys && !unloads && !recoveries);
    reset_case(&shell); constants.live_profile_certified = false;
    assert(serial_command(&shell, NULL) == 1);
    assert(!probes && !deploys && !unloads && !recoveries);
    reset_case(&shell); constants.certified_connection_generation = 9U;
    assert(serial_command(&shell, NULL) == 1);
    assert(!probes && !deploys && !unloads && !recoveries);

    reset_case(&shell); device.serial_safe = false;
    last_status = "OK acm x1 injected iface_next=12 (USB reset pending)";
    assert(serial_command(&shell, NULL) == 0);
    assert(!deploys && !unloads && recoveries == 1U);

    reset_case(&shell); probe_ok = false; last_status = operations[3];
    assert(serial_command(&shell, "status") == 0);
    assert(!probes && !deploys && !unloads && !recoveries);
    reset_case(&shell); status_transport = false;
    assert(serial_command(&shell, "status") == 1);
    assert(!probes && !deploys && !unloads && !recoveries);

    reset_case(&shell); stage3_present = serial_dependencies = false;
    device.serial_safe = false;
    assert(serial_command(&shell, "noreset") == 0);
    assert(deploys == 1U && !unloads && !recoveries);
    reset_case(&shell); expected_reload = true;
    assert(serial_command(&shell, "reload") == 0);
    assert(deploys == 1U && unloads == 1U && !recoveries);
    reset_case(&shell); expected_reload = expected_reset = true;
    assert(serial_command(&shell, "reset") == 0);
    assert(deploys == 1U && unloads == 1U && recoveries == 1U);
    reset_case(&shell); stage3_present = serial_dependencies = false;
    device.serial_safe = false; expected_reset = true;
    assert(serial_command(&shell, "reset") == 0);
    assert(deploys == 1U && !unloads && recoveries == 1U);
    puts("serial functional-health/reentry regressions passed");
    return 0;
}
