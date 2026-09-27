/* Run the completion protocol against local fixtures, never USB/hardware. */
#include "shell_completion_internal.h"
#include "shell_internal.h"
#include "stage1_internal.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static size_t mailbox_calls, vfs_calls, maximum_command;
static bool malformed_response, failed_response;
static const char *root_path;

bool psvr2_shell_complete_commands(psvr2_shell *shell, const char *prefix,
                                   psvr2_line_completions *out)
{
    static const char *names[] = {
        "cat", "cd", "get", "get_all", "upload", "fast_upload", "fast_download",
        "stage1", "stage1_install", "stage1_verify", "persist", "unpersist",
        "persist_backups", "emmc", "emmc_plain", "dumpmem", "dumpuser", "chmod",
        "krw", "bb", "sh", "s1exec", "exec", "modlist"
    };
    (void)shell;
    for (size_t i = 0; i < PSVR2_ARRAY_LEN(names); ++i) {
        if (strncmp(names[i], prefix, strlen(prefix))) continue;
        char **next = realloc(out->items, (out->count + 1) * sizeof(*next));
        assert(next);
        out->items = next;
        out->items[out->count++] = psvr2_strdup(names[i]);
    }
    return true;
}

bool psvr2_shell_busybox_available(psvr2_shell *shell)
{
    strcpy(shell->busybox_path, "/tmp/busybox");
    return true;
}

bool psvr2_shell_build_busybox_command(const char *path, const char *script,
                                      psvr2_buffer *out)
{
    assert(psvr2_buffer_append(out, path, strlen(path)));
    assert(psvr2_buffer_append(out, " ash -c '", 9));
    for (const char *p = script; *p; ++p)
        assert(psvr2_buffer_append(out, *p == '\'' ? "'\\''" : p, *p == '\'' ? 4 : 1));
    assert(psvr2_buffer_append(out, "'", 2));
    return true;
}

bool psvr2_stage1_exec(psvr2_runtime *runtime, const char *command, double timeout,
                       int64_t *result, psvr2_buffer *output)
{
    (void)runtime;
    assert(timeout <= 1.0);
    size_t size = strlen(command) + strlen(PSVR2_STAGE1_COMMAND_PREFIX);
    assert(size < PSVR2_STAGE1_COMMAND_CAPACITY);
    if (size > maximum_command) maximum_command = size;
    ++mailbox_calls;
    if (failed_response) return false;
    if (malformed_response) {
        assert(psvr2_buffer_append(output, "x\0unterminated", 14));
        *result = 0;
        return true;
    }
    if (strstr(command, " --list")) {
        static const char applets[] = "cat\nchmod\necho\nls\npwd\nsh\nsleep\n";
        assert(psvr2_buffer_append(output, applets, sizeof(applets) - 1));
        *result = 0;
        return true;
    }
    const char *payload = strstr(command, " ash -c ");
    assert(payload);
    payload += strlen(" ash -c ");
    psvr2_buffer local = {0};
    assert(psvr2_buffer_append(&local, "/bin/sh -c ", 11));
    assert(psvr2_buffer_append(&local, payload, strlen(payload) + 1));
    int pipes[2];
    assert(pipe(pipes) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        assert(dup2(pipes[1], STDOUT_FILENO) >= 0);
        close(pipes[0]);
        close(pipes[1]);
        execl("/bin/sh", "sh", "-c", (char *)local.data, (char *)NULL);
        _exit(127);
    }
    psvr2_buffer_free(&local);
    close(pipes[1]);
    char bytes[4096];
    ssize_t count;
    while ((count = read(pipes[0], bytes, sizeof(bytes))) != 0) {
        if (count < 0) { assert(errno == EINTR); continue; }
        assert(psvr2_buffer_append(output, bytes, (size_t)count));
    }
    close(pipes[0]);
    int status;
    while (waitpid(child, &status, 0) < 0) assert(errno == EINTR);
    *result = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    assert(*result == 0); /* Exercises the real script's terminal exit status. */
    return true;
}

