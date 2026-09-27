#include "shell_internal.h"
#include "psvr2/constants.h"
#include "psvr2/kernel_exec.h"

#include <stdlib.h>
#include <string.h>

#define AUTH_REFLECTION_EVENT_MAGIC UINT64_C(0x3152463156525350)
#define AUTH_REFLECTION_EVENT_ABI 1U
#define AUTH_REFLECTION_EVENT_SIZE 192U
#define AUTH_REFLECTION_EVENT_CERTIFICATE 1U
#define AUTH_REFLECTION_EVENT_RESPONSE 2U
#define AUTH_REFLECTION_EVENT_RESULT 3U

typedef struct {
    uint64_t magic;
    uint16_t abi_version;
    uint16_t firmware;
    uint32_t kind;
    uint64_t generation;
    int32_t result;
    uint32_t payload_length;
    uint8_t payload[160];
} auth_reflection_event;

_Static_assert(sizeof(auth_reflection_event) == AUTH_REFLECTION_EVENT_SIZE,
               "reflection event ABI");

static uint32_t auth_crc32(const void *data, size_t length) {
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^
                  (UINT32_C(0xedb88320) &
                   (uint32_t)-(int32_t)(crc & 1U));
    }
    return ~crc;
}

static void auth_finish_report(uint8_t report[64]) {
    uint32_t crc = auth_crc32(report, 60);
    report[60] = (uint8_t)(crc >> 24);
    report[61] = (uint8_t)(crc >> 16);
    report[62] = (uint8_t)(crc >> 8);
    report[63] = (uint8_t)crc;
}

static bool auth_read_bulk_event(psvr2_shell *shell,
                                 unsigned expected_kind,
                                 uint64_t generation,
                                 uint16_t expected_firmware,
                                 auth_reflection_event *event,
                                 unsigned timeout_ms) {
    static const uint8_t magic[] = {
        0x50, 0x53, 0x52, 0x56, 0x31, 0x46, 0x52, 0x31
    };
    uint8_t frame[AUTH_REFLECTION_EVENT_SIZE];
    uint8_t *packet = malloc(64U * 1024U);
    size_t frame_length = 0;
    size_t magic_length = 0;
    size_t discarded = 0;
    double deadline = psvr2_now() + (double)timeout_ms / 1000.0;
    if (!packet)
        return false;
    while (psvr2_now() < deadline) {
        int transferred = 0;
        int result = libusb_bulk_transfer(
            shell->runtime->krw->ex->usb->handle,
            shell->bulk_in, packet, 64 * 1024, &transferred, 100);
        if (result != 0 && result != LIBUSB_ERROR_TIMEOUT) {
            fprintf(stderr, "[-] Reflection bulk read failed: %s.\n",
                    libusb_error_name(result));
            free(packet);
            return false;
        }
        for (int offset = 0; offset < transferred; ++offset) {
            uint8_t byte = packet[offset];
            if (!frame_length) {
                if (byte == magic[magic_length]) {
                    ++magic_length;
                    if (magic_length == sizeof(magic)) {
                        memcpy(frame, magic, sizeof(magic));
                        frame_length = sizeof(magic);
                        magic_length = 0;
                    }
                } else {
                    discarded += magic_length + 1U;
                    magic_length = byte == magic[0] ? 1U : 0U;
                    if (magic_length)
                        --discarded;
                }
                continue;
            }
            frame[frame_length++] = byte;
            if (frame_length < sizeof(frame))
                continue;

            memcpy(event, frame, sizeof(*event));
            frame_length = 0;
            if (event->magic != AUTH_REFLECTION_EVENT_MAGIC ||
                event->abi_version != AUTH_REFLECTION_EVENT_ABI ||
                event->firmware != expected_firmware ||
                event->kind != expected_kind ||
                (generation && event->generation != generation) ||
                event->payload_length > sizeof(event->payload)) {
                discarded += sizeof(frame);
                continue;
            }
            if (discarded)
                printf("[*] Discarded %zu stale bulk bytes before "
                       "reflection event %u.\n",
                       discarded, expected_kind);
            free(packet);
            return true;
        }
    }
    fprintf(stderr,
            "[-] Reflection event %u timed out after discarding %zu "
            "stale bytes.\n",
            expected_kind, discarded);
    free(packet);
    return false;
}

