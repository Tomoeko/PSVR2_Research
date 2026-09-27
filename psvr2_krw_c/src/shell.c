#include "shell_internal.h"

#include "line_editor_internal.h"
#include "shell_completion_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

FILE *psvr2_shell_open_output_exclusive(const char *path) {
    FILE *file = fopen(path, "wbx");
    if (!file)
        fprintf(stderr, "[-] Refusing output %s: %s\n", path, strerror(errno));
    return file;
}

bool psvr2_shell_finalize_output_exclusive(const char *partial,
                                           const char *destination) {
    /* link(2) publishes atomically and refuses an existing destination. */
    if (link(partial, destination) != 0) {
        fprintf(stderr, "[-] Refusing to replace %s: %s\n", destination,
                strerror(errno));
        return false;
    }
    if (unlink(partial) == 0)
        return true;

    int saved = errno;
    (void)unlink(destination);
    errno = saved;
    fprintf(stderr, "[-] Could not remove partial name %s: %s\n", partial,
            strerror(errno));
    return false;
}

static int split_line(const char *line, char ***arguments_out) {
    size_t capacity = 8;
    size_t count = 0;
    char **arguments = calloc(capacity, sizeof(*arguments));
    if (!arguments) return -1;

    const char *cursor = line;
    while (*cursor) {
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (!*cursor) break;

        psvr2_buffer word = {0};
        char quote = 0;
        while (*cursor &&
               (quote || !isspace((unsigned char)*cursor))) {
            if (!quote && (*cursor == '\'' || *cursor == '"')) {
                quote = *cursor++;
                continue;
            }
            if (quote && *cursor == quote) {
                quote = 0;
                ++cursor;
                continue;
            }
            if (*cursor == '\\' && cursor[1]) ++cursor;
            if (!psvr2_buffer_append(&word, cursor++, 1)) goto fail;
        }
        const char terminator = '\0';
        if (!psvr2_buffer_append(&word, &terminator, 1)) goto fail;
        if (count + 1 >= capacity) {
            capacity *= 2;
            void *next =
                realloc(arguments, capacity * sizeof(*arguments));
            if (!next) {
                psvr2_buffer_free(&word);
                goto fail;
            }
            arguments = next;
        }
        arguments[count++] = (char *)word.data;
    }
    arguments[count] = NULL;
    *arguments_out = arguments;
    return (int)count;

fail:
    for (size_t index = 0; index < count; ++index)
        free(arguments[index]);
    free(arguments);
    return -1;
}

static void free_arguments(int count, char **arguments) {
    for (int index = 0; index < count; ++index)
        free(arguments[index]);
    free(arguments);
}

static const char *named_payload(
    const char *line, const char *name, bool *matched)
{
    while (isspace((unsigned char)*line)) ++line;
    size_t length = strlen(name);
    if (strncmp(line, name, length) ||
        (line[length] &&
         !isspace((unsigned char)line[length]))) {
        *matched = false;
        return NULL;
    }
    line += length;
    while (isspace((unsigned char)*line)) ++line;
    *matched = true;
    return line;
}

static const char *raw_stage1_payload(const char *line) {
    static const char * const commands[] = {"sh", "s1exec"};
    for (size_t index = 0;
         index < PSVR2_ARRAY_LEN(commands); ++index) {
        bool matched = false;
        const char *payload =
            named_payload(line, commands[index], &matched);
        if (matched) return payload;
    }
    return NULL;
}

static bool command_is_allowed(const psvr2_shell *shell,
                               const psvr2_shell_command *command) {
    if (!command->writes || !shell->read_only) return true;
    puts("[-] Command blocked in --read-only mode.");
    return false;
}

static int cmd_exit(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    shell->running = false;
    return 0;
}

static int cmd_help(psvr2_shell *shell, int argc, char **argv);

static const psvr2_shell_command core_commands[] = {
    {"help", cmd_help, "Show this command list", false},
    {"exit", cmd_exit, "Exit native shell", false},
    {"quit", cmd_exit, "Exit native shell", false},
    {"q", cmd_exit, "Exit native shell", false},
    {"jobs", psvr2_shell_jobs_command, "List retained BusyBox jobs", false},
    {"joblog", psvr2_shell_joblog_command, "joblog <id>: read a job's output", false},
    {"jobstop", psvr2_shell_jobstop_command, "jobstop <id>: signal a job's process group", true}
};

static void command_groups(psvr2_shell_command_group groups[7]) {
    groups[0] = (psvr2_shell_command_group){
        core_commands, PSVR2_ARRAY_LEN(core_commands)
    };
    groups[1] = (psvr2_shell_command_group){
        psvr2_shell_vfs_commands, psvr2_shell_vfs_command_count
    };
    groups[2] = (psvr2_shell_command_group){
        psvr2_shell_memory_commands, psvr2_shell_memory_command_count
    };
    groups[3] = (psvr2_shell_command_group){
        psvr2_shell_stage1_commands, psvr2_shell_stage1_command_count
    };
    groups[4] = (psvr2_shell_command_group){
        psvr2_shell_transfer_commands, psvr2_shell_transfer_command_count
    };
    groups[5] = (psvr2_shell_command_group){
        psvr2_shell_emmc_commands, psvr2_shell_emmc_command_count
    };
    groups[6] = (psvr2_shell_command_group){
        psvr2_shell_persistence_commands,
        psvr2_shell_persistence_command_count
    };
}

