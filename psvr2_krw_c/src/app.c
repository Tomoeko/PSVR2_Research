#include "psvr2/app.h"

#include "psvr2/audit.h"
#include "psvr2/kernel_exec.h"
#include "psvr2/module_helper.h"
#include "psvr2/shell.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    bool serial;
    bool no_serial;
    bool force_tmp;
    bool s1off;
    bool no_rmmod_helper;
    bool read_only;
    bool kernel_probe;
    bool safety_audit;
    bool reboot;
    bool noevict;
    bool double_evict;
    bool help;
    bool version;
    const char *firmware;
    const char *stage1;
    const char *command;
    const char **fast_files;
    size_t fast_count;
} app_options;

typedef struct {
    psvr2_device device;
    psvr2_constants constants;
    psvr2_exploit exploit;
    psvr2_krw krw;
    psvr2_runtime runtime;
    psvr2_shell shell;
    bool device_initialized;
    bool exploit_initialized;
    bool shell_initialized;
} app_context;

static void print_usage(FILE *out, const char *program) {
    fprintf(out,
        "PSVR2 Kernel R/W Toolkit — native C\n\n"
        "Usage: sudo %s [options]\n\n"
        "  --fw VERSION       Force firmware 01.10/06.00 (or numeric ID)\n"
        "  --read-only        Bootstrap kernel read; block later writes\n"
        "  --reboot           Invoke immediate emergency_restart() and exit\n"
        "  --kernel-probe     Reversible exact-image kernel execution probe\n"
        "  --safety-audit     Read-only exact-image live memory audit\n"
        "  --stage1 FILE      Replace/load Stage1 from FILE through /tmp\n"
        "  --s1off            Do not discover or auto-load Stage1\n"
        "  --no-rmmod-helper  Do not auto-load persistent rmmod_helper\n"
        "  --fast FILE        Bulk-upload FILE after initialization (repeatable)\n"
        "  --serial           Deploy serial module chain (the default)\n"
        "  --no-serial        Skip automatic serial module deployment\n"
        "  --tmp              Ignore persistent /data/modules files\n"
        "  --noevict          Preserve Sony endpoints during serial deployment\n"
        "  --double-evict     Evict data8/data9 and add controller ACM port\n"
        "  --command TEXT     Execute one native shell command and exit\n"
        "  --version          Print version and exit\n"
        "  -h, --help         Show this help\n", program);
}

static void options_destroy(app_options *options) {
    free(options->fast_files);
    options->fast_files = NULL;
    options->fast_count = 0;
}

static bool options_add_fast(app_options *options, const char *path) {
    const char **next = realloc(
        options->fast_files,
        (options->fast_count + 1) * sizeof(*next));
    if (!next) return false;
    options->fast_files = next;
    options->fast_files[options->fast_count++] = path;
    return true;
}

static bool options_parse(app_options *options, int argc, char **argv) {
    memset(options, 0, sizeof(*options));
    for (int i = 1; i < argc; ++i) {
        const char *argument = argv[i];
        if (!strcmp(argument, "-h") || !strcmp(argument, "--help"))
            options->help = true;
        else if (!strcmp(argument, "--version"))
            options->version = true;
        else if (!strcmp(argument, "--serial"))
            options->serial = true;
        else if (!strcmp(argument, "--no-serial"))
            options->no_serial = true;
        else if (!strcmp(argument, "--tmp"))
            options->force_tmp = true;
        else if (!strcmp(argument, "--s1off"))
            options->s1off = true;
        else if (!strcmp(argument, "--no-rmmod-helper"))
            options->no_rmmod_helper = true;
        else if (!strcmp(argument, "--read-only"))
            options->read_only = true;
        else if (!strcmp(argument, "--kernel-probe"))
            options->kernel_probe = true;
        else if (!strcmp(argument, "--safety-audit"))
            options->safety_audit = true;
        else if (!strcmp(argument, "--reboot"))
            options->reboot = true;
        else if (!strcmp(argument, "--noevict"))
            options->noevict = true;
        else if (!strcmp(argument, "--double-evict"))
            options->double_evict = true;
        else if (!strcmp(argument, "--fw") && i + 1 < argc)
            options->firmware = argv[++i];
        else if (!strcmp(argument, "--stage1") && i + 1 < argc)
            options->stage1 = argv[++i];
        else if (!strcmp(argument, "--command") && i + 1 < argc)
            options->command = argv[++i];
        else if (!strcmp(argument, "--fast") && i + 1 < argc) {
            if (!options_add_fast(options, argv[++i])) return false;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argument);
            return false;
        }
    }
    return true;
}

