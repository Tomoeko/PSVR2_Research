#include "shell_internal.h"

#include "persistence_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define PSVR2_ISATTY _isatty
#define PSVR2_FILENO _fileno
#else
#include <unistd.h>
#define PSVR2_ISATTY isatty
#define PSVR2_FILENO fileno
#endif

#define PERSIST_DEVICE "/dev/mmcblk0p3"
#define PERSIST_MOUNT "/tmp/.psvr2-persist-mount"
#define PERSIST_MODULES PERSIST_MOUNT "/modules"
#define PERSIST_BACKUPS PERSIST_MODULES "/.psvr2-backups"
#define PERSIST_DEVICE_SIZE UINT64_C(67108864)
#define PERSIST_COMMAND_SIZE 500U
#define PERSIST_NAME_SIZE 96U

typedef struct {
    psvr2_shell *shell;
    char busybox[64];
    bool mounted;
    bool read_only;
} persistence_session;

typedef struct {
    char name[PERSIST_NAME_SIZE];
    char source[160];
    char candidate_hash[65];
    bool destination_exists;
    char destination_hash[65];
    char transaction_hash[65];
} persistence_plan;

bool psvr2_persist_name_valid(const char *name) {
    if (!name || !*name || strlen(name) >= PERSIST_NAME_SIZE ||
        !strcmp(name, ".") || !strcmp(name, ".."))
        return false;
    for (const unsigned char *cursor =
             (const unsigned char *)name; *cursor; ++cursor) {
        if (!isalnum(*cursor) && *cursor != '.' &&
            *cursor != '_' && *cursor != '-')
            return false;
    }
    return true;
}

bool psvr2_persist_parse_sha256(
    const char *text, char digest[65]) {
    if (!text || !digest) return false;
    while (isspace((unsigned char)*text)) ++text;
    if (strlen(text) < 64) return false;
    for (unsigned index = 0; index < 64; ++index) {
        if (!isxdigit((unsigned char)text[index]))
            return false;
        digest[index] =
            (char)tolower((unsigned char)text[index]);
    }
    if (!isspace((unsigned char)text[64]) &&
        text[64] != '\0')
        return false;
    digest[64] = '\0';
    return true;
}

static bool mount_line_field(
    const char *line, const char *expected, size_t *consumed) {
    size_t length = strlen(expected);
    if (strncmp(line, expected, length) ||
        !isspace((unsigned char)line[length]))
        return false;
    size_t offset = length;
    while (isspace((unsigned char)line[offset])) ++offset;
    *consumed = offset;
    return true;
}

bool psvr2_persist_mount_matches(
    const char *mounts, bool read_only) {
    if (!mounts) return false;
    const char *line = mounts;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (length < 500) {
            char copy[512];
            memcpy(copy, line, length);
            copy[length] = '\0';
            const char *cursor = copy;
            size_t used = 0;
            if (mount_line_field(cursor, PERSIST_DEVICE, &used)) {
                cursor += used;
                if (!mount_line_field(
                        cursor, PERSIST_MOUNT, &used))
                    return false;
                cursor += used;
                if (!mount_line_field(cursor, "vfat", &used))
                    return false;
                cursor += used;
                const char *options_end = cursor;
                while (*options_end &&
                       !isspace((unsigned char)*options_end))
                    ++options_end;
                size_t options_length =
                    (size_t)(options_end - cursor);
                const char *wanted = read_only ? "ro" : "rw";
                size_t wanted_length = strlen(wanted);
                for (size_t index = 0;
                     index + wanted_length <= options_length;
                     ++index) {
                    bool left =
                        index == 0 || cursor[index - 1] == ',';
                    bool right =
                        index + wanted_length == options_length ||
                        cursor[index + wanted_length] == ',';
                    if (left && right &&
                        !memcmp(
                            cursor + index, wanted,
                            wanted_length))
                        return true;
                }
                return false;
            }
        }
        if (!end) break;
        line = end + 1;
    }
    return false;
}

bool psvr2_persist_firmware_supported(
    uint32_t version, bool firmware_forced) {
    return !firmware_forced &&
        (version == PSVR2_FW_0110 || version == PSVR2_FW_0600);
}

static bool persistence_firmware_allowed(const psvr2_shell *shell) {
    if (!shell || !shell->runtime || !shell->runtime->krw ||
        !shell->runtime->krw->ex ||
        !shell->runtime->krw->ex->constants)
        return false;
    const psvr2_constants *constants =
        shell->runtime->krw->ex->constants;
    if (constants->firmware_forced) {
        puts("[-] Persistent module changes require an auto-detected "
             "firmware; --fw overrides are refused.");
        return false;
    }
    if (!psvr2_persist_firmware_supported(
            constants->version, constants->firmware_forced)) {
        puts("[-] Persistent module changes are certified only for "
             "firmware 01.10 and 06.00.");
        return false;
    }
    return true;
}

static bool persistence_device_allowed(const psvr2_shell *shell) {
    if (!persistence_firmware_allowed(shell))
        return false;
    if (!psvr2_stage1_discover(shell->runtime)) {
        puts("[-] A verified Stage1 mailbox is required.");
        return false;
    }
    return true;
}

static bool remote_command(
    persistence_session *session, psvr2_buffer *output,
    const char *format, ...) {
    char command[PERSIST_COMMAND_SIZE];
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(
        command, sizeof(command), format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= sizeof(command)) {
        puts("[-] Internal persistence command exceeded the Stage1 limit.");
        return false;
    }
    int64_t result = -1;
    bool ok = psvr2_stage1_exec(
        session->shell->runtime, command, 30, &result, output);
    if (!ok || result != 0) {
        fprintf(stderr,
                "[-] Device command failed%s%lld.\n",
                ok ? " with exit " : " before returning, exit ",
                (long long)result);
        return false;
    }
    return true;
}