static bool append_command_completion(
    const char *name, const char *prefix, psvr2_line_completions *out) {
    if (strncmp(name, prefix, strlen(prefix))) return true;
    if (out->count >= SIZE_MAX / sizeof(*out->items) - 1) return false;
    char *copy = psvr2_strdup(name);
    if (!copy) return false;
    char **items = realloc(out->items, (out->count + 1) * sizeof(*items));
    if (!items) {
        free(copy);
        return false;
    }
    out->items = items;
    out->items[out->count++] = copy;
    return true;
}

bool psvr2_shell_complete_commands(
    psvr2_shell *shell, const char *prefix, psvr2_line_completions *out) {
    if (!shell || !prefix || !out) return false;
    psvr2_shell_command_group groups[7];
    command_groups(groups);
    for (size_t group = 0; group < PSVR2_ARRAY_LEN(groups); ++group) {
        for (size_t index = 0; index < groups[group].count; ++index) {
            const psvr2_shell_command *command = &groups[group].commands[index];
            if (shell->read_only && command->writes) continue;
            if (!append_command_completion(command->name, prefix, out))
                return false;
        }
    }
    static const char * const aliases[] = {"krw", "bb", "ash", "busybox"};
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(aliases); ++index) {
        if (index && shell->read_only) continue;
        if (!append_command_completion(aliases[index], prefix, out))
            return false;
    }
    return true;
}

static int cmd_help(psvr2_shell *shell, int argc, char **argv) {
    (void)shell;
    (void)argc;
    (void)argv;
    psvr2_shell_command_group groups[7];
    command_groups(groups);
    for (size_t group_index = 0;
         group_index < PSVR2_ARRAY_LEN(groups); ++group_index) {
        for (size_t command_index = 0;
             command_index < groups[group_index].count;
             ++command_index) {
            const psvr2_shell_command *command =
                &groups[group_index].commands[command_index];
            printf("  %-15s %s\n", command->name, command->help);
        }
    }
    if (!shell->read_only)
        puts("\n  Other command lines are executed by BusyBox ash when "
             "/data/modules/busybox or /tmp/busybox is executable.\n"
             "  busybox | ash   Enter persistent BusyBox command mode\n"
             "  bb <command>   Run one command through BusyBox ash\n"
             "  krw <command>  Force a native toolkit command");
    return 0;
}

void psvr2_shell_init(psvr2_shell *shell, psvr2_runtime *runtime,
                      bool read_only, bool force_tmp, bool noevict,
                      bool double_evict) {
    memset(shell, 0, sizeof(*shell));
    shell->runtime = runtime;
    shell->read_only = read_only;
    shell->force_tmp = force_tmp;
    shell->noevict = noevict;
    shell->double_evict = double_evict;
    shell->bulk_interface = 5;
    shell->running = true;
    strcpy(shell->cwd_name, "/");
    strcpy(shell->busybox_cwd, "/");
    psvr2_vfs_init(&shell->vfs, runtime->krw);
    shell->cwd = psvr2_vfs_root(&shell->vfs);
}

void psvr2_shell_destroy(psvr2_shell *shell) {
    if (!shell) return;
    psvr2_shell_jobs_destroy(shell);
    if (!shell->bulk_claimed) return;
    psvr2_device *device = shell->runtime->krw->ex->usb;
    if (device && device->handle &&
        shell->bulk_generation == device->connection_generation)
        (void)libusb_release_interface(
            device->handle, shell->bulk_interface);
    shell->bulk_claimed = false;
}

static int enter_busybox_mode(psvr2_shell *shell) {
    if (shell->read_only) {
        puts("[-] Command blocked in --read-only mode.");
        return 1;
    }
    if (!psvr2_shell_busybox_available(shell)) {
        puts("[-] BusyBox is unavailable.");
        return 1;
    }
    if (!shell->busybox_cwd[0])
        strcpy(shell->busybox_cwd, "/");
    shell->busybox_mode = true;
    shell->busybox_announced = true;
    printf("[+] BusyBox command mode: %s ash\n"
           "    Use 'exit' for the native toolkit; prefix native "
           "commands with 'krw'.\n"
           "    Bare 'reboot', 'shutdown', and 'poweroff' use immediate "
           "kernel paths.\n",
           shell->busybox_path);
    return 0;
}

