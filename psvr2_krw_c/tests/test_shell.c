#include "test_cases.h"
#include "firmware_0600_internal.h"

#include "line_editor_internal.h"
#include "shell_completion_internal.h"
#include "shell_internal.h"

#include "psvr2/kernel_exec.h"
#include "psvr2/shell.h"
#include "psvr2/shellcode.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void test_shell_dispatch(void) {
    psvr2_shell completion_shell = {.read_only = true};
    psvr2_line_completions completions = {0};
    assert(psvr2_shell_complete_commands(&completion_shell, "fast_", &completions));
    assert(completions.count == 0);
    assert(psvr2_shell_complete_commands(&completion_shell, "hexdump", &completions));
    assert(completions.count == 1 && !strcmp(completions.items[0], "hexdump"));
    psvr2_line_completions_destroy(&completions);
    assert(psvr2_shell_complete_commands(&completion_shell, "krw", &completions));
    assert(completions.count == 1 && !strcmp(completions.items[0], "krw"));
    psvr2_line_completions_destroy(&completions);
    completion_shell.read_only = false;
    assert(psvr2_shell_complete_commands(&completion_shell, "fast_", &completions));
    assert(completions.count == 2);
    psvr2_line_completions_destroy(&completions);

    assert(psvr2_shell_emmc_command_count == 2);
    assert(!strcmp(psvr2_shell_emmc_commands[0].name, "emmc"));
    assert(!strcmp(psvr2_shell_emmc_commands[1].name, "emmc_plain"));
    assert(psvr2_shell_stage1_send_size(
               "WAIT send 0/1652024 st=-115 act=0") == 1652024);
    assert(psvr2_shell_stage1_send_size(
               "OK send 4096/1652024") == 1652024);
    assert(psvr2_shell_stage1_send_size(
               "OK send done 1652024") == 1652024);
    assert(psvr2_shell_stage1_send_size(
               "ERR send open -2 /tmp/missing") == 0);
    assert(psvr2_shell_stage1_send_size("unexpected /1652024") == 0);

    char output_directory[] = "/tmp/psvr2-shell-output.XXXXXX";
    assert(mkdtemp(output_directory));
    char partial[256];
    char destination[256];
    assert(snprintf(partial, sizeof(partial), "%s/result.partial",
                    output_directory) > 0);
    assert(snprintf(destination, sizeof(destination), "%s/result.bin",
                    output_directory) > 0);
    FILE *exclusive = psvr2_shell_open_output_exclusive(partial);
    assert(exclusive);
    assert(fwrite("safe", 1, 4, exclusive) == 4);
    assert(fclose(exclusive) == 0);
    assert(psvr2_shell_finalize_output_exclusive(partial, destination));
    assert(access(partial, F_OK) != 0);
    assert(access(destination, F_OK) == 0);

    exclusive = psvr2_shell_open_output_exclusive(partial);
    assert(exclusive);
    assert(fclose(exclusive) == 0);
    assert(!psvr2_shell_finalize_output_exclusive(partial, destination));
    assert(access(partial, F_OK) == 0);
    assert(unlink(partial) == 0);
    assert(unlink(destination) == 0);
    assert(rmdir(output_directory) == 0);

    psvr2_buffer invocation = {0};
    assert(psvr2_shell_build_busybox_command(
        "/data/modules/busybox", "printf '%s' \"x\"",
        &invocation));
    assert(!strcmp(
        (char *)invocation.data,
        "/data/modules/busybox ash -c 'printf '\\''%s'\\'' \"x\"'"));
    psvr2_buffer_free(&invocation);

    psvr2_buffer mode_invocation = {0};
    assert(psvr2_shell_build_busybox_mode_command(
        "/data/modules/busybox", "/data/user's files",
        "pwd; printf '%s' ok", &mode_invocation));
    assert(strstr(
        (char *)mode_invocation.data,
        "PSVR2_CWD=%s"));
    assert(strstr(
        (char *)mode_invocation.data,
        "/data/user"));
    assert(strstr(
        (char *)mode_invocation.data,
        "s files"));
    psvr2_buffer_free(&mode_invocation);

    psvr2_buffer visible_output = {0};
    static const char cwd_output[] = "hello\n\036PSVR2_CWD=/tmp\036tail\n";
    assert(psvr2_buffer_append(&visible_output, cwd_output, sizeof(cwd_output)));
    --visible_output.len;
    psvr2_shell_consume_busybox_cwd(NULL, &visible_output);
    assert(!strcmp((char *)visible_output.data, "hello\ntail\n"));
    psvr2_buffer_free(&visible_output);

    psvr2_buffer serial = {0};
    assert(psvr2_shell_build_serial_command(
        "/data/modules", "/data/modules/busybox",
        true, true, false, false, &serial));
    assert(strstr(
        (char *)serial.data,
        "insmod /data/modules/u_serial.ko && "
        "/data/modules/busybox sleep 1"));
    assert(strstr(
        (char *)serial.data,
        "stage3_serial.ko usb_reset=1 "
        "bb_path=/data/modules/busybox noevict=1"));
    psvr2_buffer_free(&serial);

    psvr2_buffer reload = {0};
    assert(psvr2_shell_build_serial_command(
        "/tmp", "/tmp/busybox",
        false, false, false, true, &reload));
    assert(strstr(
        (char *)reload.data,
        "echo stage3_serial > /proc/rmmod_helper"));
    assert(strstr(
        (char *)reload.data,
        "case \"$s\" in *\"Status: OK:\"*"));
    assert(strstr(
        (char *)reload.data,
        "stage3_serial.ko usb_reset=0 bb_path=/tmp/busybox"));
    assert(!strstr((char *)reload.data, "u_serial.ko"));
    assert(reload.len < 480);
    psvr2_buffer_free(&reload);

    psvr2_buffer reset_reload = {0};
    assert(psvr2_shell_build_serial_command(
        "/tmp", "/tmp/busybox",
        true, true, false, true, &reset_reload));
    assert(strstr(
        (char *)reset_reload.data,
        "stage3_serial.ko usb_reset=1 "
        "bb_path=/tmp/busybox noevict=1"));
    assert(!strstr((char *)reset_reload.data, "u_serial.ko"));
    psvr2_buffer_free(&reset_reload);

    psvr2_buffer double_evict = {0};
    assert(psvr2_shell_build_serial_command(
        "/tmp", "/tmp/busybox",
        true, true, true, true, &double_evict));
    assert(strstr(
        (char *)double_evict.data,
        "noevict=1 double_evict=1"));
    assert(!strstr((char *)double_evict.data, "u_serial.ko"));
    psvr2_buffer_free(&double_evict);

    psvr2_shell shell = {
        .read_only = true,
        .running = true
    };
    assert(psvr2_shell_execute(&shell, "") == 0);
    assert(psvr2_shell_execute(&shell, "help") == 0);
    assert(psvr2_shell_execute(&shell, "write 0x1 0x2") == 1);
    assert(psvr2_shell_execute(&shell, "passthrough on") == 1);
    assert(psvr2_shell_execute(&shell, "dumpuser") == 1);
    assert(psvr2_shell_execute(&shell, "persist stage1.ko") == 1);
    assert(psvr2_shell_execute(&shell, "does-not-exist") == 1);
    assert(psvr2_shell_execute(&shell, "busybox") == 1);
    assert(psvr2_shell_execute(&shell, "bb true") == 1);
    assert(psvr2_shell_execute(&shell, "krw help") == 0);
    assert(psvr2_shell_execute(&shell, "q") == 0);
    assert(!shell.running);

    psvr2_shell mode_shell = {
        .running = true,
        .busybox_mode = true
    };
    assert(psvr2_shell_execute(&mode_shell, "exit") == 0);
    assert(!mode_shell.busybox_mode);
    assert(mode_shell.running);

    psvr2_shell quit_mode_shell = {
        .running = true,
        .busybox_mode = true
    };
    assert(psvr2_shell_execute(&quit_mode_shell, "q") == 0);
    assert(!quit_mode_shell.running);

    psvr2_device device = {0};
    psvr2_exploit exploit = {.usb = &device};
    psvr2_krw krw = {.ex = &exploit};
    psvr2_runtime runtime = {.krw = &krw};
    psvr2_shell reboot_shell = {
        .runtime = &runtime,
        .running = true
    };
    assert(psvr2_shell_execute(&reboot_shell, "reboot") == 1);
    assert(!reboot_shell.running);

    psvr2_shell busybox_reboot_shell = {
        .runtime = &runtime,
        .running = true,
        .busybox_mode = true
    };
    assert(psvr2_shell_execute(
               &busybox_reboot_shell, "  reboot  ") == 1);
    assert(!busybox_reboot_shell.running);
    assert(!busybox_reboot_shell.busybox_mode);

    psvr2_shell shutdown_shell = {
        .runtime = &runtime,
        .running = true
    };
    assert(psvr2_shell_execute(&shutdown_shell, "shutdown") == 1);
    assert(!shutdown_shell.running);

    psvr2_shell busybox_shutdown_shell = {
        .runtime = &runtime,
        .running = true,
        .busybox_mode = true
    };
    assert(psvr2_shell_execute(
               &busybox_shutdown_shell, "  shutdown  ") == 1);
    assert(!busybox_shutdown_shell.running);
    assert(!busybox_shutdown_shell.busybox_mode);

    psvr2_shell busybox_poweroff_shell = {
        .runtime = &runtime,
        .running = true,
        .busybox_mode = true
    };
    assert(psvr2_shell_execute(
               &busybox_poweroff_shell, "poweroff") == 1);
    assert(!busybox_poweroff_shell.running);
    assert(!busybox_poweroff_shell.busybox_mode);

    psvr2_shell persistence_shell = {0};
    assert(psvr2_shell_execute(&persistence_shell, "persist") == 1);
    assert(psvr2_shell_execute(&persistence_shell, "unpersist") == 1);
    assert(psvr2_shell_execute(
               &persistence_shell, "persist ../stage1.ko") == 1);
}