static bool remote_output(
    persistence_session *session, char *output,
    size_t output_size, const char *format, ...) {
    char command[PERSIST_COMMAND_SIZE];
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(
        command, sizeof(command), format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= sizeof(command))
        return false;
    psvr2_buffer buffer = {0};
    int64_t result = -1;
    bool ok = psvr2_stage1_exec(
        session->shell->runtime, command, 30, &result, &buffer) &&
        result == 0;
    if (ok) {
        size_t length =
            psvr2_min_size(buffer.len, output_size - 1);
        if (length) memcpy(output, buffer.data, length);
        output[length] = '\0';
    } else {
        fprintf(
            stderr,
            "[-] Device output command failed%s%lld: %s\n",
            result >= 0 ? " with exit " : " before returning, exit ",
            (long long)result, command);
        if (buffer.len) {
            fwrite(buffer.data, 1, buffer.len, stderr);
            if (buffer.data[buffer.len - 1] != '\n')
                fputc('\n', stderr);
        }
    }
    psvr2_buffer_free(&buffer);
    return ok;
}

static bool locate_busybox(persistence_session *session) {
    char output[96];
    if (!remote_output(
            session, output, sizeof(output),
            "if [ -x /tmp/busybox ]; then echo /tmp/busybox; "
            "elif [ -x /data/modules/busybox ]; then "
            "echo /data/modules/busybox; else exit 1; fi"))
        return false;
    char *path = psvr2_trim(output);
    if (strcmp(path, "/tmp/busybox") &&
        strcmp(path, "/data/modules/busybox"))
        return false;
    snprintf(
        session->busybox, sizeof(session->busybox), "%s", path);
    return true;
}

static const char *local_busybox(
    const persistence_session *session) {
    const char *override = getenv("PSVR2_BUSYBOX");
    if (override && *override)
        return psvr2_file_exists(override) ? override : NULL;
    const psvr2_constants *constants =
        session && session->shell && session->shell->runtime &&
        session->shell->runtime->krw &&
        session->shell->runtime->krw->ex
            ? session->shell->runtime->krw->ex->constants
            : NULL;
    const char *firmware = constants
        ? psvr2_firmware_name(constants->version) : NULL;
    static char build_path[128];
    static char parent_build_path[132];
    if (!firmware || !strcmp(firmware, "unknown"))
        return NULL;
    int build_length = snprintf(
        build_path, sizeof(build_path),
        "output/psvr2-build/%s/tools/busybox", firmware);
    int parent_length = snprintf(
        parent_build_path, sizeof(parent_build_path),
        "../output/psvr2-build/%s/tools/busybox", firmware);
    if (build_length < 0 ||
        (size_t)build_length >= sizeof(build_path) ||
        parent_length < 0 ||
        (size_t)parent_length >= sizeof(parent_build_path))
        return NULL;
    const char *const candidates[] = {
        build_path,
        parent_build_path
    };
    for (size_t index = 0;
         index < PSVR2_ARRAY_LEN(candidates); ++index)
        if (psvr2_file_exists(candidates[index]))
            return candidates[index];
    return NULL;
}

static bool busybox_matches_repository(
    persistence_session *session, const char *local_path) {
    uint8_t local_digest[32];
    char expected[65], observed[65];
    uint64_t local_size = 0;
    if (!local_path ||
        !psvr2_sha256_file(
            local_path, local_digest, &local_size) ||
        local_size < 1024)
        return false;
    psvr2_sha256_hex(local_digest, expected);
    char output[160];
    if (!remote_output(
            session, output, sizeof(output),
            "%s sha256sum '%s'",
            session->busybox, session->busybox) ||
        !psvr2_persist_parse_sha256(output, observed))
        return false;
    if (strcmp(expected, observed)) {
        fprintf(stderr,
                "[-] Device BusyBox hash %s does not match the "
                "repository binary %s.\n",
                observed, expected);
        return false;
    }
    return true;
}

static bool ensure_busybox(
    persistence_session *session, bool allow_upload) {
    const char *path = local_busybox(session);
    if (!path) {
        puts("[-] Set PSVR2_BUSYBOX to a matching target BusyBox executable.");
        return false;
    }
    if (locate_busybox(session) &&
        busybox_matches_repository(session, path))
        return true;
    if (!allow_upload) {
        puts("[-] A repository-matching BusyBox is required at "
             "/tmp/busybox for the write phase.");
        return false;
    }
    puts("[*] Uploading the repository BusyBox to volatile /tmp...");
    char *arguments[] = {"fast_upload", (char *)path};
    if (psvr2_shell_fast_upload(
            session->shell, 2, arguments) != 0 ||
        !locate_busybox(session) ||
        strcmp(session->busybox, "/tmp/busybox") ||
        !busybox_matches_repository(session, path)) {
        puts("[-] BusyBox upload or verification failed.");
        return false;
    }
    return true;
}