uint64_t psvr2_vfs_resolve_path(psvr2_vfs *vfs, uint64_t base, const char *path)
{
    (void)vfs;
    assert(base == 42);
    assert(!strcmp(path, ".") || !strcmp(path, "./"));
    ++vfs_calls;
    return 42;
}

psvr2_walk_result psvr2_vfs_list_ex(psvr2_vfs *vfs, uint64_t parent,
                                   psvr2_vfs_entry_fn callback, void *opaque)
{
    (void)vfs;
    assert(parent == 42);
    ++vfs_calls;
    assert(callback(opaque, 43, "cached-file", PSVR2_VFS_FILE, 42));
    assert(callback(opaque, 44, "cached-dir", PSVR2_VFS_DIRECTORY, 0));
    assert(callback(opaque, 45, "bad\033name", PSVR2_VFS_FILE, 0));
    return PSVR2_WALK_COMPLETE;
}

static void destroy(psvr2_line_completions *out)
{
    for (size_t i = 0; i < out->count; ++i) free(out->items[i]);
    free(out->items);
    memset(out, 0, sizeof(*out));
}

static void expect(psvr2_shell *shell, const char *line, size_t cursor,
                   const char *first, size_t count, size_t start, size_t end)
{
    psvr2_line_completions out = {0};
    bool ok = psvr2_shell_complete(shell, line, cursor, &out);
    if (!ok || out.count != count || out.start != start || out.end != end ||
        (first && (!out.count || strcmp(out.items[0], first)))) {
        fprintf(stderr, "completion mismatch: %s => ok=%d count=%zu range=%zu..%zu\n",
                line, ok, out.count, out.start, out.end);
        for (size_t i = 0; i < out.count; ++i) fprintf(stderr, "  [%s]\n", out.items[i]);
        abort();
    }
    destroy(&out);
}