static bool options_valid(const app_options *options) {
    if (options->serial && options->no_serial) {
        fputs("[-] --serial and --no-serial cannot be used together.\n",
              stderr);
        return false;
    }
    unsigned standalone_count =
        (options->kernel_probe ? 1U : 0U) +
        (options->safety_audit ? 1U : 0U) +
        (options->reboot ? 1U : 0U);
    if (!standalone_count) return true;
    if (standalone_count != 1U) {
        fputs("[-] Select only one standalone operation: --kernel-probe, "
              "--safety-audit, or --reboot.\n",
              stderr);
        return false;
    }
    if (options->read_only || options->serial || options->no_serial ||
        options->force_tmp ||
        options->s1off || options->no_rmmod_helper ||
        options->noevict || options->double_evict || options->stage1 ||
        options->fast_count || options->command || options->firmware) {
        fprintf(stderr, "[-] %s must be used alone and requires "
                        "auto-detected firmware.\n",
                options->kernel_probe ? "--kernel-probe"
                    : options->safety_audit ? "--safety-audit"
                                            : "--reboot");
        return false;
    }
    return true;
}

static bool firmware_supported(uint32_t firmware) {
    return firmware == PSVR2_FW_0110 ||
           firmware == PSVR2_FW_0600;
}

static void report_result(const char *operation, const psvr2_result *result) {
    if (!result || psvr2_result_is_ok(result)) return;
    fprintf(
        stderr, "[-] %s: %s%s%s",
        operation, psvr2_status_name(result->status),
        result->message[0] ? " — " : "",
        result->message);
    if (result->native_code)
        fprintf(stderr, " (native=%d)", result->native_code);
    fputc('\n', stderr);
}

static bool connect_device(app_context *app, uint32_t forced,
                           uint32_t *firmware) {
    printf("[1/5] Connecting to PSVR2%s...\n",
           forced ? " with firmware override" : "");
    double deadline =
        psvr2_now() +
        (double)PSVR2_DEVICE_STARTUP_TIMEOUT_MS / 1000.0;
    *firmware = forced;
    while (psvr2_now() < deadline) {
        if (!app->device.handle &&
            !psvr2_device_connect(&app->device)) {
            psvr2_sleep_ms(500);
            continue;
        }
        if (forced) {
            uint32_t reported = 0;
            if (psvr2_device_get_firmware(
                    &app->device, &reported) && reported) {
                if (!firmware_supported(reported)) {
                    fprintf(stderr,
                            "[-] Device returned unsupported firmware "
                            "0x%08x while validating --fw.\n",
                            reported);
                    return false;
                }
                if (reported != forced) {
                    fprintf(stderr,
                            "[-] Refusing --fw %s: the connected device "
                            "reports %s (0x%08x).\n",
                            psvr2_firmware_name(forced),
                            psvr2_firmware_name(reported), reported);
                    return false;
                }
                puts("[+] Firmware override matches the device report.");
            } else {
                fputs("[-] Firmware report unavailable; refusing to execute "
                      "a forced profile before an exact device match.\n",
                      stderr);
                return false;
            }
            return true;
        }
        if (psvr2_device_get_firmware(&app->device, firmware)) {
            if (firmware_supported(*firmware)) return true;
            /*
             * The device can acknowledge the firmware report with a zero
             * payload during early reboot. Keep using the bounded readiness
             * window for that transient state, but reject any concrete
             * unsupported firmware immediately.
             */
            if (*firmware == 0) {
                psvr2_sleep_ms(500);
                continue;
            }
            fprintf(stderr,
                    "[-] Device returned unsupported firmware 0x%08x.\n",
                    *firmware);
            return false;
        }
        psvr2_sleep_ms(500);
    }
    fputs(app->device.handle
              ? "[-] Firmware query failed or returned an unsupported version.\n"
              : "[-] Device is not visible to libusb in this process. "
                "Check the cable and direct USB permissions; a sandbox can "
                "hide a headset that macOS still lists.\n",
          stderr);
    return false;
}