static bool ensure_tmp_busybox_candidate(
    persistence_session *session)
{
    const char *path = local_busybox(session);
    if (!path)
        return false;
    if (!strcmp(session->busybox, "/tmp/busybox") &&
        busybox_matches_repository(session, path))
        return true;

    puts("[*] Materializing the verified BusyBox candidate in /tmp...");
    char *arguments[] = {"fast_upload", (char *)path};
    if (psvr2_shell_fast_upload(
            session->shell, 2, arguments) != 0 ||
        !locate_busybox(session) ||
        strcmp(session->busybox, "/tmp/busybox") ||
        !busybox_matches_repository(session, path)) {
        puts("[-] BusyBox candidate upload or verification failed.");
        return false;
    }
    return true;
}

static bool remote_sha256(
    persistence_session *session, const char *path,
    char digest[65]) {
    char output[160];
    if (!remote_output(
            session, output, sizeof(output),
            "%s sha256sum '%s'", session->busybox, path))
        return false;
    return psvr2_persist_parse_sha256(output, digest);
}

static bool remote_exists(
    persistence_session *session, const char *path,
    bool *exists) {
    char output[8];
    if (!remote_output(
            session, output, sizeof(output),
            "p='%s'; if [ -f \"$p\" ]; then echo 1; "
            "elif [ -e \"$p\" ] || [ -L \"$p\" ]; then echo X; "
            "else echo 0; fi",
            path))
        return false;
    char *state = psvr2_trim(output);
    *exists = state[0] == '1' && state[1] == '\0';
    if (state[0] == 'X' && state[1] == '\0')
        fprintf(stderr,
                "[-] Refusing non-regular persistent path: %s\n",
                path);
    return *exists || (state[0] == '0' && state[1] == '\0');
}

static bool filesystem_is_unmounted(
    persistence_session *session) {
    char mounts[4096];
    if (!remote_output(
            session, mounts, sizeof(mounts),
            "%s cat /proc/mounts", session->busybox))
        return false;
    const char *line = mounts;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if ((length >= strlen(PERSIST_DEVICE) &&
             !strncmp(line, PERSIST_DEVICE, strlen(PERSIST_DEVICE)) &&
             isspace((unsigned char)line[strlen(PERSIST_DEVICE)])) ||
            strstr(line, " " PERSIST_MOUNT " ")) {
            puts("[-] Refusing persistence: the target device or dedicated "
                 "mountpoint is already mounted.");
            return false;
        }
        if (!end) break;
        line = end + 1;
    }
    return true;
}

static bool validate_partition_size(
    persistence_session *session) {
    char output[96];
    if (!remote_output(
            session, output, sizeof(output),
            "%s blockdev --getsize64 " PERSIST_DEVICE,
            session->busybox))
        return false;
    uint64_t size = 0;
    if (!psvr2_parse_u64(psvr2_trim(output), &size) ||
        size != PERSIST_DEVICE_SIZE) {
        fprintf(stderr,
                "[-] Refusing persistence: " PERSIST_DEVICE
                " is %llu bytes, expected %llu.\n",
                (unsigned long long)size,
                (unsigned long long)PERSIST_DEVICE_SIZE);
        return false;
    }
    return true;
}

static bool busybox_supports_fsync(persistence_session *session) {
    psvr2_buffer applets = {0};
    bool ok = remote_command(
        session, &applets, "%s --list", session->busybox);
    bool found = false;
    if (ok && applets.len <= 8192) {
        size_t start = 0;
        for (size_t end = 0; end <= applets.len; ++end) {
            if (end != applets.len && applets.data[end] != '\n')
                continue;
            if (end - start == 5 &&
                !memcmp(applets.data + start, "fsync", 5))
                found = true;
            start = end + 1;
        }
    }
    psvr2_buffer_free(&applets);
    if (!ok || !found)
        puts("[-] Verified BusyBox must provide the fsync applet "
             "before a persistent write mount is opened.");
    return ok && found;
}

static bool remote_fsync(
    persistence_session *session, const char *path) {
    /* BusyBox opens files and directories read-only and reports fsync errors. */
    return remote_command(
        session, NULL, "%s fsync '%s'", session->busybox, path);
}

static bool fsync_recovery_directories(persistence_session *session) {
    return remote_fsync(session, PERSIST_BACKUPS) &&
           remote_fsync(session, PERSIST_MODULES) &&
           remote_fsync(session, PERSIST_MOUNT);
}

static bool fsync_persistent_file(
    persistence_session *session, const char *path) {
    return remote_fsync(session, path) &&
           fsync_recovery_directories(session);
}

static bool close_session(persistence_session *session) {
    bool ok = true;
    if (session->mounted) {
        if (!session->read_only &&
            !remote_command(
                session, NULL, "%s sync", session->busybox))
            ok = false;
        if (!remote_command(
                session, NULL, "%s umount " PERSIST_MOUNT,
                session->busybox)) {
            puts("CRITICAL: Could not unmount " PERSIST_MOUNT
                 "; do not reboot or perform another persistent operation.");
            ok = false;
        } else {
            session->mounted = false;
        }
    }
    if (!session->mounted)
        (void)remote_command(
            session, NULL, "%s rmdir " PERSIST_MOUNT,
            session->busybox);
    return ok;
}

