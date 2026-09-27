/* Exercise the real persistence transaction against an in-memory Stage1 peer. */
#define psvr2_file_exists mock_local_file_exists
#define psvr2_sha256_file mock_local_sha256_file
#define psvr2_firmware_name mock_firmware_name
#include "../src/shell_persistence.c"
#undef psvr2_file_exists
#undef psvr2_sha256_file
#undef psvr2_firmware_name

#include <assert.h>

typedef struct {
    char path[400];
    char hash[65];
    bool directory;
} mock_file;

static mock_file files[32];
static size_t file_count, trace_count, fsync_count, fail_fsync;
static char trace[256][PERSIST_COMMAND_SIZE];
static bool mounted, mounted_ro, fail_unmount, fail_listing, fail_sync;
static const char *applet_list;
static char old_hash[65], new_hash[65], busybox_hash[65];

static mock_file *lookup(const char *path) {
    for (size_t index = 0; index < file_count; ++index)
        if (!strcmp(files[index].path, path)) return &files[index];
    return NULL;
}

static void put_file(const char *path, const char *hash, bool directory) {
    mock_file *file = lookup(path);
    if (!file) {
        assert(file_count < PSVR2_ARRAY_LEN(files));
        file = &files[file_count++];
    }
    snprintf(file->path, sizeof(file->path), "%s", path);
    snprintf(file->hash, sizeof(file->hash), "%s", hash ? hash : "");
    file->directory = directory;
}

static void remove_file(const char *path) {
    mock_file *file = lookup(path);
    assert(file);
    *file = files[--file_count];
}

static const char *quoted_path(const char *text, char path[400]) {
    const char *start = strchr(text, '\'');
    assert(start);
    const char *end = strchr(start + 1, '\'');
    assert(end && (size_t)(end - start - 1) < 400);
    size_t length = (size_t)(end - start - 1);
    memcpy(path, start + 1, length);
    path[length] = '\0';
    return end + 1;
}

static void set_output(psvr2_buffer *output, const char *text) {
    assert(output);
    assert(psvr2_buffer_append(output, text, strlen(text)));
}

bool psvr2_stage1_discover(psvr2_runtime *runtime) {
    assert(runtime);
    return true;
}

bool psvr2_stage1_exec(psvr2_runtime *runtime, const char *command,
    double timeout, int64_t *result, psvr2_buffer *output) {
    assert(runtime && timeout == 30.0);
    assert(trace_count < PSVR2_ARRAY_LEN(trace));
    snprintf(trace[trace_count++], sizeof(trace[0]), "%s", command);
    *result = 0;
    char first[400], second[400];
    if (strstr(command, "if [ -x /tmp/busybox ]")) {
        set_output(output, "/tmp/busybox\n");
    } else if (strstr(command, " --list")) {
        set_output(output, applet_list);
        if (fail_listing) *result = 1;
    } else if (strstr(command, " sha256sum ")) {
        quoted_path(command, first);
        mock_file *file = lookup(first);
        if (!file || file->directory) *result = 1;
        else {
            set_output(output, file->hash);
            set_output(output, "  file\n");
        }
    } else if (strstr(command, " blockdev --getsize64 ")) {
        set_output(output, "67108864\n");
    } else if (strstr(command, " cat /proc/mounts")) {
        if (!mounted) set_output(output, "tmpfs /tmp tmpfs rw 0 0\n");
        else set_output(output, mounted_ro
            ? PERSIST_DEVICE " " PERSIST_MOUNT " vfat ro,nosuid,nodev,noexec 0 0\n"
            : PERSIST_DEVICE " " PERSIST_MOUNT " vfat rw,nosuid,nodev,noexec 0 0\n");
    } else if (!strncmp(command, "p='", 3)) {
        quoted_path(command, first);
        mock_file *file = lookup(first);
        set_output(output, !file ? "0\n" : file->directory ? "X\n" : "1\n");
    } else if (strstr(command, "then exit 18; fi;")) {
        assert(!mounted && !lookup(PERSIST_MOUNT));
        put_file(PERSIST_MOUNT, NULL, true);
    } else if (strstr(command, " mount -t vfat ")) {
        assert(!mounted && lookup(PERSIST_MOUNT));
        mounted = true;
        mounted_ro = strstr(command, "-o ro,") != NULL;
    } else if (strstr(command, " test -f " PERSIST_MOUNT "/pcbid")) {
        assert(mounted);
    } else if (!strncmp(command, "if [ -e '", 9)) {
        quoted_path(command, first);
        mock_file *file = lookup(first);
        if (file && !file->directory) *result = 1;
        else if (!file) put_file(first, NULL, true);
    } else if (strstr(command, " cp ")) {
        assert(mounted && !mounted_ro);
        const char *next = quoted_path(command, first);
        quoted_path(next, second);
        mock_file *file = lookup(first);
        assert(file && !file->directory);
        put_file(second, file->hash, false);
    } else if (strstr(command, " mv ")) {
        assert(mounted && !mounted_ro);
        const char *next = quoted_path(command, first);
        quoted_path(next, second);
        mock_file *file = lookup(first);
        assert(file && !file->directory);
        char hash[65];
        snprintf(hash, sizeof(hash), "%s", file->hash);
        remove_file(first);
        put_file(second, hash, false);
    } else if (strstr(command, " fsync ")) {
        assert(mounted && !mounted_ro);
        quoted_path(command, first);
        assert(lookup(first));
        if (++fsync_count == fail_fsync) *result = 1;
    } else if (!strcmp(command, "/tmp/busybox sync")) {
        if (fail_sync) *result = 1;
    } else if (!strcmp(command, "/tmp/busybox umount " PERSIST_MOUNT)) {
        assert(mounted);
        if (fail_unmount) *result = 1;
        else mounted = false;
    } else if (!strcmp(command, "/tmp/busybox rmdir " PERSIST_MOUNT)) {
        assert(!mounted);
        remove_file(PERSIST_MOUNT);
    } else {
        fprintf(stderr, "Unexpected mock command: %s\n", command);
        assert(0);
    }
    return true;
}