int psvr2_shell_execute(psvr2_shell *shell, const char *line) {
    if (!shell || !line)
        return 1;
    const char *content = line;
    while (isspace((unsigned char)*content)) ++content;
    if (!*content)
        return 0;

    bool matched = false;
    const char *payload = NULL;
    if (shell->busybox_mode) {
        payload = named_payload(line, "quit", &matched);
        if (!matched)
            payload = named_payload(line, "q", &matched);
        if (matched && !*payload) {
            shell->running = false;
            return 0;
        }

        payload = named_payload(line, "exit", &matched);
        if (matched && !*payload) {
            shell->busybox_mode = false;
            puts("[+] Returned to the native toolkit shell.");
            return 0;
        }

        payload = named_payload(line, "krw", &matched);
        if (matched) {
            shell->busybox_mode = false;
            int result = psvr2_shell_execute(
                shell, *payload ? payload : "help");
            if (shell->running)
                shell->busybox_mode = true;
            return result;
        }

        /*
         * BusyBox's plain reboot applet signals PID 1 and expects a
         * conventional init system.  This headset does not provide one that
         * reliably completes that request.  Keep the exact bare command on
         * the native control plane so it uses the verified
         * emergency_restart() path; commands with arguments retain normal
         * BusyBox semantics.
         */
        payload = named_payload(line, "reboot", &matched);
        if (matched && !*payload) {
            shell->busybox_mode = false;
            return psvr2_shell_execute(shell, "reboot");
        }

        /*
         * BusyBox power-control applets have the same PID 1 dependency.
         * Route exact bare shutdown/poweroff commands to the architecture's
         * verified machine_power_off() path.
         */
        payload = named_payload(line, "shutdown", &matched);
        if (!matched)
            payload = named_payload(line, "poweroff", &matched);
        if (matched && !*payload) {
            shell->busybox_mode = false;
            return psvr2_shell_execute(shell, "shutdown");
        }

        if (shell->read_only) {
            puts("[-] Command blocked in --read-only mode.");
            return 1;
        }
        return psvr2_shell_busybox_mode_execute(shell, line);
    }

    payload = named_payload(line, "krw", &matched);
    if (matched)
        return psvr2_shell_execute(
            shell, *payload ? payload : "help");

    payload = named_payload(line, "bb", &matched);
    if (matched && *payload) {
        if (shell->read_only) {
            puts("[-] Command blocked in --read-only mode.");
            return 1;
        }
        return psvr2_shell_busybox_execute(shell, payload);
    }

    bool enter_busybox = matched && !*payload;
    if (!enter_busybox) {
        payload = named_payload(line, "busybox", &matched);
        enter_busybox = matched && !*payload;
    }
    if (!enter_busybox) {
        payload = named_payload(line, "ash", &matched);
        enter_busybox = matched && !*payload;
    }
    if (enter_busybox) {
        return enter_busybox_mode(shell);
    }

    const char *stage1_payload = raw_stage1_payload(line);
    if (stage1_payload) {
        if (shell->read_only) {
            puts("[-] Command blocked in --read-only mode.");
            return 1;
        }
        return psvr2_shell_stage1_execute_line(
            shell, stage1_payload);
    }

    char **arguments = NULL;
    int count = split_line(line, &arguments);
    if (count <= 0) {
        if (count == 0) free(arguments);
        return count < 0 ? 1 : 0;
    }

    psvr2_shell_command_group groups[7];
    command_groups(groups);
    for (size_t group_index = 0;
         group_index < PSVR2_ARRAY_LEN(groups); ++group_index) {
        for (size_t command_index = 0;
             command_index < groups[group_index].count;
             ++command_index) {
            const psvr2_shell_command *command =
                &groups[group_index].commands[command_index];
            if (strcmp(arguments[0], command->name)) continue;
            int result = command_is_allowed(shell, command)
                ? command->function(shell, count, arguments)
                : 1;
            free_arguments(count, arguments);
            return result;
        }
    }
    char unknown[128];
    snprintf(unknown, sizeof(unknown), "%s", arguments[0]);
    free_arguments(count, arguments);
    if (!shell->read_only &&
        psvr2_shell_busybox_available(shell)) {
        if (!shell->busybox_announced) {
            printf("[+] Remote shell: %s ash\n",
                   shell->busybox_path);
            shell->busybox_announced = true;
        }
        return psvr2_shell_busybox_execute(shell, line);
    }
    fprintf(stderr, "[-] Unknown command: %s (try help)\n",
            unknown);
    return 1;
}

int psvr2_shell_run(psvr2_shell *shell) {
    puts("\n========================================\n"
         "  PSVR2 Kernel Shell — native C\n"
         "========================================\n"
         "Type 'help' for commands.");
    if (!shell->read_only)
        (void)enter_busybox_mode(shell);
    psvr2_line_editor editor;
    psvr2_line_editor_init(&editor);
    editor.complete = psvr2_shell_complete;
    editor.complete_context = shell;
    while (shell->running) {
        char prompt[1100];
        snprintf(
            prompt, sizeof(prompt), "%s%s:%s # ",
            shell->busybox_mode ? "psvr2-busybox" : "psvr2",
            shell->read_only ? " [RO]" : "",
            shell->busybox_mode
                ? shell->busybox_cwd : shell->cwd_name);
        char *line = NULL;
        if (!psvr2_line_editor_read(&editor, prompt, &line)) break;
        (void)psvr2_shell_execute(shell, line);
        free(line);
    }
    psvr2_line_editor_destroy(&editor);
    return 0;
}