static bool auth_set_report(psvr2_device *device,
                            const uint8_t report[64]) {
    return psvr2_device_control(
               device, UINT8_C(0x21), UINT8_C(0x09),
               UINT16_C(0x01f0), 0, (void *)report, 64, 1000) == 64;
}

static bool auth_send_certificate(psvr2_device *device,
                                  const uint8_t certificate[160],
                                  uint8_t authentication_id) {
    uint8_t record[180] = {0};
    memcpy(record + 20, certificate, 160);
    /* VrhmdMain consumes record[4..19] as its challenge.  Use the device's
     * peripheral certificate bytes so this input is nonzero without needing
     * another Secure World call or host RNG dependency. */
    memcpy(record + 4, certificate, 16);
    for (unsigned block = 0; block < 4; ++block) {
        uint8_t report[64] = {0};
        size_t offset = block * 56U;
        size_t count = psvr2_min_size(56U, sizeof(record) - offset);
        report[0] = UINT8_C(0xf0);
        report[1] = 1;
        report[2] = authentication_id;
        report[3] = (uint8_t)block;
        memcpy(report + 4, record + offset, count);
        auth_finish_report(report);
        if (!auth_set_report(device, report)) {
            fprintf(stderr,
                    "[-] Authentication certificate block %u failed.\n",
                    block);
            memset(record, 0, sizeof(record));
            return false;
        }
    }
    memset(record, 0, sizeof(record));
    return true;
}

static bool auth_drain_device_response(psvr2_device *device,
                                       uint8_t authentication_id) {
    for (unsigned block = 0; block < 4; ++block) {
        bool accepted = false;
        double deadline = psvr2_now() + 3.0;
        while (psvr2_now() < deadline) {
            uint8_t report[64] = {0};
            int result = psvr2_device_hid_get(
                device, UINT8_C(0xf1), 0, report, sizeof(report));
            if (result == (int)sizeof(report) &&
                report[0] == UINT8_C(0xf1) && report[1] == 1 &&
                report[2] == authentication_id &&
                report[3] == block) {
                accepted = true;
                break;
            }
            psvr2_sleep_ms(25);
        }
        if (!accepted) {
            fprintf(stderr,
                    "[-] Timed out draining device response block %u.\n",
                    block);
            return false;
        }
    }
    return true;
}

