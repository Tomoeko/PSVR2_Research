#include "shell_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#endif

static bool list_cb(void *opaque, uint64_t dentry, const char *name,
                    psvr2_vfs_type type, uint64_t size) {
    (void)opaque;
    printf("%-5s %10llu  %016llx  %s\n", psvr2_vfs_type_name(type),
           (unsigned long long)size, (unsigned long long)dentry, name);
    return true;
}

static bool mount_cb(void *opaque, const char *name, uint64_t root,
                     uint64_t base) {
    (void)opaque;
    printf("%-20s root=%016llx mount=%016llx\n", name,
           (unsigned long long)root, (unsigned long long)base);
    return true;
}

static int cmd_ls(psvr2_shell *shell, int argc, char **argv) {
    uint64_t directory =
        argc > 1
            ? psvr2_vfs_resolve_path(
                  &shell->vfs, shell->cwd, argv[1])
            : shell->cwd;
    if (!directory) {
        puts("[-] Directory not found.");
        return 1;
    }
    return psvr2_vfs_list(
               &shell->vfs, directory, list_cb, NULL) ? 0 : 1;
}

static int cmd_cd(psvr2_shell *shell, int argc, char **argv) {
    if (argc != 2) {
        puts("Usage: cd <path|dentry>");
        return 1;
    }
    uint64_t value = 0;
    uint64_t directory =
        psvr2_parse_u64(argv[1], &value)
            ? value
            : psvr2_vfs_resolve_path(
                  &shell->vfs, shell->cwd, argv[1]);
    if (!directory) {
        puts("[-] Directory not found.");
        return 1;
    }
    psvr2_vfs_type type;
    uint64_t size;
    if (!psvr2_vfs_inode_info(
            &shell->vfs, directory, &type, &size) ||
        type != PSVR2_VFS_DIRECTORY) {
        puts("[-] Not a directory.");
        return 1;
    }
    shell->cwd = directory;
    if (argv[1][0] == '/') {
        snprintf(shell->cwd_name, sizeof(shell->cwd_name), "%s", argv[1]);
    } else if (!strcmp(argv[1], "..")) {
        char *slash = strrchr(shell->cwd_name, '/');
        if (slash && slash != shell->cwd_name) *slash = '\0';
        else strcpy(shell->cwd_name, "/");
    } else if (strcmp(argv[1], ".")) {
        size_t length = strlen(shell->cwd_name);
        snprintf(
            shell->cwd_name + length,
            sizeof(shell->cwd_name) - length, "%s%s",
            length > 1 ? "/" : "", argv[1]);
    }
    return 0;
}

static int cmd_cat(psvr2_shell *shell, int argc, char **argv) {
    if (argc != 2) {
        puts("Usage: cat <file>");
        return 1;
    }
    uint64_t file = psvr2_vfs_resolve_path(
        &shell->vfs, shell->cwd, argv[1]);
    psvr2_buffer data = {0};
    if (!file ||
        !psvr2_vfs_read_file(
            &shell->vfs, file, &data, 1024 * 1024)) {
        puts("[-] File is unavailable or evicted from page cache.");
        psvr2_buffer_free(&data);
        return 1;
    }
    fwrite(data.data, 1, data.len, stdout);
    if (!data.len || data.data[data.len - 1] != '\n') putchar('\n');
    psvr2_buffer_free(&data);
    return 0;
}

static int cmd_get(psvr2_shell *shell, int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        puts("Usage: get <remote-file> [local-file]\n"
             "       get <kernel-address> <size> [local-file]");
        return 1;
    }
    uint64_t address, size;
    psvr2_buffer data = {0};
    const char *local = NULL;
    if (psvr2_parse_u64(argv[1], &address)) {
        if (argc < 3 || !psvr2_parse_u64(argv[2], &size) ||
            size > SIZE_MAX)
            return 1;
        local = argc > 3 ? argv[3] : "memory.bin";
        if (!psvr2_buffer_reserve(&data, (size_t)size) ||
            !psvr2_krw_read(
                shell->runtime->krw, address, data.data, (size_t)size)) {
            psvr2_buffer_free(&data);
            return 1;
        }
        data.len = (size_t)size;
    } else {
        uint64_t file = psvr2_vfs_resolve_path(
            &shell->vfs, shell->cwd, argv[1]);
        local = argc > 2 ? argv[2] : psvr2_basename(argv[1]);
        if (!file ||
            !psvr2_vfs_read_file(
                &shell->vfs, file, &data, 128U * 1024U * 1024U)) {
            psvr2_buffer_free(&data);
            return 1;
        }
    }
    bool ok = psvr2_write_file(local, data.data, data.len);
    if (ok) printf("[+] Saved %zu bytes to %s\n", data.len, local);
    else printf("[-] Could not write %s\n", local);
    psvr2_buffer_free(&data);
    return ok ? 0 : 1;
}