static const char *find_stage1_path(const app_options *options,
                                    uint32_t firmware) {
    if (options->stage1) return options->stage1;
    static char candidates[2][160];
    const char *name = psvr2_firmware_name(firmware);
    snprintf(candidates[0], sizeof(candidates[0]),
             "output/psvr2-build/%s/modules/stage1.ko", name);
    snprintf(candidates[1], sizeof(candidates[1]),
             "../output/psvr2-build/%s/modules/stage1.ko", name);
    for (unsigned i = 0; i < PSVR2_ARRAY_LEN(candidates); ++i)
        if (psvr2_file_exists(candidates[i])) return candidates[i];
    return NULL;
}

static bool append_shell_argument(psvr2_buffer *command,
                                  const char *argument) {
    static const char prefix[] = " \"";
    if (!command || !argument ||
        !psvr2_buffer_append(command, prefix, sizeof(prefix) - 1))
        return false;
    for (const char *cursor = argument; *cursor; ++cursor) {
        if ((*cursor == '\\' || *cursor == '\"') &&
            !psvr2_buffer_append(command, "\\", 1))
            return false;
        if (!psvr2_buffer_append(command, cursor, 1))
            return false;
    }
    return psvr2_buffer_append(command, "\"", 1);
}

static bool run_fast_uploads(psvr2_shell *shell,
                             const app_options *options) {
    if (!options->fast_count) return true;
    psvr2_buffer command = {0};
    static const char name[] = "fast_upload";
    bool ok = psvr2_buffer_append(
        &command, name, sizeof(name) - 1);
    for (size_t i = 0; ok && i < options->fast_count; ++i)
        ok = append_shell_argument(&command, options->fast_files[i]);
    char zero = '\0';
    ok = ok && psvr2_buffer_append(&command, &zero, 1) &&
         psvr2_shell_execute(shell, (char *)command.data) == 0;
    psvr2_buffer_free(&command);
    if (!ok) fputs("[-] Fast upload failed.\n", stderr);
    return ok;
}

static bool run_kernel_probe(app_context *app, uint32_t firmware) {
    puts("[4/5] Running reversible kernel execution probe...");
    psvr2_result probe =
        psvr2_krw_kernel_execution_probe_result(&app->krw, true);
    bool probe_ok = psvr2_result_is_ok(&probe);
    if (!probe_ok)
        report_result("Kernel execution probe failed", &probe);
    uint32_t health_firmware = 0;
    uint8_t health_byte = 0;
    bool usb_ok =
        psvr2_device_get_firmware(
            &app->device, &health_firmware) &&
        health_firmware == firmware;
    bool read_ok = psvr2_krw_read_ex(
        &app->krw, app->constants.fw.kernel_probe_byte,
        &health_byte, 1, true, false);
    printf(usb_ok
               ? "[+] Post-probe USB firmware query passed.\n"
               : "[-] Post-probe USB firmware query failed.\n");
    printf(read_ok
               ? "[+] Post-probe arbitrary read passed.\n"
               : "[-] Post-probe arbitrary read failed.\n");
    return probe_ok && usb_ok && read_ok;
}

static void app_cleanup(app_context *app) {
    if (app->shell_initialized)
        psvr2_shell_destroy(&app->shell);
    if (app->exploit_initialized)
        psvr2_exploit_destroy(&app->exploit);
    if (app->device_initialized)
        psvr2_device_close(&app->device);
}