static int cmd_auth_reflect(psvr2_shell *shell, int argc, char **argv) {
    auth_reflection_event certificate;
    auth_reflection_event response;
    auth_reflection_event terminal;
    psvr2_device *device = shell->runtime->krw->ex->usb;
    int64_t command_result = -1;
    uint64_t generation = 0;
    uint8_t authentication_id = 0;
    uint8_t final_report[64] = {0};
    char arm_command[256];
    uint32_t firmware;
    uint16_t event_firmware;
    bool armed = false;
    bool success = false;

    (void)argv;
    if (argc != 1) {
        puts("Usage: auth_reflect");
        return 1;
    }
    firmware = shell->runtime->krw->ex->constants->version;
    if (firmware != PSVR2_FW_0110 && firmware != PSVR2_FW_0600) {
        fputs("[-] auth_reflect is certified only for firmware "
              "01.10 and 06.00.\n", stderr);
        return 1;
    }
    event_firmware = (uint16_t)(firmware >> 16);
    if (!psvr2_stage1_discover(shell->runtime) ||
        shell->runtime->mailbox.firmware != event_firmware) {
        fprintf(stderr,
                "[-] A matching %s Stage1 mailbox is required.\n",
                psvr2_firmware_name(firmware));
        return 1;
    }
    if (!psvr2_shell_find_bulk_endpoints(shell) || !shell->bulk_in) {
        fputs("[-] Stage1 bulk-IN endpoint is unavailable.\n", stderr);
        return 1;
    }
    if (!psvr2_shell_stop_and_drain_bulk_in(shell)) {
        fputs("[-] Could not synchronize the Stage1 bulk-IN endpoint.\n",
              stderr);
        return 1;
    }

    int written = snprintf(
        arm_command, sizeof(arm_command),
        "%s killall -9 VrhmdMain >/dev/null 2>&1 || true; "
        "%s sleep 1; echo auth_reflect_launch > /proc/stage1",
        shell->busybox_path, shell->busybox_path);
    if (written < 0 || (size_t)written >= sizeof(arm_command)) {
        fputs("[-] Could not construct the controlled launch command.\n",
              stderr);
        return 1;
    }

    puts("[*] Replacing VrhmdMain with an armed controlled launch...");
    if (!psvr2_stage1_exec(
            shell->runtime, arm_command,
            5.0, &command_result, NULL) || command_result != 0) {
        fprintf(stderr,
                "[-] Stage1 reflection arm failed (exit=%lld).\n",
                (long long)command_result);
        return 1;
    }
    armed = true;

    memset(&certificate, 0, sizeof(certificate));
    if (!auth_read_bulk_event(
            shell, AUTH_REFLECTION_EVENT_CERTIFICATE, 0,
            event_firmware, &certificate, 65000) ||
        certificate.result != 0 ||
        certificate.payload_length != 160) {
        fprintf(stderr,
                "[-] Device-certificate event failed (result=%d).\n",
                certificate.result);
        goto out;
    }
    generation = certificate.generation;
    puts("[+] Received the device-backed reflected certificate event.");

    if (!auth_send_certificate(
            device, certificate.payload, authentication_id))
        goto out;

    memset(&response, 0, sizeof(response));
    if (!auth_read_bulk_event(
            shell, AUTH_REFLECTION_EVENT_RESPONSE, generation,
            event_firmware, &response, 17000) || response.result != 0 ||
        response.payload_length != 60) {
        fprintf(stderr,
                "[-] Exact-session response event failed (result=%d).\n",
                response.result);
        goto out;
    }
    if (response.payload[0] != UINT8_C(0xf0) ||
        response.payload[1] != 2 ||
        response.payload[2] != authentication_id) {
        fputs("[-] Exact-session response framing mismatch.\n", stderr);
        goto out;
    }
    puts("[+] Captured and synthesized the exact selector-13 session.");

    if (!auth_drain_device_response(device, authentication_id))
        goto out;
    memcpy(final_report, response.payload, 60);
    auth_finish_report(final_report);
    if (!auth_set_report(device, final_report)) {
        fputs("[-] Final host authentication response failed.\n", stderr);
        goto out;
    }

    memset(&terminal, 0, sizeof(terminal));
    if (!auth_read_bulk_event(
            shell, AUTH_REFLECTION_EVENT_RESULT, generation,
            event_firmware, &terminal, 5000) || terminal.result != 0 ||
        terminal.payload_length != 1 || terminal.payload[0] != 0x40) {
        fprintf(stderr,
                "[-] VrhmdMain rejected the reflected session "
                "(result=%d state=0x%02x).\n",
                terminal.result,
                terminal.payload_length ? terminal.payload[0] : 0);
        goto out;
    }
    success = true;
    armed = false;
    puts("[+] VrhmdMain reached AUTHENTICATE_OK (state 0x40).");
    puts("[+] Stage1 restored its normal authentication bypass.");

out:
    memset(&certificate, 0, sizeof(certificate));
    memset(&response, 0, sizeof(response));
    memset(&terminal, 0, sizeof(terminal));
    memset(final_report, 0, sizeof(final_report));
    if (armed) {
        puts("[*] Waiting for Stage1's bounded fail-safe to restore transport...");
        psvr2_sleep_ms(17000);
    } else {
        psvr2_sleep_ms(250);
    }
    return success ? 0 : 1;
}

static char *join_arguments(int argc, char **argv, int start) {
    psvr2_buffer buffer = {0};
    for (int index = start; index < argc; ++index) {
        if (index > start) {
            const char space = ' ';
            if (!psvr2_buffer_append(&buffer, &space, 1)) goto fail;
        }
        if (!psvr2_buffer_append(
                &buffer, argv[index], strlen(argv[index])))
            goto fail;
    }
    const char terminator = '\0';
    if (!psvr2_buffer_append(&buffer, &terminator, 1)) goto fail;
    return (char *)buffer.data;

fail:
    psvr2_buffer_free(&buffer);
    return NULL;
}

static int execute_remote_command(psvr2_shell *shell, int argc,
                                  char **argv, bool stage1) {
    if (argc < 2) return 1;
    char *command_line = join_arguments(argc, argv, 1);
    if (!command_line) return 1;

    int64_t result = -1;
    psvr2_buffer output = {0};
    bool ok = stage1
        ? psvr2_stage1_exec(
              shell->runtime, command_line, 15.0, &result, &output)
        : psvr2_exec(
              shell->runtime, command_line, true, &result, &output);
    if (output.len) {
        fwrite(output.data, 1, output.len, stdout);
        if (output.data[output.len - 1] != '\n') putchar('\n');
    }
    printf("[exit %lld]\n", (long long)result);
    psvr2_buffer_free(&output);
    free(command_line);
    return ok ? 0 : 1;
}