bool mock_local_file_exists(const char *path) {
    return !strcmp(path, "/mock/external/busybox") ||
        strstr(path, "/tools/busybox") != NULL;
}

bool mock_local_sha256_file(const char *path, uint8_t digest[32], uint64_t *size) {
    assert(mock_local_file_exists(path));
    memset(digest, 0xdd, 32);
    *size = 4096;
    return true;
}

const char *mock_firmware_name(uint32_t version) {
    assert(version == PSVR2_FW_0600);
    return "06.00";
}

int psvr2_shell_fast_upload(psvr2_shell *shell, int argc, char **argv) {
    (void)shell; (void)argc; (void)argv;
    assert(0 && "No upload is allowed in this harness");
    return 1;
}

bool psvr2_load_stage1(psvr2_runtime *runtime, const char *path,
    bool force, bool temporary) {
    (void)runtime; (void)path; (void)force; (void)temporary;
    assert(0 && "No module loading is allowed in this harness");
    return false;
}

static void reset_case(bool existing, persistence_plan *plan) {
    file_count = trace_count = fsync_count = fail_fsync = 0;
    mounted = mounted_ro = fail_unmount = fail_listing = fail_sync = false;
    applet_list = "ash\ncp\nfsync\nmv\nsync\n";
    memset(old_hash, 'a', 64); old_hash[64] = '\0';
    memset(new_hash, 'b', 64); new_hash[64] = '\0';
    memset(busybox_hash, 'd', 64); busybox_hash[64] = '\0';
    put_file("/tmp/busybox", busybox_hash, false);
    put_file("/tmp/stage1.ko", new_hash, false);
    put_file(PERSIST_MODULES, NULL, true);
    if (existing) put_file(PERSIST_MODULES "/stage1.ko", old_hash, false);
    *plan = (persistence_plan){.destination_exists = existing};
    snprintf(plan->name, sizeof(plan->name), "stage1.ko");
    snprintf(plan->source, sizeof(plan->source), "/tmp/stage1.ko");
    snprintf(plan->candidate_hash, sizeof(plan->candidate_hash), "%s", new_hash);
    snprintf(plan->destination_hash, sizeof(plan->destination_hash), "%s", old_hash);
    memset(plan->transaction_hash, 'c', 64); plan->transaction_hash[64] = '\0';
}

static void assert_hash(const char *path, const char *hash) {
    mock_file *file = lookup(path);
    assert(file && !strcmp(file->hash, hash));
}

static void assert_flush_sequence(size_t index, const char *path) {
    const char *paths[] = {path, PERSIST_BACKUPS, PERSIST_MODULES, PERSIST_MOUNT};
    for (size_t offset = 0; offset < PSVR2_ARRAY_LEN(paths); ++offset) {
        char expected[PERSIST_COMMAND_SIZE];
        snprintf(expected, sizeof(expected), "/tmp/busybox fsync '%s'", paths[offset]);
        assert(index + offset < trace_count);
        assert(!strcmp(trace[index + offset], expected));
    }
}

