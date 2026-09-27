#include "shell_internal.h"
#include "stage1_internal.h"

#include <stdio.h>
#include <string.h>

#define PSVR2_BUSYBOX_CWD_MARKER "\036PSVR2_CWD="

static void print_remote_output(const psvr2_buffer *output)
{
    if (output->len) {
        fwrite(output->data, 1, output->len, stdout);
        if (output->data[output->len - 1] != '\n')
            putchar('\n');
    }
}

static void print_remote_result(
    const psvr2_buffer *output, int64_t result)
{
    print_remote_output(output);
    printf("[exit %lld]\n", (long long)result);
}

static bool append_single_quoted(
    psvr2_buffer *out, const char *text)
{
    static const char quote[] = "'\\''";
    if (!psvr2_buffer_append(out, "'", 1))
        return false;
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor == '\'') {
            if (!psvr2_buffer_append(
                    out, quote, sizeof(quote) - 1))
                return false;
        } else if (!psvr2_buffer_append(out, cursor, 1)) {
            return false;
        }
    }
    return psvr2_buffer_append(out, "'", 1);
}

bool psvr2_shell_build_busybox_command(
    const char *path, const char *command, psvr2_buffer *out)
{
    static const char prefix[] = " ash -c '";
    static const char quote[] = "'\\''";
    const char terminator = '\0';

    if (!path || !*path || !command || !out ||
        !psvr2_buffer_append(out, path, strlen(path)) ||
        !psvr2_buffer_append(out, prefix, sizeof(prefix) - 1))
        return false;

    for (const char *cursor = command; *cursor; ++cursor) {
        if (*cursor == '\'') {
            if (!psvr2_buffer_append(out, quote, sizeof(quote) - 1))
                return false;
        } else if (!psvr2_buffer_append(out, cursor, 1)) {
            return false;
        }
    }

    return psvr2_buffer_append(out, "'", 1) &&
           psvr2_buffer_append(out, &terminator, 1);
}

bool psvr2_shell_build_busybox_mode_command(
    const char *path, const char *cwd,
    const char *command, psvr2_buffer *out)
{
    static const char status[] =
        "\n_psvr2_status=$?\n"
        "printf '\\036PSVR2_CWD=%s\\036' \"$PWD\"\n"
        "exit \"$_psvr2_status\"";
    const char terminator = '\0';
    psvr2_buffer payload = {0};
    bool ok =
        path && *path && cwd && cwd[0] == '/' &&
        command && *command && out &&
        psvr2_buffer_append(&payload, "cd ", 3) &&
        append_single_quoted(&payload, cwd) &&
        psvr2_buffer_append(
            &payload, " || exit 125\n", 13) &&
        psvr2_buffer_append(
            &payload, command, strlen(command)) &&
        psvr2_buffer_append(
            &payload, status, sizeof(status) - 1) &&
        psvr2_buffer_append(&payload, &terminator, 1) &&
        psvr2_shell_build_busybox_command(
            path, (char *)payload.data, out);
    psvr2_buffer_free(&payload);
    return ok;
}

bool psvr2_shell_busybox_available(psvr2_shell *shell)
{
    if (!shell || !shell->runtime)
        return false;
    if (shell->busybox_path[0])
        return true;

    static const char probe[] =
        "if [ -x /data/modules/busybox ]; then "
        "echo /data/modules/busybox; "
        "elif [ -x /tmp/busybox ]; then echo /tmp/busybox; "
        "else exit 127; fi";
    int64_t result = -1;
    psvr2_buffer output = {0};
    bool ok = psvr2_stage1_exec(
        shell->runtime, probe, 5.0, &result, &output);
    char *path = output.data
        ? psvr2_trim((char *)output.data) : NULL;
    ok = ok && result == 0 && path &&
         (!strcmp(path, "/data/modules/busybox") ||
          !strcmp(path, "/tmp/busybox"));
    if (ok)
        snprintf(
            shell->busybox_path, sizeof(shell->busybox_path),
            "%s", path);
    psvr2_buffer_free(&output);
    return ok;
}

int psvr2_shell_busybox_execute(
    psvr2_shell *shell, const char *command)
{
    if (!shell || !command || !*command ||
        !psvr2_shell_busybox_available(shell))
        return 1;

    psvr2_buffer output = {0};
    int64_t result = -1;
    int status = psvr2_shell_run_busybox_job(
        shell, "/", command, &output, &result);
    psvr2_shell_consume_busybox_cwd(NULL, &output);
    if (status == PSVR2_SHELL_JOB_COMPLETE)
        print_remote_result(&output, result);
    else if (status == PSVR2_SHELL_JOB_RUNNING)
        print_remote_output(&output);
    psvr2_buffer_free(&output);
    return status == PSVR2_SHELL_JOB_RUNNING ||
           (status == PSVR2_SHELL_JOB_COMPLETE && result == 0) ? 0 : 1;
}

void psvr2_shell_consume_busybox_cwd(
    psvr2_shell *shell, psvr2_buffer *output)
{
    static const char marker[] = PSVR2_BUSYBOX_CWD_MARKER;
    const size_t marker_length = sizeof(marker) - 1;
    if (!output->data || output->len <= marker_length + 1)
        return;

    size_t marker_offset = SIZE_MAX;
    for (size_t offset = 0;
         offset + marker_length < output->len; ++offset) {
        if (!memcmp(
                output->data + offset, marker, marker_length))
            marker_offset = offset;
    }
    if (marker_offset == SIZE_MAX)
        return;

    size_t cwd_start = marker_offset + marker_length;
    size_t cwd_end = cwd_start;
    while (cwd_end < output->len &&
           output->data[cwd_end] != '\036')
        ++cwd_end;
    size_t cwd_length = cwd_end - cwd_start;
    if (cwd_end == output->len || !cwd_length ||
        cwd_length >= sizeof(shell->busybox_cwd) ||
        output->data[cwd_start] != '/')
        return;
    for (size_t index = cwd_start; index < cwd_end; ++index) {
        unsigned char value = output->data[index];
        if (value < 0x20 || value == 0x7f)
            return;
    }

    if (shell) {
        memcpy(shell->busybox_cwd, output->data + cwd_start, cwd_length);
        shell->busybox_cwd[cwd_length] = '\0';
    }
    size_t tail = cwd_end + 1;
    memmove(
        output->data + marker_offset,
        output->data + tail, output->len - tail);
    output->len -= tail - marker_offset;
    output->data[output->len] = '\0';
}

int psvr2_shell_busybox_mode_execute(
    psvr2_shell *shell, const char *command)
{
    if (!shell || !command || !*command ||
        !psvr2_shell_busybox_available(shell))
        return 1;

    psvr2_buffer output = {0};
    int64_t result = -1;
    int status = psvr2_shell_run_busybox_job(
        shell, shell->busybox_cwd, command, &output, &result);
    if (status == PSVR2_SHELL_JOB_COMPLETE) {
        psvr2_shell_consume_busybox_cwd(shell, &output);
        print_remote_output(&output);
        if (result != 0)
            printf("[exit %lld]\n", (long long)result);
    } else if (status == PSVR2_SHELL_JOB_RUNNING) {
        psvr2_shell_consume_busybox_cwd(NULL, &output);
        print_remote_output(&output);
    }
    psvr2_buffer_free(&output);
    return status == PSVR2_SHELL_JOB_RUNNING ||
           (status == PSVR2_SHELL_JOB_COMPLETE && result == 0) ? 0 : 1;
}