static bool open_session(
    persistence_session *session, psvr2_shell *shell,
    bool read_only, bool allow_busybox_upload) {
    memset(session, 0, sizeof(*session));
    session->shell = shell;
    session->read_only = read_only;
    if (!persistence_device_allowed(shell) ||
        !ensure_busybox(session, allow_busybox_upload) ||
        (!read_only && !busybox_supports_fsync(session)) ||
        !validate_partition_size(session) ||
        !filesystem_is_unmounted(session))
        return false;

    if (!remote_command(
            session, NULL,
            "if [ -e " PERSIST_MOUNT " ] || "
            "[ -L " PERSIST_MOUNT " ]; then exit 18; fi; "
            "%s mkdir " PERSIST_MOUNT,
            session->busybox))
        return false;
    if (!remote_command(
            session, NULL,
            "%s mount -t vfat -o %s,nosuid,nodev,noexec "
            PERSIST_DEVICE " " PERSIST_MOUNT,
            session->busybox, read_only ? "ro" : "rw")) {
        (void)remote_command(
            session, NULL, "%s rmdir " PERSIST_MOUNT,
            session->busybox);
        return false;
    }
    session->mounted = true;

    char mounts[4096];
    if (!remote_output(
            session, mounts, sizeof(mounts),
            "%s cat /proc/mounts", session->busybox) ||
        !psvr2_persist_mount_matches(mounts, read_only)) {
        puts("[-] Mounted filesystem did not match the exact expected "
             "device, target, type, and access mode.");
        (void)close_session(session);
        return false;
    }
    if (!remote_command(
            session, NULL,
            "%s test -f " PERSIST_MOUNT "/pcbid && "
            "%s test -d " PERSIST_MOUNT "/disp",
            session->busybox, session->busybox)) {
        puts("[-] Mounted filesystem lacks the expected FACTORY_2 "
             "identity files.");
        (void)close_session(session);
        return false;
    }
    return true;
}

static bool calculate_transaction_hash(
    const char *operation, const persistence_plan *plan,
    char digest[65]) {
    char description[640];
    int length = snprintf(
        description, sizeof(description),
        "psvr2-persistence-v1\noperation=%s\nname=%s\n"
        "source=%s\ncandidate=%s\ndestination=%s\n",
        operation, plan->name, plan->source,
        plan->candidate_hash,
        plan->destination_exists
            ? plan->destination_hash : "absent");
    if (length < 0 || (size_t)length >= sizeof(description))
        return false;
    uint8_t binary[32];
    if (!psvr2_sha256(description, (size_t)length, binary))
        return false;
    psvr2_sha256_hex(binary, digest);
    return true;
}

static bool extract_confirmation(
    int *argc, char **argv, const char **confirmation) {
    *confirmation = NULL;
    if (*argc >= 3 &&
        !strcmp(argv[*argc - 2], "--confirm")) {
        *confirmation = argv[*argc - 1];
        *argc -= 2;
    }
    for (int index = 1; index < *argc; ++index)
        if (!strcmp(argv[index], "--confirm"))
            return false;
    return true;
}

static bool confirm_plan(
    const char *action, const persistence_plan *plan,
    const char *provided) {
    printf("\nPersistent eMMC transaction:\n"
           "  Action:       %s\n"
           "  Partition:    " PERSIST_DEVICE
           " (FACTORY_2, %llu bytes)\n"
           "  File:         %s\n"
           "  Candidate:    %s\n"
           "  Existing:     %s\n"
           "  Confirmation: %s\n",
           action,
           (unsigned long long)PERSIST_DEVICE_SIZE,
           plan->name, plan->candidate_hash,
           plan->destination_exists
               ? plan->destination_hash : "absent",
           plan->transaction_hash);
    if (provided)
        return strcmp(provided, plan->transaction_hash) == 0;
    if (!PSVR2_ISATTY(PSVR2_FILENO(stdin))) {
        puts("[-] Non-interactive persistent operations require "
             "--confirm <full-transaction-hash>.");
        return false;
    }
    printf("Type the complete confirmation hash to proceed: ");
    fflush(stdout);
    char input[128];
    if (!fgets(input, sizeof(input), stdin)) return false;
    return !strcmp(psvr2_trim(input), plan->transaction_hash);
}

static bool make_plan(
    psvr2_shell *shell, const char *operation,
    const char *source, const char *name,
    const char *required_candidate_hash,
    persistence_plan *plan, bool allow_busybox_upload) {
    memset(plan, 0, sizeof(*plan));
    snprintf(plan->name, sizeof(plan->name), "%s", name);
    snprintf(plan->source, sizeof(plan->source), "%s", source);

    persistence_session session;
    if (!open_session(
            &session, shell, true, allow_busybox_upload))
        return false;
    bool ok = true;
    if (allow_busybox_upload &&
        !strcmp(source, "/tmp/busybox"))
        ok = ensure_tmp_busybox_candidate(&session);
    if (ok)
        ok = remote_sha256(
            &session, source, plan->candidate_hash);
    if (ok && required_candidate_hash &&
        strcmp(required_candidate_hash, plan->candidate_hash)) {
        puts("[-] The volatile candidate hash does not match the "
             "verified host file.");
        ok = false;
    }
    char destination[256];
    snprintf(destination, sizeof(destination),
             PERSIST_MODULES "/%s", name);
    if (ok)
        ok = remote_exists(
            &session, destination,
            &plan->destination_exists);
    if (ok && plan->destination_exists)
        ok = remote_sha256(
            &session, destination,
            plan->destination_hash);
    if (!close_session(&session)) ok = false;
    return ok && calculate_transaction_hash(
        operation, plan, plan->transaction_hash);
}

static bool ensure_directory(
    persistence_session *session, const char *path) {
    return remote_command(
        session, NULL,
        "if [ -e '%s' ]; then %s test -d '%s'; "
        "else %s mkdir '%s'; fi",
        path, session->busybox, path,
        session->busybox, path);
}