static void assert_all_renames_flushed(void) {
    for (size_t index = 0; index < trace_count; ++index) {
        if (!strstr(trace[index], " mv ")) continue;
        char source[400], destination[400];
        const char *next = quoted_path(trace[index], source);
        quoted_path(next, destination);
        assert(index >= 4);
        assert_flush_sequence(index - 4, source);
        assert_flush_sequence(index + 1, destination);
    }
}

static void test_rollback_flushes(psvr2_shell *shell, persistence_plan *plan,
    bool existing) {
    for (size_t failure = 0; failure <= 8; ++failure) {
        reset_case(existing, plan);
        mounted = true;
        put_file(PERSIST_MOUNT, NULL, true);
        put_file(PERSIST_BACKUPS, NULL, true);
        put_file(PERSIST_MODULES "/stage1.ko", new_hash, false);
        char recovery[384];
        snprintf(recovery, sizeof(recovery),
            PERSIST_BACKUPS "/stage1.ko.%s.bak", old_hash);
        if (existing) put_file(recovery, old_hash, false);
        persistence_session session = {.shell = shell, .mounted = true};
        snprintf(session.busybox, sizeof(session.busybox), "/tmp/busybox");
        fail_fsync = failure;
        bool ok = restore_after_failed_install(&session, plan,
            PERSIST_MODULES "/stage1.ko");
        assert(ok == (failure == 0));
        if (failure == 0) assert_all_renames_flushed();
        if (failure > 0 && failure <= 4)
            assert_hash(PERSIST_MODULES "/stage1.ko", new_hash);
        else if (existing)
            assert_hash(PERSIST_MODULES "/stage1.ko", old_hash);
        else {
            assert(!lookup(PERSIST_MODULES "/stage1.ko"));
            snprintf(recovery, sizeof(recovery),
                PERSIST_BACKUPS "/stage1.ko.%s.failed", plan->transaction_hash);
            assert_hash(recovery, new_hash);
        }
        assert(close_session(&session));
    }
}

static void test_unpersist_flushes(psvr2_shell *shell, persistence_plan *plan) {
    for (size_t failure = 0; failure <= 8; ++failure) {
        reset_case(true, plan);
        snprintf(plan->source, sizeof(plan->source), PERSIST_MODULES "/stage1.ko");
        snprintf(plan->candidate_hash, sizeof(plan->candidate_hash), "%s", old_hash);
        char confirmation[65];
        assert(calculate_transaction_hash("unpersist", plan, confirmation));
        char *arguments[] = {"unpersist", "stage1.ko", "--confirm", confirmation};
        fail_fsync = failure;
        int result = cmd_unpersist(shell, 4, arguments);
        assert(result == (failure == 0 ? 0 : 1));
        assert(!mounted);
        if (failure == 0) {
            assert(fsync_count == 8);
            assert(!lookup(PERSIST_MODULES "/stage1.ko"));
            char disabled[384];
            snprintf(disabled, sizeof(disabled),
                PERSIST_BACKUPS "/stage1.ko.%s.disabled", old_hash);
            assert_hash(disabled, old_hash);
            assert_all_renames_flushed();
        } else {
            assert_hash(PERSIST_MODULES "/stage1.ko", old_hash);
        }
    }
}

static void test_busybox_override(psvr2_shell *shell,
    psvr2_constants *constants, persistence_plan *plan) {
    persistence_session session = {.shell = shell};
    assert(setenv("PSVR2_BUSYBOX", "/mock/external/busybox", 1) == 0);
    assert(!strcmp(local_busybox(&session), "/mock/external/busybox"));

    /* A valid executable override cannot bypass either firmware guard. */
    reset_case(true, plan);
    constants->firmware_forced = true;
    assert(!open_session(&session, shell, false, false));
    assert(!trace_count && !mounted && !fsync_count);
    assert_hash(PERSIST_MODULES "/stage1.ko", old_hash);
    constants->firmware_forced = false;
    constants->version = UINT32_C(0x07000000);
    assert(!open_session(&session, shell, false, false));
    assert(!trace_count && !mounted && !fsync_count);
    constants->version = PSVR2_FW_0600;

    /* A supported session uses the supplied host path for hash validation. */
    assert(install_plan(shell, plan));
    assert_hash(PERSIST_MODULES "/stage1.ko", new_hash);
    assert(!mounted);

    /* An explicit missing input fails rather than using the default output. */
    assert(setenv("PSVR2_BUSYBOX", "/mock/missing/busybox", 1) == 0);
    assert(!local_busybox(&session));
    reset_case(true, plan);
    assert(!open_session(&session, shell, false, false));
    assert(!trace_count && !mounted && !fsync_count);
    assert_hash(PERSIST_MODULES "/stage1.ko", old_hash);
    assert(unsetenv("PSVR2_BUSYBOX") == 0);
    assert(strstr(local_busybox(&session), "/tools/busybox"));
}