static void fixture(const char *name, bool directory, bool executable)
{
    if (directory) assert(mkdir(name, 0700) == 0);
    else {
        FILE *file = fopen(name, "w");
        assert(file);
        assert(fclose(file) == 0);
        if (executable) assert(chmod(name, 0700) == 0);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    root_path = argv[1];
    assert(chdir(root_path) == 0);
    fixture("open_vrhmd", false, true);
    fixture("opaque", false, false);
    fixture("folder", true, false);
    fixture("multi-a", false, false);
    fixture("multi-b", false, false);
    fixture("space name", false, false);
    fixture("apostrophe'name", false, false);
    fixture("literal$(sentinel)", true, false);
    fixture("literal$(sentinel)/open_vrhmd", false, true);
    fixture(".hidden", false, false);
    fixture("bad\033name", false, false);
    fixture("host-module.ko", false, false);
    fixture("tail-module.ko", false, false);
    fixture("a\\b", false, false);
    fixture("ab", false, false);
    psvr2_runtime runtime = {0};
    psvr2_shell shell = {.runtime = &runtime, .cwd = 42, .busybox_mode = true};
    strcpy(shell.busybox_path, "/tmp/busybox");
    assert(strlen(root_path) < sizeof(shell.busybox_cwd));
    strcpy(shell.busybox_cwd, root_path);
    strcpy(shell.cwd_name, root_path);
    shell.vfs.krw = (psvr2_krw *)(uintptr_t)1;

    expect(&shell, "./open", 6, "./open_vrhmd ", 1, 0, 6);
    expect(&shell, "cat ./ope", 9, "./open_vrhmd ", 1, 4, 9);
    expect(&shell, "cat ./missing", 13, NULL, 0, 4, 13);
    expect(&shell, "cat ./multi", 11, "./multi-a", 2, 4, 11);
    expect(&shell, "cd ./fol", 8, "./folder/", 1, 3, 8);
    expect(&shell, "cd ./ope", 8, NULL, 0, 3, 8);
    expect(&shell, "cat ./spa", 9, "./space\\ name ", 1, 4, 9);
    expect(&shell, "cat './apo", 10, "./apostrophe\\'name ", 1, 4, 10);
    expect(&shell, "cat \"./a\\b\"", 11, "./a\\\\b ", 1, 4, 11);
    expect(&shell, "cat ./ope --tail", 9, "./open_vrhmd", 1, 4, 9);
    expect(&shell, "cat ./openTAIL --tail", 10, "./open_vrhmd", 1, 4, 14);
    expect(&shell, "echo x; ./open", 14, "./open_vrhmd ", 1, 8, 14);
    expect(&shell, "cat > ./spa", 11, "./space\\ name ", 1, 6, 11);
    expect(&shell, "cat './literal$(sentinel)/ope", 29,
           "./literal\\$\\(sentinel\\)/open_vrhmd ", 1, 4, 29);
    assert(access("sentinel", F_OK) != 0);
    size_t calls = mailbox_calls;
    psvr2_line_completions out = {0};
    assert(!psvr2_shell_complete(&shell, "cat $(touch sentinel)/op", 24, &out));
    assert(mailbox_calls == calls && !out.count && access("sentinel", F_OK) != 0);
    expect(&shell, "cat ./.hi", 9, "./.hidden ", 1, 4, 9);
    expect(&shell, "./opa", 5, NULL, 0, 0, 5); /* Not executable. */
    malformed_response = true;
    assert(!psvr2_shell_complete(&shell, "./open", 6, &out) && !out.count);
    malformed_response = false;

    shell.busybox_mode = false;
    expect(&shell, "upload host-m", 13, "host-module.ko ", 1, 7, 13);
    expect(&shell, "krw fast_upload host-module.ko tail-m", 37,
           "tail-module.ko ", 1, 31, 37);
    expect(&shell, "stage1_install host-m", 21, "host-module.ko ", 1, 15, 21);
    expect(&shell, "fast_download /tmp/source host-m", 32,
           "host-module.ko ", 1, 26, 32);
    expect(&shell, "get /tmp/source host-m", 22, "host-module.ko ", 1, 16, 22);
    expect(&shell, "get 0xffff 64 host-m", 20, "host-module.ko ", 1, 14, 20);
    expect(&shell, "get \"./a\\b\"", 11, "./ab ", 1, 4, 11);
    expect(&shell, "cd ./fol", 8, "./folder/", 1, 3, 8);
    assert(vfs_calls == 0); /* Active queries see actual files, not cached dentries. */
    calls = mailbox_calls;
    shell.read_only = true;
    expect(&shell, "cat cached-f", 12, "cached-file ", 1, 4, 12);
    expect(&shell, "cd cached-d", 11, "cached-dir/", 1, 3, 11);
    assert(mailbox_calls == calls && vfs_calls == 4);
    shell.read_only = false;
    shell.busybox_mode = true;

    /* Actual PATH enumeration: unrelated executable count must not cap prefix matches. */
    assert(mkdir("path", 0700) == 0);
    for (unsigned i = 0; i < 140; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "path/aaa-%03u", i);
        fixture(name, false, true);
    }
    fixture("path/open_vrhmd", false, true);
    fixture("path/!tool", false, true);
    char path[4096];
    snprintf(path, sizeof(path), "%s/path", root_path);
    assert(setenv("PATH", path, 1) == 0);
    expect(&shell, "open", 4, "open_vrhmd ", 1, 0, 4);
    expect(&shell, "ech", 3, "echo ", 1, 0, 3); /* Applet + builtin deduplicated. */
    expect(&shell, "!to", 3, "\\!tool ", 1, 0, 3);
    assert(psvr2_shell_complete(&shell, "", 0, &out) && out.count > 128);
    destroy(&out);

    /* A bounded/truncated directory response must never imply a sole complete match. */
    assert(mkdir("many", 0700) == 0);
    for (unsigned i = 0; i < 129; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "many/match-%03u", i);
        fixture(name, false, false);
    }
    assert(!psvr2_shell_complete(&shell, "cat ./many/match", 16, &out) && !out.count);
    failed_response = true;
    assert(!psvr2_shell_complete(&shell, "./open", 6, &out) && !out.count);
    failed_response = false;
    printf("completion protocol passed; %zu queries, max command %zu bytes\n",
           mailbox_calls, maximum_command);
    return 0;
}