int psvr2_shell_stage1_execute_line(
    psvr2_shell *shell, const char *command)
{
    if (!shell || !command || !*command)
        return 1;

    int64_t result = -1;
    psvr2_buffer output = {0};
    bool ok = psvr2_stage1_exec(
        shell->runtime, command, 15.0, &result, &output);
    if (output.len) {
        fwrite(output.data, 1, output.len, stdout);
        if (output.data[output.len - 1] != '\n')
            putchar('\n');
    }
    printf("[exit %lld]\n", (long long)result);
    psvr2_buffer_free(&output);
    return ok && result == 0 ? 0 : 1;
}

static int run_fixed_stage1_command(psvr2_shell *shell,
                                    const char *command) {
    char *arguments[] = {"s1exec", (char *)command};
    return execute_remote_command(shell, 2, arguments, true);
}

static int cmd_exec(psvr2_shell *shell, int argc, char **argv) {
    return execute_remote_command(shell, argc, argv, false);
}

static int cmd_stage1_exec(psvr2_shell *shell, int argc, char **argv) {
    return execute_remote_command(shell, argc, argv, true);
}

static int cmd_upload(psvr2_shell *shell, int argc, char **argv) {
    if (argc < 2 || argc > 3) return 1;
    return psvr2_upload_file(
               shell->runtime, argv[1], argc == 3 ? argv[2] : NULL)
        ? 0
        : 1;
}

static int cmd_stage1(psvr2_shell *shell, int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : NULL;
    return psvr2_load_stage1(
               shell->runtime, path,
               shell->force_tmp || path != NULL, path != NULL)
        ? 0
        : 1;
}

static int cmd_inject(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return psvr2_inject_str_shellcode(shell->runtime) ? 0 : 1;
}

static int cmd_chmod(psvr2_shell *shell, int argc, char **argv) {
    if (argc != 3) return 1;
    char *arguments[] = {"s1exec", "chmod", argv[1], argv[2]};
    return execute_remote_command(shell, 4, arguments, true);
}

static int cmd_fts_info(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return run_fixed_stage1_command(
        shell,
        "/sie/bin/fts.elf get disable_autoboot; "
        "/sie/bin/fts.elf get disable_wdt");
}

static int cmd_fts_fix(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return run_fixed_stage1_command(
        shell,
        "/sie/bin/fts.elf set disable_autoboot 0; "
        "/sie/bin/fts.elf set disable_wdt 0");
}

static int cmd_recv_test(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return run_fixed_stage1_command(
        shell,
        "echo 'recv /tmp/test 100' > /proc/stage1; "
        "read line < /proc/stage1; echo \"$line\"");
}

bool psvr2_shell_build_serial_command(
    const char *directory, const char *busybox,
    bool reset, bool noevict, bool double_evict, bool reload,
    psvr2_buffer *out)
{
    if (!directory || directory[0] != '/' ||
        !busybox || busybox[0] != '/' || !out)
        return false;

    char command[512];
    int written;
    if (reload) {
        written = snprintf(
            command, sizeof(command),
            "if %s grep -q '^stage3_serial ' /proc/modules; then "
            "echo stage3_serial > /proc/rmmod_helper || exit 1; "
            "s=$(%s cat /proc/rmmod_helper); echo \"$s\"; "
            "case \"$s\" in *\"Status: OK:\"*) ;; *) exit 1;; esac; "
            "fi; "
            "%s sleep 1 && "
            "insmod %s/stage3_serial.ko usb_reset=%d "
            "bb_path=%s%s%s",
            busybox, busybox, busybox, directory,
            reset ? 1 : 0, busybox,
            noevict ? " noevict=1" : "",
            double_evict ? " double_evict=1" : "");
    } else {
        written = snprintf(
            command, sizeof(command),
            "insmod %s/u_serial.ko && "
            "%s sleep 1 && "
            "insmod %s/usb_f_acm.ko && "
            "%s sleep 1 && "
            "insmod %s/stage3_serial.ko usb_reset=%d "
            "bb_path=%s%s%s",
            directory, busybox, directory, busybox, directory,
            reset ? 1 : 0, busybox,
            noevict ? " noevict=1" : "",
            double_evict ? " double_evict=1" : "");
    }
    if (written < 0 || (size_t)written >= sizeof(command))
        return false;

    const char terminator = '\0';
    return psvr2_buffer_append(
               out, command, (size_t)written) &&
           psvr2_buffer_append(out, &terminator, 1);
}