static bool verify_plan_unchanged(
    persistence_session *session,
    const persistence_plan *plan) {
    char observed[65];
    if (!remote_sha256(
            session, plan->source, observed) ||
        strcmp(observed, plan->candidate_hash)) {
        puts("[-] Candidate changed after confirmation.");
        return false;
    }
    char destination[256];
    snprintf(destination, sizeof(destination),
             PERSIST_MODULES "/%s", plan->name);
    bool exists = false;
    if (!remote_exists(session, destination, &exists) ||
        exists != plan->destination_exists)
        return false;
    if (exists &&
        (!remote_sha256(session, destination, observed) ||
         strcmp(observed, plan->destination_hash))) {
        puts("[-] Persistent destination changed after confirmation.");
        return false;
    }
    return true;
}

static bool preserve_existing(
    persistence_session *session,
    const persistence_plan *plan) {
    if (!plan->destination_exists) return true;
    char destination[256], backup[384], temporary[400];
    snprintf(destination, sizeof(destination),
             PERSIST_MODULES "/%s", plan->name);
    snprintf(backup, sizeof(backup),
             PERSIST_BACKUPS "/%s.%s.bak",
             plan->name, plan->destination_hash);
    snprintf(temporary, sizeof(temporary), "%s.new", backup);
    bool exists = false;
    if (!remote_exists(session, backup, &exists)) return false;
    char observed[65];
    if (exists)
        return remote_sha256(session, backup, observed) &&
               !strcmp(observed, plan->destination_hash) &&
               fsync_persistent_file(session, backup);

    bool temporary_exists = false;
    if (!remote_exists(
            session, temporary, &temporary_exists))
        return false;
    if (!temporary_exists &&
        !remote_command(
            session, NULL, "%s cp '%s' '%s'",
            session->busybox, destination, temporary))
        return false;
    if (!remote_command(
            session, NULL, "%s sync", session->busybox) ||
        !remote_sha256(session, temporary, observed) ||
        strcmp(observed, plan->destination_hash) ||
        !fsync_persistent_file(session, temporary))
        return false;
    if (!remote_command(
            session, NULL, "%s mv '%s' '%s'",
            session->busybox, temporary, backup) ||
        !fsync_persistent_file(session, backup) ||
        !remote_command(
            session, NULL, "%s sync", session->busybox) ||
        !remote_sha256(session, backup, observed) ||
        strcmp(observed, plan->destination_hash))
        return false;
    return true;
}

static bool restore_after_failed_install(
    persistence_session *session,
    const persistence_plan *plan,
    const char *destination) {
    char observed[65];
    if (plan->destination_exists) {
        char backup[384], staged[384];
        snprintf(backup, sizeof(backup),
                 PERSIST_BACKUPS "/%s.%s.bak",
                 plan->name, plan->destination_hash);
        snprintf(staged, sizeof(staged),
                 PERSIST_MODULES "/.%s.restore.%s",
                 plan->name, plan->destination_hash);
        bool staged_exists = false;
        if (!remote_exists(
                session, staged, &staged_exists))
            return false;
        if (!staged_exists &&
            !remote_command(
                session, NULL, "%s cp '%s' '%s'",
                session->busybox, backup, staged))
            return false;
        return remote_command(
                   session, NULL, "%s sync", session->busybox) &&
               remote_sha256(session, staged, observed) &&
               !strcmp(observed, plan->destination_hash) &&
               fsync_persistent_file(session, staged) &&
               remote_command(
                   session, NULL, "%s mv -f '%s' '%s'",
                   session->busybox, staged, destination) &&
               fsync_persistent_file(session, destination) &&
               remote_command(
                   session, NULL, "%s sync", session->busybox) &&
               remote_sha256(session, destination, observed) &&
               !strcmp(observed, plan->destination_hash);
    }

    char failed[384];
    snprintf(failed, sizeof(failed),
             PERSIST_BACKUPS "/%s.%s.failed",
             plan->name, plan->transaction_hash);
    bool failed_exists = false;
    if (!remote_exists(
            session, failed, &failed_exists) ||
        failed_exists ||
        !fsync_persistent_file(session, destination) ||
        !remote_command(
            session, NULL, "%s mv '%s' '%s'",
            session->busybox, destination, failed) ||
        !fsync_persistent_file(session, failed) ||
        !remote_command(
            session, NULL, "%s sync", session->busybox))
        return false;
    bool destination_exists = true;
    return remote_exists(
               session, destination, &destination_exists) &&
           !destination_exists;
}