int psvr2_app_main(int argc, char **argv) {
    app_options options;
    if (!options_parse(&options, argc, argv)) {
        print_usage(stderr, argv[0]);
        options_destroy(&options);
        return EXIT_FAILURE;
    }
    if (options.help) {
        print_usage(stdout, argv[0]);
        options_destroy(&options);
        return EXIT_SUCCESS;
    }
    if (options.version) {
        puts("psvr2_krw_c 1.0.0");
        options_destroy(&options);
        return EXIT_SUCCESS;
    }
    if (!options_valid(&options)) {
        options_destroy(&options);
        return EXIT_FAILURE;
    }

    app_context app = {0};
    int result = EXIT_FAILURE;
    puts("==================================================\n"
         "  PSVR2 Kernel R/W Toolkit — native C\n"
         "==================================================");

    uint32_t forced_firmware = 0;
    if (options.firmware &&
        (!psvr2_parse_firmware(
             options.firmware, &forced_firmware) ||
         !firmware_supported(forced_firmware))) {
        fprintf(stderr, "[-] Unsupported --fw value: %s\n",
                options.firmware);
        goto done;
    }

    if (psvr2_device_init(&app.device) != 0) {
        fputs("[-] Could not initialize libusb.\n", stderr);
        goto done;
    }
    app.device_initialized = true;
    uint32_t firmware = 0;
    double step_started = psvr2_now();
    bool step_ok = connect_device(
        &app, forced_firmware, &firmware);
    psvr2_report_timing(
        "connect and firmware detection", step_started, step_ok);
    if (!step_ok) goto done;
    if (!psvr2_constants_set_firmware(
            &app.constants, firmware, forced_firmware != 0))
        goto done;
    printf("[+] %s firmware: %s (0x%08x); USB mode: %s\n",
           forced_firmware ? "Forced" : "Detected",
           psvr2_firmware_name(firmware), firmware,
           app.device.serial_safe ? "serial-safe" : "normal");

    psvr2_exploit_init(
        &app.exploit, &app.device, &app.constants);
    app.exploit_initialized = true;
    puts("[2/5] Initializing arbitrary read...");
    step_started = psvr2_now();
    psvr2_result initialization =
        psvr2_exploit_initialize_result(&app.exploit, false);
    step_ok = psvr2_result_is_ok(&initialization);
    psvr2_report_timing(
        "arbitrary-read initialization", step_started, step_ok);
    if (!step_ok) {
        report_result("Exploit initialization failed", &initialization);
        goto done;
    }
    printf("[+] Direct arbitrary read ready (EP0 buffer: 0x%llx)\n",
           (unsigned long long)app.exploit.request_buffer);

    psvr2_krw_init(&app.krw, &app.exploit);
    puts("[+] Certifying selected profile against live kernel text...");
    if (!psvr2_kernel_certify_live_profile(&app.krw)) {
        fprintf(stderr,
                "[-] Selected %s profile does not match the live "
                "exact-image execution anchors; stopping before "
                "write discovery.\n",
                psvr2_firmware_name(firmware));
        goto done;
    }
    puts("[+] Profile passed live exact-image certification.");
    if ((options.kernel_probe || options.safety_audit || options.reboot) &&
        !psvr2_constants_kernel_probe_safe(&app.constants)) {
        fprintf(stderr,
                "[-] Exact memory audit/probe unavailable: "
                "no certified %s binary baseline.\n",
                psvr2_firmware_name(firmware));
        goto done;
    }
    psvr2_runtime_init(&app.runtime, &app.krw);
    if (options.safety_audit) {
        psvr2_safety_report report;
        result = psvr2_memory_safety_audit(&app.runtime, &report)
                     ? EXIT_SUCCESS : EXIT_FAILURE;
        goto done;
    }
    if (options.kernel_probe) {
        psvr2_safety_report report;
        if (!psvr2_memory_safety_audit(&app.runtime, &report) ||
            !report.bounded_probe_safe) {
            fputs("[-] Live read-only safety baseline failed; "
                  "kernel probe blocked.\n", stderr);
            goto done;
        }
    }
    if (!options.read_only) {
        puts("[3/5] Discovering write primitive...");
        step_started = psvr2_now();
        psvr2_result write_setup =
            psvr2_krw_setup_write_result(
                &app.krw, forced_firmware != 0, false);
        step_ok = psvr2_result_is_ok(&write_setup);
        psvr2_report_timing(
            "write-primitive discovery", step_started, step_ok);
        if (!step_ok) {
            report_result("Write primitive unavailable", &write_setup);
            goto done;
        }
        printf("[+] Write ready (spinlock: 0x%llx)\n",
               (unsigned long long)app.krw.spinlock);
        if (options.kernel_probe) {
            result = run_kernel_probe(&app, firmware)
                         ? EXIT_SUCCESS : EXIT_FAILURE;
            goto done;
        }

        puts("[4/5] Injecting native STR shellcode...");
        step_started = psvr2_now();
        step_ok = psvr2_inject_str_shellcode(&app.runtime);
        psvr2_report_timing(
            "STR/helper installation", step_started, step_ok);
        if (!step_ok) {
            fputs("[-] Shellcode injection failed.\n", stderr);
            goto done;
        }
        if (options.reboot) {
            puts("[5/5] Invoking exact-image immediate restart...");
            bool dispatched =
                psvr2_kernel_force_restart(&app.runtime);
            puts(dispatched
                     ? "[+] Immediate kernel restart dispatched."
                     : "[-] Immediate kernel restart dispatch failed.");
            result = dispatched ? EXIT_SUCCESS : EXIT_FAILURE;
            goto done;
        }
        if (!options.no_rmmod_helper &&
            (firmware == PSVR2_FW_0110 || firmware == PSVR2_FW_0600)) {
            psvr2_rmmod_helper_state helper =
                psvr2_ensure_rmmod_helper(&app.runtime);
            if (helper == PSVR2_RMMOD_HELPER_UNAVAILABLE)
                puts("[*] /data/modules/rmmod_helper.ko is not installed; "
                     "automatic loading skipped.");
            else if (helper == PSVR2_RMMOD_HELPER_ERROR)
                puts("[!] rmmod_helper is unavailable; module replacement "
                     "may be limited.");
        }
        if (!options.s1off) {
            puts("[5/5] Discovering/loading Stage1...");
            bool explicit_stage1 = options.stage1 != NULL;
            step_started = psvr2_now();
            step_ok = psvr2_load_stage1(
                &app.runtime, find_stage1_path(&options, firmware),
                options.force_tmp || explicit_stage1,
                explicit_stage1);
            psvr2_report_timing(
                "Stage1 discovery/load", step_started, step_ok);
            if (!step_ok)
                puts("[!] Stage1 is unavailable; UMH and page-cache "
                     "paths remain usable.");
        }
    } else {
        puts("[3/5] Restricted mode: write discovery and "
             "shellcode injection skipped. Initial reader bootstrap "
             "already completed.");
    }

    psvr2_shell_init(
        &app.shell, &app.runtime, options.read_only,
        options.force_tmp, options.noevict,
        options.double_evict);
    app.shell_initialized = true;
    if (!run_fast_uploads(&app.shell, &options))
        goto done;
    if (!options.read_only && !options.no_serial &&
        psvr2_shell_execute(&app.shell, "serial") != 0) {
        fputs("[-] Automatic Stage3 serial recovery failed; "
              "refusing to open an unusable shell.\n"
              "    Re-run with --no-serial for a maintenance session.\n",
              stderr);
        goto done;
    }
    int shell_result =
        options.command
            ? psvr2_shell_execute(&app.shell, options.command)
            : psvr2_shell_run(&app.shell);
    result = shell_result ? EXIT_FAILURE : EXIT_SUCCESS;

done:
    app_cleanup(&app);
    options_destroy(&options);
    return result;
}