static bool serial_directory_ready(
    psvr2_shell *shell, uint64_t root, const char *directory,
    bool stage3_only)
{
    static const char * const names[] = {
        "u_serial.ko", "usb_f_acm.ko", "stage3_serial.ko"
    };
    char path[128];
    size_t first = stage3_only ? PSVR2_ARRAY_LEN(names) - 1 : 0;
    for (size_t index = first;
         index < PSVR2_ARRAY_LEN(names); ++index) {
        int written = snprintf(
            path, sizeof(path), "%s/%s", directory, names[index]);
        if (written < 0 || (size_t)written >= sizeof(path) ||
            !psvr2_vfs_resolve_path(&shell->vfs, root, path))
            return false;
    }
    return true;
}

static bool serial_status(
    psvr2_shell *shell, bool require_ok,
    bool require_active, bool announce)
{
    int64_t result = -1;
    psvr2_buffer output = {0};
    char command[128];
    snprintf(
        command, sizeof(command), "%s cat /proc/stage3 2>/dev/null",
        shell->busybox_path);
    bool executed = psvr2_stage1_exec(
        shell->runtime, command, 5.0, &result, &output);
    const char *status = output.data
        ? psvr2_trim((char *)output.data) : NULL;
    if (announce && status && *status)
        printf("    /proc/stage3: %s\n", status);
    else if (announce)
        puts("    /proc/stage3: (not found)");
    bool ok = executed && result == 0 && status &&
              (!require_ok || !strncmp(status, "OK", 2)) &&
              (!require_active ||
               strstr(status, "active after USB reset"));
    psvr2_buffer_free(&output);
    return ok;
}

static bool serial_shell_healthy(psvr2_shell *shell) {
    psvr2_runtime *runtime = shell->runtime;
    psvr2_krw *krw = runtime ? runtime->krw : NULL;
    psvr2_exploit *exploit = krw ? krw->ex : NULL;
    psvr2_device *device = exploit ? exploit->usb : NULL;
    psvr2_constants *constants = exploit ? exploit->constants : NULL;
    if (!device || !device->handle || !device->serial_safe || !constants ||
        !constants->live_profile_certified ||
        !constants->fw.exact_image_baseline_present ||
        constants->certified_connection_generation != device->connection_generation ||
        !psvr2_find_module(krw, "stage3_serial") ||
        !psvr2_find_module(krw, "u_serial") ||
        !psvr2_find_module(krw, "usb_f_acm"))
        return false;

    /* Current Stage3's wrapper publishes both PIDs and keeps only the child
     * attached to ttyGS0. Inspect this live state without opening the tty or
     * changing any endpoint; mutable /proc/stage3 text is a last-operation log. */
    char command[512];
    int written = snprintf(command, sizeof(command),
        "test -r /proc/stage3 && test -w /proc/stage3 && "
        "test -c /dev/ttyGS0 && test ! -e /tmp/.stage3_shell_stop || exit 1; "
        "read P </tmp/.stage3_shell_pid && read C </tmp/.stage3_shell_child || exit 1; "
        "case $P:$C in *[!0-9:]*|:*|*:) exit 1;; esac; "
        "[ \"$P\" -gt 1 ] && [ \"$C\" -gt 1 ] && kill -0 \"$P\" \"$C\" || exit 1; "
        "for F in 0 1 2; do [ \"$(%s readlink /proc/$C/fd/$F)\" = /dev/ttyGS0 ] || exit 1; done",
        shell->busybox_path);
    if (written < 0 || (size_t)written >= sizeof(command)) return false;
    int64_t result = -1;
    return psvr2_stage1_exec(runtime, command, 5.0, &result, NULL) && result == 0;
}