static bool install_plan(
    psvr2_shell *shell, const persistence_plan *plan) {
    persistence_session session;
    if (!open_session(&session, shell, false, false)) {
        puts("[-] Write phase stopped while opening the verified "
             "FACTORY_2 session.");
        return false;
    }
    bool ok = ensure_directory(&session, PERSIST_MODULES);
    if (!ok)
        puts("[-] Write phase stopped while validating the modules "
             "directory.");
    if (ok) {
        ok = ensure_directory(&session, PERSIST_BACKUPS);
        if (!ok)
            puts("[-] Write phase stopped while validating the recovery "
                 "directory.");
    }
    if (ok)
        ok = fsync_recovery_directories(&session);
    if (ok) {
        ok = verify_plan_unchanged(&session, plan);
        if (!ok)
            puts("[-] Write phase stopped because the confirmed plan "
                 "no longer matched.");
    }
    if (ok) {
        ok = preserve_existing(&session, plan);
        if (!ok)
            puts("[-] Write phase stopped while preserving the existing "
                 "persistent file.");
    }

    char destination[256], staged[384], observed[65];
    snprintf(destination, sizeof(destination),
             PERSIST_MODULES "/%s", plan->name);
    snprintf(staged, sizeof(staged),
             PERSIST_MODULES "/.%s.new.%s",
             plan->name, plan->candidate_hash);
    bool staged_exists = false;
    if (ok) {
        ok = remote_exists(
            &session, staged, &staged_exists);
        if (!ok)
            puts("[-] Write phase could not validate the staging path.");
    }
    if (ok && !staged_exists) {
        ok = remote_command(
            &session, NULL, "%s cp '%s' '%s'",
            session.busybox, plan->source, staged);
        if (!ok)
            puts("[-] Write phase could not create the staging file.");
    }
    if (ok) {
        ok = remote_command(
                 &session, NULL, "%s sync", session.busybox) &&
             remote_sha256(&session, staged, observed) &&
             !strcmp(observed, plan->candidate_hash) &&
             fsync_persistent_file(&session, staged);
        if (!ok)
            puts("[-] Write phase could not verify the staged candidate.");
    }
    bool replaced = false;
    if (ok) {
        ok = remote_command(
            &session, NULL, "%s mv -f '%s' '%s'",
            session.busybox, staged, destination);
        replaced = ok;
    }
    if (ok)
        ok = fsync_persistent_file(&session, destination) &&
             remote_command(
                 &session, NULL, "%s sync", session.busybox) &&
             remote_sha256(
                 &session, destination, observed) &&
             !strcmp(observed, plan->candidate_hash);
    if (replaced && !ok)
        puts("[-] Write phase could not verify the installed candidate.");
    if (!ok && replaced) {
        puts("[!] Final verification failed; restoring the prior state...");
        if (!restore_after_failed_install(
                &session, plan, destination))
            puts("CRITICAL: Automatic persistence rollback failed.");
        else
            puts("[+] Prior persistent state was restored and verified.");
    }
    if (!close_session(&session)) ok = false;
    return ok;
}

static int persist_one(
    psvr2_shell *shell, const char *operation,
    const char *source, const char *name,
    const char *required_candidate_hash,
    const char *confirmation, bool allow_busybox_upload) {
    persistence_plan plan;
    if (!make_plan(
            shell, operation, source, name,
            required_candidate_hash, &plan,
            allow_busybox_upload))
        return 1;
    if (plan.destination_exists &&
        !strcmp(
            plan.candidate_hash, plan.destination_hash)) {
        printf("[+] Persistent %s already matches candidate SHA-256 %s; "
               "no write is needed.\n",
               name, plan.candidate_hash);
        return 0;
    }
    if (!confirm_plan(operation, &plan, confirmation)) {
        puts("[-] Persistent transaction cancelled or confirmation "
             "did not match.");
        return 1;
    }
    puts("[*] Reopening FACTORY_2 read-write and revalidating the plan...");
    if (!install_plan(shell, &plan)) {
        puts("[-] Persistent transaction failed. The verified backup, "
             "if created, was retained.");
        return 1;
    }
    printf("[+] Installed %s with verified SHA-256 %s\n",
           name, plan.candidate_hash);
    return 0;
}

static bool stage1_candidate_valid(
    const char *path, char digest[65], uint64_t *size) {
    psvr2_buffer data = {0};
    uint8_t binary_digest[32];
    static const char description[] =
        "description=PSVR2 Stage1 - Fast File Transfer + "
        "Command Execution";
    static const char vermagic[] =
        "vermagic=4.4.139 SMP preempt aarch64";
    bool ok =
        psvr2_sha256_file(path, binary_digest, size) &&
        *size >= 1024 && *size <= UINT64_C(2) * 1024 * 1024 &&
        psvr2_read_file(path, &data) && data.len >= 64 &&
        !memcmp(data.data, "\x7f" "ELF", 4) &&
        data.data[4] == 2 && data.data[5] == 1 &&
        psvr2_load_le16(data.data + 16) == 1 &&
        psvr2_load_le16(data.data + 18) == 183;
    bool description_found = false;
    bool vermagic_found = false;
    if (ok) {
        for (size_t offset = 0; offset < data.len; ++offset) {
            if (!description_found &&
                sizeof(description) - 1 <= data.len - offset &&
                !memcmp(
                    data.data + offset, description,
                    sizeof(description) - 1))
                description_found = true;
            if (!vermagic_found &&
                sizeof(vermagic) - 1 <= data.len - offset &&
                !memcmp(
                    data.data + offset, vermagic,
                    sizeof(vermagic) - 1))
                vermagic_found = true;
        }
        ok = description_found && vermagic_found;
    }
    psvr2_buffer_free(&data);
    if (!ok) return false;
    psvr2_sha256_hex(binary_digest, digest);
    return true;
}