void test_line_history(void) {
    psvr2_line_editor editor;
    psvr2_line_editor_init(&editor);
    assert(strcmp(
        psvr2_line_editor_previous(&editor, "draft"), "draft") == 0);
    assert(psvr2_line_editor_add_history(&editor, "ls"));
    assert(psvr2_line_editor_add_history(&editor, "cd data"));
    assert(psvr2_line_editor_add_history(&editor, "cd data"));
    assert(editor.count == 2);

    assert(strcmp(
        psvr2_line_editor_previous(&editor, "hexdump "),
        "cd data") == 0);
    assert(strcmp(
        psvr2_line_editor_previous(&editor, NULL), "ls") == 0);
    assert(strcmp(
        psvr2_line_editor_previous(&editor, NULL), "ls") == 0);
    assert(strcmp(psvr2_line_editor_next(&editor), "cd data") == 0);
    assert(strcmp(psvr2_line_editor_next(&editor), "hexdump ") == 0);
    assert(strcmp(psvr2_line_editor_next(&editor), "hexdump ") == 0);

    char command[32];
    for (unsigned index = 0;
         index <= PSVR2_LINE_HISTORY_LIMIT; ++index) {
        snprintf(command, sizeof(command), "command-%u", index);
        assert(psvr2_line_editor_add_history(&editor, command));
    }
    assert(editor.count == PSVR2_LINE_HISTORY_LIMIT);
    assert(strcmp(editor.entries[0], "command-1") == 0);
    assert(strcmp(
        editor.entries[PSVR2_LINE_HISTORY_LIMIT - 1],
        "command-100") == 0);
    psvr2_line_editor_destroy(&editor);
}