static bool serial_reset_pending(psvr2_shell *shell) {
    char command[128];
    snprintf(command, sizeof(command), "%s cat /proc/stage3 2>/dev/null",
             shell->busybox_path);
    int64_t result = -1;
    psvr2_buffer output = {0};
    bool executed = psvr2_stage1_exec(shell->runtime, command, 5.0,
                                      &result, &output);
    const char *status = output.data ? psvr2_trim((char *)output.data) : NULL;
    static const char pending[] = " (USB reset pending)";
    size_t length = status ? strlen(status) : 0U;
    bool ok = executed && result == 0 && status &&
        !strncmp(status, "OK acm x", 8U) &&
        strstr(status, " injected iface_next=") &&
        length >= sizeof(pending) - 1U &&
        !memcmp(status + length - (sizeof(pending) - 1U),
                pending, sizeof(pending) - 1U);
    psvr2_buffer_free(&output);
    return ok;
}

static bool serial_recover_after_reset(psvr2_shell *shell) {
    psvr2_runtime *runtime = shell->runtime;
    psvr2_krw *krw = runtime->krw;
    psvr2_exploit *exploit = krw->ex;
    psvr2_device *device = exploit->usb;
    bool disconnected = false;
    double started = psvr2_now();

    puts("[*] Waiting for the scheduled Stage3 USB reset...");
    double deadline = started + 8.0;
    while (psvr2_now() < deadline) {
        if (!psvr2_device_present(device)) {
            disconnected = true;
            break;
        }
        psvr2_sleep_ms(100);
    }
    if (!disconnected)
        puts("[!] USB disconnect was too brief to observe; "
             "refreshing the device handle.");

    if (!psvr2_device_wait_ready(device, 10000U)) {
        fputs("[-] PSVR2 did not return after the Stage3 USB reset.\n",
              stderr);
        return false;
    }

    psvr2_runtime_reset_after_reconnect(runtime);
    psvr2_result read_setup =
        psvr2_exploit_recover_installed_reader(exploit);
    if (!psvr2_result_is_ok(&read_setup))
        read_setup =
            psvr2_exploit_initialize_result(exploit, false);
    if (!psvr2_result_is_ok(&read_setup)) {
        fprintf(stderr, "[-] Post-reset arbitrary-read recovery failed: %s\n",
                read_setup.message);
        return false;
    }
    if (!psvr2_kernel_certify_live_profile(krw)) {
        fputs("[-] Post-reset live kernel certification failed.\n", stderr);
        return false;
    }
    psvr2_result write_setup =
        psvr2_krw_setup_write_result(krw, false, false);
    if (!psvr2_result_is_ok(&write_setup)) {
        fprintf(stderr, "[-] Post-reset write recovery failed: %s\n",
                write_setup.message);
        return false;
    }
    if (!psvr2_inject_str_shellcode(runtime) ||
        !psvr2_stage1_discover(runtime)) {
        fputs("[-] Post-reset kernel runtime recovery failed.\n", stderr);
        return false;
    }

    psvr2_vfs_init(&shell->vfs, krw);
    shell->cwd = psvr2_vfs_root(&shell->vfs);
    strcpy(shell->cwd_name, "/");
    printf("[+] Native kernel session recovered after the USB reset "
           "in %.3fs.\n",
           psvr2_now() - started);
    return true;
}

static bool serial_wait_active(psvr2_shell *shell) {
    double deadline = psvr2_now() + 5.0;
    do {
        if (serial_shell_healthy(shell)) {
            return serial_status(shell, false, false, true);
        }
        psvr2_sleep_ms(250);
    } while (psvr2_now() < deadline);
    (void)serial_status(shell, false, false, true);
    return false;
}