static int cmd_stage1_install(
    psvr2_shell *shell, int argc, char **argv) {
    const char *confirmation = NULL;
    if (!extract_confirmation(
            &argc, argv, &confirmation) || argc != 2) {
        puts("Usage: stage1_install <local-stage1.ko> "
             "[--confirm <transaction-sha256>]");
        return 1;
    }
    if (!persistence_firmware_allowed(shell)) return 1;
    char host_hash[65];
    uint64_t size = 0;
    if (!stage1_candidate_valid(argv[1], host_hash, &size)) {
        puts("[-] Candidate is not a bounded AArch64 ELF relocatable "
             "kernel module.");
        return 1;
    }
    printf("[*] Host candidate: %llu bytes, SHA-256 %s\n",
           (unsigned long long)size, host_hash);
    puts("[*] Loading the candidate from volatile /tmp before "
         "persistent access...");
    if (!psvr2_load_stage1(
            shell->runtime, argv[1], true, true)) {
        puts("[-] Candidate did not load from /tmp; eMMC was not touched.");
        return 1;
    }
    int64_t result = -1;
    psvr2_buffer output = {0};
    bool mailbox_ok = psvr2_stage1_exec(
        shell->runtime, "echo PSVR2_STAGE1_INSTALL_OK",
        10, &result, &output);
    bool output_ok = mailbox_ok && result == 0 && output.data &&
        !strcmp(
            psvr2_trim((char *)output.data),
            "PSVR2_STAGE1_INSTALL_OK");
    psvr2_buffer_free(&output);
    if (!output_ok) {
        puts("[-] Candidate mailbox self-test failed; eMMC was not touched.");
        return 1;
    }
    return persist_one(
        shell, "stage1-install", "/tmp/stage1.ko",
        "stage1.ko", host_hash, confirmation, true);
}

static int cmd_stage1_verify(
    psvr2_shell *shell, int argc, char **argv) {
    if (argc > 2) {
        puts("Usage: stage1_verify [local-stage1.ko]");
        return 1;
    }
    char local_hash[65] = {0};
    uint64_t local_size = 0;
    if (argc == 2 &&
        !stage1_candidate_valid(
            argv[1], local_hash, &local_size)) {
        puts("[-] Local Stage1 candidate is invalid.");
        return 1;
    }
    persistence_session session;
    if (!open_session(&session, shell, true, true))
        return 1;
    char persistent_hash[65];
    bool ok = remote_sha256(
        &session, PERSIST_MODULES "/stage1.ko",
        persistent_hash);
    if (!close_session(&session)) ok = false;
    if (!ok) {
        puts("[-] Persistent stage1.ko is missing or unreadable.");
        return 1;
    }
    printf("[+] Persistent stage1.ko SHA-256: %s\n",
           persistent_hash);
    if (argc == 2) {
        printf("    Local stage1.ko: %llu bytes, %s\n",
               (unsigned long long)local_size, local_hash);
        if (strcmp(local_hash, persistent_hash)) {
            puts("[-] Persistent and local Stage1 hashes differ.");
            return 1;
        }
        puts("[+] Persistent Stage1 exactly matches the local candidate.");
    }
    return 0;
}

static int cmd_persist(
    psvr2_shell *shell, int argc, char **argv) {
    const char *confirmation = NULL;
    if (!extract_confirmation(
            &argc, argv, &confirmation) || argc != 2 ||
        !psvr2_persist_name_valid(argv[1])) {
        puts("Usage: persist <explicit-/tmp-filename> "
             "[--confirm <transaction-sha256>]");
        puts("Only one validated filename is installed per transaction.");
        return 1;
    }
    char source[160];
    snprintf(source, sizeof(source), "/tmp/%s", argv[1]);
    return persist_one(
        shell, "persist", source, argv[1], NULL,
        confirmation, true);
}

static int cmd_unpersist(
    psvr2_shell *shell, int argc, char **argv) {
    const char *confirmation = NULL;
    if (!extract_confirmation(
            &argc, argv, &confirmation) || argc != 2 ||
        !psvr2_persist_name_valid(argv[1])) {
        puts("Usage: unpersist <explicit-filename> "
             "[--confirm <transaction-sha256>]");
        puts("The file is moved into a verified recovery directory; "
             "it is not deleted.");
        return 1;
    }

    persistence_session session;
    if (!open_session(&session, shell, true, true))
        return 1;
    persistence_plan plan = {0};
    snprintf(plan.name, sizeof(plan.name), "%s", argv[1]);
    snprintf(plan.source, sizeof(plan.source),
             PERSIST_MODULES "/%s", argv[1]);
    bool ok = remote_exists(
        &session, plan.source, &plan.destination_exists);
    if (ok && !plan.destination_exists) {
        puts("[-] The requested persistent file does not exist.");
        ok = false;
    }
    if (ok)
        ok = remote_sha256(
            &session, plan.source, plan.candidate_hash);
    snprintf(plan.destination_hash, sizeof(plan.destination_hash),
             "%s", plan.candidate_hash);
    if (!close_session(&session)) ok = false;
    if (!ok || !calculate_transaction_hash(
            "unpersist", &plan, plan.transaction_hash))
        return 1;
    if (!confirm_plan("unpersist", &plan, confirmation)) {
        puts("[-] Unpersist cancelled or confirmation did not match.");
        return 1;
    }

    if (!open_session(&session, shell, false, false))
        return 1;
    char source[256], disabled[384], observed[65];
    snprintf(source, sizeof(source),
             PERSIST_MODULES "/%s", argv[1]);
    snprintf(disabled, sizeof(disabled),
             PERSIST_BACKUPS "/%s.%s.disabled",
             argv[1], plan.candidate_hash);
    ok = ensure_directory(&session, PERSIST_BACKUPS) &&
         remote_sha256(&session, source, observed) &&
         !strcmp(observed, plan.candidate_hash);
    bool disabled_exists = false;
    if (ok)
        ok = remote_exists(
            &session, disabled, &disabled_exists);
    if (ok && disabled_exists)
        ok = remote_sha256(&session, disabled, observed) &&
             !strcmp(observed, plan.candidate_hash);
    if (ok)
        ok = fsync_persistent_file(&session, source);
    bool moved = false;
    if (ok) {
        ok = remote_command(
            &session, NULL, "%s mv -f '%s' '%s'",
            session.busybox, source, disabled);
        moved = ok;
    }
    if (ok)
        ok = fsync_persistent_file(&session, disabled) &&
             remote_command(
                 &session, NULL, "%s sync", session.busybox);
    bool source_exists = true;
    if (ok)
        ok = remote_exists(
                 &session, source, &source_exists) &&
             !source_exists &&
             remote_sha256(
                 &session, disabled, observed) &&
             !strcmp(observed, plan.candidate_hash);
    if (!ok && moved) {
        puts("[!] Unpersist verification failed; restoring the file...");
        bool source_now_exists = false;
        if (!remote_exists(
                &session, source, &source_now_exists) ||
            source_now_exists ||
            !fsync_persistent_file(&session, disabled) ||
            !remote_command(
                &session, NULL, "%s mv '%s' '%s'",
                session.busybox, disabled, source) ||
            !fsync_persistent_file(&session, source) ||
            !remote_command(
                &session, NULL, "%s sync", session.busybox) ||
            !remote_sha256(
                &session, source, observed) ||
            strcmp(observed, plan.candidate_hash))
            puts("CRITICAL: Automatic unpersist rollback failed.");
        else
            puts("[+] Original persistent file was restored and verified.");
    }
    if (!close_session(&session)) ok = false;
    if (!ok) {
        puts("[-] Unpersist failed; inspect the recovery directory "
             "before retrying.");
        return 1;
    }
    printf("[+] Disabled %s; verified recovery copy: "
           ".psvr2-backups/%s.%s.disabled\n",
           argv[1], argv[1], plan.candidate_hash);
    return 0;
}