int main(void) {
    psvr2_constants constants = {.version = PSVR2_FW_0600};
    psvr2_exploit exploit = {.constants = &constants};
    psvr2_krw krw = {.ex = &exploit};
    psvr2_runtime runtime = {.krw = &krw};
    psvr2_shell shell = {.runtime = &runtime};
    persistence_plan plan;
    char backup[384], failed[384];
    assert(unsetenv("PSVR2_BUSYBOX") == 0);
    test_busybox_override(&shell, &constants, &plan);

    reset_case(true, &plan);
    assert(install_plan(&shell, &plan));
    assert_hash(PERSIST_MODULES "/stage1.ko", new_hash);
    snprintf(backup, sizeof(backup), PERSIST_BACKUPS "/stage1.ko.%s.bak", old_hash);
    assert_hash(backup, old_hash);
    assert(!mounted);
    assert_all_renames_flushed();
    size_t successful_flushes = fsync_count;
    assert(successful_flushes == 19);
    /* Every file/parent flush is a checked barrier, including post-rename failures. */
    for (size_t failure = 1; failure <= successful_flushes; ++failure) {
        reset_case(true, &plan);
        fail_fsync = failure;
        assert(!install_plan(&shell, &plan));
        assert_hash(PERSIST_MODULES "/stage1.ko", old_hash);
        assert(!mounted);
    }

    reset_case(true, &plan);
    put_file(PERSIST_BACKUPS, NULL, true);
    put_file(backup, old_hash, false);
    assert(install_plan(&shell, &plan));
    assert_hash(backup, old_hash);
    assert_all_renames_flushed();
    assert(fsync_count == 15); /* Existing backup is flushed too. */

    reset_case(false, &plan);
    assert(install_plan(&shell, &plan));
    successful_flushes = fsync_count;
    assert(successful_flushes == 11);
    assert_all_renames_flushed();
    for (size_t failure = 1; failure <= successful_flushes; ++failure) {
        reset_case(false, &plan);
        fail_fsync = failure;
        assert(!install_plan(&shell, &plan));
        assert(!lookup(PERSIST_MODULES "/stage1.ko"));
        assert(!mounted);
        if (failure > 7) {
            snprintf(failed, sizeof(failed), PERSIST_BACKUPS "/stage1.ko.%s.failed",
                plan.transaction_hash);
            assert_hash(failed, new_hash);
        }
    }

    /* Fsync applet discovery is read-only and precedes any mountpoint mutation. */
    const char *invalid_lists[] = {"ash\nsync\n", "ash\nfsync-extra\n", "xfsync\n"};
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(invalid_lists); ++index) {
        reset_case(true, &plan);
        applet_list = invalid_lists[index];
        assert(!install_plan(&shell, &plan));
        assert(!mounted && !lookup(PERSIST_MOUNT) && !fsync_count);
        assert_hash(PERSIST_MODULES "/stage1.ko", old_hash);
        assert(trace_count == 3);
    }
    reset_case(true, &plan); fail_listing = true;
    assert(!install_plan(&shell, &plan));
    assert(trace_count == 3 && !mounted && !lookup(PERSIST_MOUNT));
    reset_case(true, &plan);
    static char excessive_list[8194];
    memset(excessive_list, '\n', sizeof(excessive_list) - 1);
    memcpy(excessive_list, "fsync\n", 6);
    applet_list = excessive_list;
    assert(!install_plan(&shell, &plan));
    assert(trace_count == 3 && !mounted && !lookup(PERSIST_MOUNT));
    reset_case(true, &plan);
    applet_list = "ash\nsync\n";
    persistence_session read_only_session;
    assert(open_session(&read_only_session, &shell, true, false));
    assert(close_session(&read_only_session));
    assert(!fsync_count); /* Read-only verification does not flush the partition. */

    reset_case(true, &plan); fail_unmount = true;
    assert(!install_plan(&shell, &plan));
    assert(mounted); /* Never report completion after a failed unmount. */

    reset_case(true, &plan); fail_sync = true;
    assert(!install_plan(&shell, &plan));
    assert_hash(PERSIST_MODULES "/stage1.ko", old_hash);
    assert(!mounted);
    test_rollback_flushes(&shell, &plan, true);
    test_rollback_flushes(&shell, &plan, false);
    test_unpersist_flushes(&shell, &plan);
    puts("Persistence durability checks passed without hardware.");
    return 0;
}