static int cmd_serial(psvr2_shell *shell, int argc, char **argv) {
    bool reset = true;
    bool reload = false;
    bool status_only = false;
    bool double_evict = shell->double_evict;
    bool mode_seen = false;
    if (argc > 3)
        return 1;
    for (int index = 1; index < argc; ++index) {
        if (!strcmp(argv[index], "double-evict")) {
            if (double_evict && !shell->double_evict)
                return 1;
            double_evict = true;
        } else if (mode_seen) {
            return 1;
        } else if (!strcmp(argv[index], "noreset")) {
            reset = false;
            mode_seen = true;
        } else if (!strcmp(argv[index], "reload")) {
            reset = false;
            reload = true;
            mode_seen = true;
        } else if (!strcmp(argv[index], "reset")) {
            reset = true;
            reload = true;
            mode_seen = true;
        } else if (!strcmp(argv[index], "status")) {
            status_only = true;
            mode_seen = true;
        } else {
            return 1;
        }
    }

    if (!psvr2_shell_busybox_available(shell)) {
        fputs("[-] Serial deployment requires BusyBox at "
              "/data/modules/busybox or /tmp/busybox.\n", stderr);
        return 1;
    }
    if (status_only)
        return serial_status(shell, false, false, true) ? 0 : 1;

    /* "serial reset" means reload when Stage3 is already present, but on a
     * clean boot it must still install the u_serial/usb_f_acm dependency
     * chain.  Loading Stage3 alone otherwise fails later with ACM -ENOENT. */
    bool stage3_present =
        psvr2_find_module(shell->runtime->krw, "stage3_serial") != 0;
    if (reload && !stage3_present)
        reload = false;

    if (!reload && !double_evict) {
        if (serial_shell_healthy(shell)) {
            if (!serial_status(shell, false, false, true)) {
                fputs("[-] Stage3 status could not be read; existing "
                      "serial configuration was preserved.\n", stderr);
                return 1;
            }
            puts("[+] Stage3 serial is already active; deployment skipped.");
            return 0;
        }
        if (stage3_present &&
            !shell->runtime->krw->ex->usb->serial_safe &&
            serial_reset_pending(shell)) {
            puts("[*] Stage3 is awaiting its scheduled USB reset; "
                 "deployment skipped.");
            return serial_recover_after_reset(shell) &&
                   serial_wait_active(shell) ? 0 : 1;
        }
        if (stage3_present || shell->runtime->krw->ex->usb->serial_safe) {
            fputs("[-] Existing serial state could not be verified; "
                  "automatic deployment/reset was blocked. "
                  "Use --no-serial for maintenance.\n", stderr);
            return 1;
        }
    }

    uint64_t root = psvr2_vfs_root(&shell->vfs);
    if (!root) {
        fputs("[-] Cannot inspect the device filesystem.\n", stderr);
        return 1;
    }
    const char *directory = NULL;
    if (!shell->force_tmp &&
        serial_directory_ready(
            shell, root, "/data/modules", reload))
        directory = "/data/modules";
    else if (serial_directory_ready(
                 shell, root, "/tmp", reload))
        directory = "/tmp";
    if (!directory) {
        fputs("[-] Missing u_serial.ko, usb_f_acm.ko, or "
              "stage3_serial.ko in the selected device directory.\n",
              stderr);
        return 1;
    }

    if (reload) {
        int64_t result = -1;
        if (!psvr2_stage1_exec(
                shell->runtime,
                "test -w /proc/rmmod_helper",
                5.0, &result, NULL) ||
            result != 0) {
            fputs("[-] serial reload requires the active "
                  "rmmod_helper module.\n", stderr);
            return 1;
        }
    }

    psvr2_buffer command = {0};
    bool built = psvr2_shell_build_serial_command(
        directory, shell->busybox_path, reset,
        true, double_evict, reload, &command);
    if (!built) {
        psvr2_buffer_free(&command);
        return 1;
    }

    int64_t result = -1;
    psvr2_buffer output = {0};
    bool executed = psvr2_stage1_exec(
        shell->runtime, (char *)command.data,
        15.0, &result, &output);
    if (output.len) {
        fwrite(output.data, 1, output.len, stdout);
        if (output.data[output.len - 1] != '\n')
            putchar('\n');
    }
    psvr2_buffer_free(&output);
    psvr2_buffer_free(&command);
    if ((!executed || result != 0) && !reset) {
        fprintf(stderr,
                "[-] Serial module command failed (exit=%lld).\n",
                (long long)result);
        return 1;
    }

    if (reset) {
        if (executed && result != 0 && result != -1) {
            fprintf(stderr,
                    "[-] Serial module command failed before reset "
                    "(exit=%lld).\n",
                    (long long)result);
            return 1;
        }
        if (!executed || result == -1)
            puts("[*] Stage3 reset interrupted the command mailbox; "
                 "validating USB reconnect and live status.");
        if (!serial_recover_after_reset(shell))
            return 1;
        return serial_wait_active(shell) ? 0 : 1;
    } else {
        psvr2_sleep_ms(1000);
    }
    return serial_status(shell, true, false, true) ? 0 : 1;
}