static int cmd_stage1_rollback(
    psvr2_shell *shell, int argc, char **argv) {
    const char *confirmation = NULL;
    char backup_hash[65];
    if (!extract_confirmation(
            &argc, argv, &confirmation) || argc != 2 ||
        !psvr2_persist_parse_sha256(argv[1], backup_hash) ||
        strlen(argv[1]) != 64) {
        puts("Usage: stage1_rollback <full-backup-sha256> "
             "[--confirm <transaction-sha256>]");
        return 1;
    }

    persistence_session session;
    if (!open_session(&session, shell, true, true))
        return 1;
    char backup[384], observed[65];
    snprintf(backup, sizeof(backup),
             PERSIST_BACKUPS "/stage1.ko.%s.bak",
             backup_hash);
    bool ok = remote_sha256(&session, backup, observed) &&
              !strcmp(observed, backup_hash);
    if (!ok) {
        snprintf(backup, sizeof(backup),
                 PERSIST_BACKUPS "/stage1.ko.%s.disabled",
                 backup_hash);
        ok = remote_sha256(&session, backup, observed) &&
             !strcmp(observed, backup_hash);
    }
    if (ok)
        ok = remote_command(
            &session, NULL,
            "%s cp '%s' /tmp/stage1.rollback.ko",
            session.busybox, backup);
    if (!close_session(&session)) ok = false;
    if (!ok) {
        puts("[-] Requested verified Stage1 backup was not found.");
        return 1;
    }
    return persist_one(
        shell, "stage1-rollback",
        "/tmp/stage1.rollback.ko", "stage1.ko",
        backup_hash, confirmation, false);
}

static int cmd_persist_backups(
    psvr2_shell *shell, int argc, char **argv) {
    if (argc > 2 ||
        (argc == 2 && !psvr2_persist_name_valid(argv[1]))) {
        puts("Usage: persist_backups [filename]");
        return 1;
    }
    persistence_session session;
    if (!open_session(&session, shell, true, true))
        return 1;
    char output[8192];
    bool ok = remote_output(
        &session, output, sizeof(output),
        "%s ls -1 " PERSIST_BACKUPS,
        session.busybox);
    if (!close_session(&session)) ok = false;
    if (!ok) {
        puts("[-] No matching persistent recovery files were found.");
        return 1;
    }
    char *trimmed = psvr2_trim(output);
    if (!*trimmed) {
        puts("[-] No matching persistent recovery files were found.");
        return 1;
    }
    if (argc == 1) {
        puts(trimmed);
        return 0;
    }
    size_t prefix_length = strlen(argv[1]);
    bool found = false;
    char *line = trimmed;
    while (*line) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        if (!strncmp(line, argv[1], prefix_length) &&
            line[prefix_length] == '.') {
            puts(line);
            found = true;
        }
        if (!end) break;
        line = end + 1;
    }
    if (!found)
        puts("[-] No matching persistent recovery files were found.");
    return found ? 0 : 1;
}

const psvr2_shell_command psvr2_shell_persistence_commands[] = {
    {"stage1_install", cmd_stage1_install,
     "stage1_install <local.ko> [--confirm HASH] - test and persist", true},
    {"stage1_verify", cmd_stage1_verify,
     "stage1_verify [local.ko] - read-only persistent hash check", true},
    {"stage1_rollback", cmd_stage1_rollback,
     "stage1_rollback <backup-sha256> [--confirm HASH]", true},
    {"persist_backups", cmd_persist_backups,
     "persist_backups [filename] - list recovery files", true},
    {"persist", cmd_persist,
     "persist <explicit-/tmp-file> [--confirm HASH]", true},
    {"unpersist", cmd_unpersist,
     "unpersist <explicit-file> [--confirm HASH]", true}
};

const size_t psvr2_shell_persistence_command_count =
    PSVR2_ARRAY_LEN(psvr2_shell_persistence_commands);