void test_shellcode_safety_gate(void) {
    psvr2_constants constants;
    assert(psvr2_constants_set_firmware(
        &constants, PSVR2_FW_0600, false));
    constants.fw.injected_execution_certified = false;
    psvr2_exploit exploit = {.constants = &constants};
    psvr2_krw krw = {
        .ex = &exploit,
        .spinlock = 1,
        .regs = {.valid = true}
    };
    psvr2_runtime runtime;
    psvr2_runtime_init(&runtime, &krw);
    const uint32_t instruction = UINT32_C(0xd503201f);
    const psvr2_kernel_text_patch_request request = {
        .target = PSVR2_STR_SC,
        .data = &instruction,
        .length = sizeof(instruction)
    };

    assert(!psvr2_verify_shellcode(&runtime, false));
    assert(!psvr2_inject_str_shellcode(&runtime));
    assert(!psvr2_exec(&runtime, "true", false, NULL, NULL));
    assert(!psvr2_exec_blind(&runtime, "true"));
    assert(!psvr2_kernel_text_patch_many(&krw, NULL, 0));
    assert(!psvr2_kernel_text_patch_many(&krw, &request, 1));
    assert(!psvr2_krw_write_u64_fast(&krw, 1, 2));
    assert(!psvr2_krw_flush_tlb(&krw));
}