static int cmd_wdt(psvr2_shell *shell, int argc, char **argv) {
    uint64_t count = 31;
    if (argc > 1 &&
        (!psvr2_parse_u64(argv[1], &count) || count > UINT8_MAX))
        return 1;
    uint8_t payload[] = {1, (uint8_t)count, 0};
    return psvr2_device_vendor_set(
               shell->runtime->krw->ex->usb, 0x23, 1,
               payload, sizeof(payload), 1000)
        ? 0
        : 1;
}

static int cmd_rcm(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return run_fixed_stage1_command(
        shell,
        "echo 2 > "
        "/sys/bus/mmc/devices/mmc0:0001/boot_partition_for_boot");
}

static int cmd_disable_autoboot(psvr2_shell *shell,
                                int argc, char **argv) {
    const char *action = argc > 1 ? argv[1] : "";
    char command[512];
    if (!*action) {
        snprintf(command, sizeof(command),
                 "/sie/bin/fts.elf get disable_autoboot; "
                 "/sie/bin/fts.elf get disable_wdt");
    } else if (!strcmp(action, "on") || !strcmp(action, "off")) {
        int value = !strcmp(action, "on");
        snprintf(
            command, sizeof(command),
            "/sie/bin/fts.elf set disable_autoboot %d", value);
    } else {
        return 1;
    }
    return run_fixed_stage1_command(shell, command);
}

static int cmd_reboot(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    shell->running = false;
    puts("[*] Invoking exact-image immediate restart...");
    bool accepted = psvr2_kernel_force_restart(shell->runtime);
    puts(accepted
             ? "[+] Immediate kernel restart dispatched; leaving the shell."
             : "[!] Immediate kernel restart failed; leaving the shell anyway.");
    return accepted ? 0 : 1;
}

static int cmd_kernel_reboot(
    psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    shell->running = false;
    puts("[*] Invoking exact-image immediate restart...");
    return psvr2_kernel_force_restart(shell->runtime) ? 0 : 1;
}

static int cmd_shutdown(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    shell->running = false;
    puts("[*] Invoking exact-image immediate power off...");
    bool accepted = psvr2_kernel_force_power_off(shell->runtime);
    puts(accepted
             ? "[+] Immediate power off dispatched; leaving the shell."
             : "[!] Immediate power off failed; leaving the shell anyway.");
    return accepted ? 0 : 1;
}

const psvr2_shell_command psvr2_shell_stage1_commands[] = {
    {"exec", cmd_exec, "exec <command> — UMH execution", true},
    {"sh", cmd_stage1_exec, "sh <command> — Stage1 execution", true},
    {"s1exec", cmd_stage1_exec, "s1exec <command>", true},
    {"upload", cmd_upload,
     "upload <local> [remote-name] — page-cache upload", true},
    {"stage1", cmd_stage1, "stage1 [stage1.ko]", true},
    {"auth_reflect", cmd_auth_reflect,
     "Run the bounded 01.10/06.00 device-certificate auth reflection", true},
    {"inject", cmd_inject,
     "Inject/repair native STR shellcode", true},
    {"chmod", cmd_chmod, "chmod <mode> <path>", true},
    {"serial", cmd_serial,
     "serial [noreset|reload|reset|status] [double-evict]", true},
    {"wdt", cmd_wdt,
     "wdt [count] — set boot failure count", true},
    {"rcm", cmd_rcm,
     "Enter recovery mode through stage3", true},
    {"disable_autoboot", cmd_disable_autoboot,
     "disable_autoboot [on|off]", true},
    {"reboot", cmd_reboot, "Immediately restart headset", true},
    {"kernel_reboot", cmd_kernel_reboot,
     "Force an exact-image emergency restart", true},
    {"shutdown", cmd_shutdown, "Immediately power off headset", true},
    {"poweroff", cmd_shutdown, "Alias for immediate shutdown", true},
    {"fts_info", cmd_fts_info,
     "Read persistent FTS boot flags", true},
    {"fts_fix", cmd_fts_fix,
     "Clear persistent FTS boot flags", true},
    {"recv_test", cmd_recv_test,
     "Exercise Stage1 bulk receiver", true}
};

const size_t psvr2_shell_stage1_command_count =
    PSVR2_ARRAY_LEN(psvr2_shell_stage1_commands);
