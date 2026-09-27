#include "shell_internal.h"

#include <stdlib.h>
#include <string.h>

static const char *const emmc_partitions[] = {
    "mmcblk0boot0", "mmcblk0boot1",
    "mmcblk0p1", "mmcblk0p2", "mmcblk0p3", "mmcblk0p4",
    "mmcblk0p5", "mmcblk0p6", "mmcblk0p7", "mmcblk0p8",
    "mmcblk0p9", "mmcblk0p10", "mmcblk0p11", "mmcblk0p12",
    "mmcblk0p13", "mmcblk0p14", "mmcblk0p15", "mmcblk0p16",
    "mmcblk0p17", "mmcblk0p18", "mmcblk0p19", "mmcblk0p20",
    "mmcblk0"
};

static uint64_t emmc_device_size(psvr2_shell *shell, const char *remote) {
    char size_command[256];
    snprintf(size_command, sizeof(size_command),
             "/data/modules/busybox blockdev --getsize64 '%s'", remote);
    int64_t result;
    psvr2_buffer output = {0};
    uint64_t device_size = 0;
    if (psvr2_stage1_exec(
            shell->runtime, size_command, 10, &result, &output) &&
        result == 0 && output.data)
        (void)psvr2_parse_u64(
            psvr2_trim((char *)output.data), &device_size);
    psvr2_buffer_free(&output);
    return device_size;
}

static int cmd_emmc(psvr2_shell *shell, int argc, char **argv) {
    if (argc != 2) return 1;
    if (!strcmp(argv[1], "list")) {
        for (size_t index = 0;
             index < PSVR2_ARRAY_LEN(emmc_partitions); ++index)
            puts(emmc_partitions[index]);
        return 0;
    }

    int command_result = 0;
    bool matched = false;
    for (size_t index = 0;
         index < PSVR2_ARRAY_LEN(emmc_partitions); ++index) {
        if (strcmp(argv[1], "all") &&
            strcmp(argv[1], emmc_partitions[index]))
            continue;
        matched = true;
        char remote[128];
        char local[160];
        snprintf(remote, sizeof(remote),
                 "/dev/%s", emmc_partitions[index]);
        snprintf(local, sizeof(local),
                 "%s.img", emmc_partitions[index]);

        uint64_t device_size = emmc_device_size(shell, remote);

        char size_text[32];
        snprintf(size_text, sizeof(size_text), "%llu",
                 (unsigned long long)device_size);
        char *arguments[] = {
            "fast_download", remote, local, size_text
        };
        if (!device_size ||
            psvr2_shell_fast_download(shell, 4, arguments))
            command_result = 1;
        if (strcmp(argv[1], "all")) return command_result;
    }
    if (!matched) {
        fprintf(stderr, "[-] Unknown eMMC target: %s\n", argv[1]);
        return 1;
    }

    return command_result;
}

static bool append_staged_file(
    psvr2_shell *shell, const char *temporary,
    uint64_t expected_size, FILE *destination) {
    uint64_t dentry = psvr2_vfs_resolve_path(
        &shell->vfs, shell->cwd, temporary);
    psvr2_buffer data = {0};
    bool ok = dentry && expected_size <= SIZE_MAX &&
        psvr2_vfs_read_file(
            &shell->vfs, dentry, &data, (size_t)expected_size) &&
        data.len == expected_size &&
        fwrite(data.data, 1, data.len, destination) == data.len;
    psvr2_buffer_free(&data);
    return ok;
}

static bool read_remote_u64(
    psvr2_shell *shell, const char *path, uint64_t *value) {
    char command[384];
    snprintf(command, sizeof(command),
             "/data/modules/busybox cat '%s'", path);
    int64_t result = -1;
    psvr2_buffer output = {0};
    bool ok = psvr2_stage1_exec(
        shell->runtime, command, 10, &result, &output);
    char text[96];
    size_t length = psvr2_min_size(output.len, sizeof(text) - 1);
    if (length) memcpy(text, output.data, length);
    text[length] = '\0';
    ok = ok && result == 0 && psvr2_parse_u64(psvr2_trim(text), value);
    psvr2_buffer_free(&output);
    return ok;
}