static bool mkdir_if_needed(const char *path) {
#if defined(_WIN32)
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

static bool mkdir_tree(char *path) {
    for (char *cursor = path + 1; *cursor; ++cursor) {
        if (*cursor != '/' && *cursor != '\\') continue;
        char saved = *cursor;
        *cursor = '\0';
        if (!mkdir_if_needed(path)) {
            *cursor = saved;
            return false;
        }
        *cursor = saved;
    }
    return mkdir_if_needed(path);
}

typedef struct {
    psvr2_shell *shell;
    const char *local_dir;
    uint64_t *visited;
    size_t visited_count;
    size_t visited_cap;
    bool ok;
} recursive_download;

static bool download_directory(recursive_download *context,
                               uint64_t dentry, const char *local_dir);

static bool download_entry_cb(void *opaque, uint64_t dentry,
                              const char *name, psvr2_vfs_type type,
                              uint64_t size) {
    recursive_download *context = opaque;
    if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, "?"))
        return true;
    char path[2048];
    if (snprintf(path, sizeof(path), "%s/%s",
                 context->local_dir, name) >= (int)sizeof(path))
        return true;
    if (type == PSVR2_VFS_DIRECTORY) {
        recursive_download child = *context;
        child.local_dir = path;
        context->ok = download_directory(&child, dentry, path);
        context->visited_count = child.visited_count;
    } else if (type == PSVR2_VFS_FILE &&
               size <= 64U * 1024U * 1024U) {
        psvr2_buffer data = {0};
        if (psvr2_vfs_read_file(
                &context->shell->vfs, dentry, &data,
                64U * 1024U * 1024U)) {
            context->ok =
                psvr2_write_file(path, data.data, data.len);
            if (context->ok)
                printf("  %s (%zu bytes)\n", path, data.len);
        }
        psvr2_buffer_free(&data);
    }
    return context->ok;
}

static bool download_directory(recursive_download *context,
                               uint64_t dentry, const char *local_dir) {
    for (size_t i = 0; i < context->visited_count; ++i)
        if (context->visited[i] == dentry) return true;
    if (context->visited_count >= context->visited_cap ||
        !mkdir_if_needed(local_dir))
        return false;
    context->visited[context->visited_count++] = dentry;
    const char *previous = context->local_dir;
    context->local_dir = local_dir;
    bool ok = psvr2_vfs_list(
        &context->shell->vfs, dentry, download_entry_cb, context);
    context->local_dir = previous;
    return ok && context->ok;
}

static int cmd_get_all(psvr2_shell *shell, int argc, char **argv) {
    const char *target_argument =
        argc > 1 ? argv[1] : "psvr2_krw_c/downloads/VFS";
    char target[2048];
    snprintf(target, sizeof(target), "%s%s%s", target_argument,
             shell->cwd_name[0] == '/' ? "" : "/", shell->cwd_name);
    if (!mkdir_tree(target)) return 1;
    uint64_t visited[4096];
    recursive_download context = {
        .shell = shell,
        .local_dir = target,
        .visited = visited,
        .visited_cap = PSVR2_ARRAY_LEN(visited),
        .ok = true
    };
    printf("[*] Recursive VFS download -> %s\n", target);
    return download_directory(
               &context, shell->cwd, target) ? 0 : 1;
}

static int cmd_passthrough(psvr2_shell *shell, int argc, char **argv) {
    if (argc != 2 ||
        (strcmp(argv[1], "on") && strcmp(argv[1], "off")))
        return 1;
    uint8_t payload[2] = {0, 0};
    if (!strcmp(argv[1], "on")) {
        payload[0] = 1;
        payload[1] = 3;
    }
    return psvr2_device_vendor_set(
               shell->runtime->krw->ex->usb, 0x17, 1,
               payload, sizeof(payload), 1000) ? 0 : 1;
}

static int cmd_pwd(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("%s (0x%llx)\n", shell->cwd_name,
           (unsigned long long)shell->cwd);
    return 0;
}

static int cmd_mounts(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return psvr2_vfs_mounts(
               &shell->vfs, mount_cb, NULL) ? 0 : 1;
}

const psvr2_shell_command psvr2_shell_vfs_commands[] = {
    {"ls", cmd_ls, "ls [path] — list a kernel VFS directory", false},
    {"cd", cmd_cd, "cd <path|dentry> — change VFS directory", false},
    {"cat", cmd_cat, "cat <file> — read a page-cached file", false},
    {"get", cmd_get, "get <file>|<addr> <size> [local] — download", false},
    {"get_all", cmd_get_all,
     "get_all [local-dir] — recursive VFS download", false},
    {"passthrough", cmd_passthrough, "passthrough <on|off>", true},
    {"pwd", cmd_pwd, "Show current VFS path and dentry", false},
    {"mounts", cmd_mounts, "List kernel mount namespace", false}
};

const size_t psvr2_shell_vfs_command_count =
    PSVR2_ARRAY_LEN(psvr2_shell_vfs_commands);