static int cmd_emmc_plain(psvr2_shell *shell, int argc, char **argv) {
    const uint64_t chunk_sectors =
        (UINT64_C(16) * 1024 * 1024) / 512;
    const uint64_t aes_switch_address =
        shell->runtime->krw->ex->constants->fw.aes_switch_address;
    const char *temporary = "/tmp/.psvr2_emmc_plain";
    uint64_t absolute_start, requested_sectors;

    if ((argc != 3 && argc != 4) ||
        !psvr2_parse_u64(argv[1], &absolute_start) ||
        !psvr2_parse_u64(argv[2], &requested_sectors) ||
        !requested_sectors ||
        requested_sectors > UINT64_MAX - absolute_start) {
        puts("Usage: emmc_plain <absolute-start-lba> <sector-count> "
             "[local-output]");
        return 1;
    }
    if (shell->runtime->krw->ex->constants->version != PSVR2_FW_0600) {
        puts("[-] emmc_plain is certified only for firmware 06.00.");
        return 1;
    }

    uint8_t aes_enabled = 0;
    if (!psvr2_krw_read(
            shell->runtime->krw, aes_switch_address,
            &aes_enabled, sizeof(aes_enabled)) ||
        aes_enabled != 1) {
        printf("[-] Refusing acquisition: aes_switch_on is 0x%02x, "
               "expected 0x01.\n", aes_enabled);
        return 1;
    }

    uint64_t module_abi, module_firmware, module_read_only;
    uint64_t module_start, module_sectors;
    if (!read_remote_u64(
            shell,
            "/sys/module/msdc_plaintext_reader/parameters/abi_version",
            &module_abi) ||
        !read_remote_u64(
            shell,
            "/sys/module/msdc_plaintext_reader/parameters/firmware_family",
            &module_firmware) ||
        !read_remote_u64(
            shell,
            "/sys/module/msdc_plaintext_reader/parameters/read_only_capability",
            &module_read_only) ||
        !read_remote_u64(
            shell,
            "/sys/module/msdc_plaintext_reader/parameters/effective_start_lba",
            &module_start) ||
        !read_remote_u64(
            shell,
            "/sys/module/msdc_plaintext_reader/parameters/"
                         "effective_sector_count",
            &module_sectors)) {
        puts("[-] A managed msdc_plaintext_reader is not loaded or its "
             "runtime handshake is unavailable.");
        return 1;
    }
    if (module_abi != 1 || module_firmware != 0x0600 || module_read_only != 1 ||
        !module_sectors) {
        printf("[-] Refusing unrecognized reader: abi=%llu "
               "firmware=0x%04llx read_only=%llu sectors=%llu.\n",
               (unsigned long long)module_abi,
               (unsigned long long)module_firmware,
               (unsigned long long)module_read_only,
               (unsigned long long)module_sectors);
        return 1;
    }
    if (absolute_start < module_start) {
        puts("[-] Requested LBA precedes the module window.");
        return 1;
    }
    uint64_t relative_start = absolute_start - module_start;
    if (relative_start >= module_sectors ||
        requested_sectors > module_sectors - relative_start) {
        puts("[-] Requested range exceeds the module window.");
        return 1;
    }

    char local[256], partial[272];
    int local_length;
    if (argc == 4)
        local_length = snprintf(local, sizeof(local), "%s", argv[3]);
    else
        local_length = snprintf(local, sizeof(local),
                                "emmc_plain_%llu_%llu.img",
                                (unsigned long long)absolute_start,
                                (unsigned long long)requested_sectors);
    int partial_length =
        snprintf(partial, sizeof(partial), "%s.partial", local);
    if (local_length < 0 || (size_t)local_length >= sizeof(local) ||
        partial_length < 0 || (size_t)partial_length >= sizeof(partial)) {
        puts("[-] Local output path is too long.");
        return 1;
    }

    FILE *destination = psvr2_shell_open_output_exclusive(partial);
    if (!destination) return 1;
    uint64_t completed = 0;
    bool ok = true;
    while (completed < requested_sectors) {
        uint64_t sectors =
            psvr2_min_u64(chunk_sectors, requested_sectors - completed);
        uint64_t skip = relative_start + completed;
        char command[512];
        snprintf(command, sizeof(command),
                 "/data/modules/busybox dd if=/dev/msdc_plaintext "
                 "of='%s' bs=512 skip=%llu count=%llu",
                 temporary,
                 (unsigned long long)skip,
                 (unsigned long long)sectors);
        int64_t result = -1;
        uint64_t bytes = sectors * 512;
        if (!psvr2_stage1_exec(
                shell->runtime, command, 60, &result, NULL) ||
            result != 0 ||
            !append_staged_file(
                shell, temporary, bytes, destination) ||
            !psvr2_krw_read(
                shell->runtime->krw, aes_switch_address,
                &aes_enabled, sizeof(aes_enabled)) ||
            aes_enabled != 1) {
            ok = false;
            break;
        }
        completed += sectors;
        printf("\r    plaintext %llu/%llu sectors",
               (unsigned long long)completed,
               (unsigned long long)requested_sectors);
        fflush(stdout);
    }
    fclose(destination);

    int64_t cleanup_result;
    (void)psvr2_stage1_exec(
        shell->runtime,
        "/data/modules/busybox rm -f /tmp/.psvr2_emmc_plain",
        10, &cleanup_result, NULL);
    if (!ok) {
        puts("\n[-] Acquisition failed; the partial host file was retained.");
        return 1;
    }
    if (!psvr2_shell_finalize_output_exclusive(partial, local)) {
        puts("\n[-] Could not finalize the host output.");
        return 1;
    }
    printf("\n[+] Saved %llu sectors from absolute LBA %llu to %s\n",
           (unsigned long long)requested_sectors,
           (unsigned long long)absolute_start, local);
    return 0;
}

const psvr2_shell_command psvr2_shell_emmc_commands[] = {
    {"emmc", cmd_emmc,
     "emmc list|all|<partition> — bulk dump", true},
    {"emmc_plain", cmd_emmc_plain,
     "emmc_plain <LBA> <sectors> [local] — read-only post-XTS dump", true}
};

const size_t psvr2_shell_emmc_command_count =
    PSVR2_ARRAY_LEN(psvr2_shell_emmc_commands);
