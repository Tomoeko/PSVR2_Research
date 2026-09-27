#include "shell_internal.h"

#include "psvr2/audit.h"
#include "psvr2/tasks.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool print_task_cb(void *opaque, uint64_t task,
                          uint64_t mm, const char *name) {
    (void)opaque;
    printf("%016llx  %016llx  %s\n",
           (unsigned long long)task, (unsigned long long)mm, name);
    return true;
}

static int cmd_ps(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    puts("TASK ADDRESS       MM ADDRESS         COMM");
    return psvr2_tasks_foreach(
               shell->runtime->krw, print_task_cb, NULL) ? 0 : 1;
}

static int cmd_hexdump(psvr2_shell *shell, int argc, char **argv) {
    uint64_t address, length = 256;
    if (argc < 2 || !psvr2_parse_u64(argv[1], &address) ||
        (argc > 2 && !psvr2_parse_u64(argv[2], &length)) ||
        length > 16U * 1024U * 1024U || length > SIZE_MAX) {
        puts("Usage: hexdump <address> [length]");
        return 1;
    }
    uint8_t *data = malloc((size_t)length);
    if (!data ||
        !psvr2_krw_read(
            shell->runtime->krw, address, data, (size_t)length)) {
        free(data);
        return 1;
    }
    psvr2_hexdump(stdout, address, data, (size_t)length);
    free(data);
    return 0;
}

static bool local_output_exists(const char *path) {
    struct stat metadata;
    return path && lstat(path, &metadata) == 0;
}

typedef bool (*memory_capture_read_fn)(
    void *opaque, uint64_t address, void *buffer, size_t length);

static bool capture_kernel_memory(
    void *opaque, uint64_t address, void *buffer, size_t length) {
    return psvr2_krw_read(opaque, address, buffer, length);
}

typedef struct {
    psvr2_krw *krw;
    uint64_t pgd;
} user_capture_context;

static bool capture_user_memory(
    void *opaque, uint64_t address, void *buffer, size_t length) {
    user_capture_context *context = opaque;
    return psvr2_user_read(
        context->krw, context->pgd, address, buffer, length);
}

static int capture_memory_to_file(
    uint64_t address, uint64_t length, const char *path,
    const char *address_kind, memory_capture_read_fn read_memory,
    void *opaque) {
    const size_t chunk_size = 4096;
    if (!path || strlen(path) > SIZE_MAX - sizeof(".partial"))
        return 1;
    size_t partial_length = strlen(path) + sizeof(".partial");
    char *partial = malloc(partial_length);
    uint8_t *buffer = malloc(chunk_size);
    if (!partial || !buffer) {
        free(buffer);
        free(partial);
        return 1;
    }
    snprintf(partial, partial_length, "%s.partial", path);
    if (local_output_exists(path) || local_output_exists(partial)) {
        puts("[-] Refusing to overwrite an output or partial file.");
        free(buffer);
        free(partial);
        return 1;
    }

    int open_flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    open_flags |= O_NOFOLLOW;
#endif
    int descriptor = open(partial, open_flags, 0600);
    if (descriptor < 0) {
        fprintf(stderr, "[-] Could not create %s: %s\n",
                partial, strerror(errno));
        free(buffer);
        free(partial);
        return 1;
    }
    FILE *file = fdopen(descriptor, "wb");
    if (!file) {
        int saved_errno = errno;
        close(descriptor);
        (void)unlink(partial);
        fprintf(stderr, "[-] Could not open %s as a stream: %s\n",
                partial, strerror(saved_errno));
        free(buffer);
        free(partial);
        return 1;
    }

    uint64_t completed = 0;
    bool ok = true;
    while (completed < length) {
        size_t amount = (size_t)psvr2_min_u64(
            chunk_size, length - completed);
        if (!read_memory(
                opaque, address + completed, buffer, amount) ||
            fwrite(buffer, 1, amount, file) != amount) {
            ok = false;
            break;
        }
        completed += amount;
    }
    if (fclose(file) != 0) ok = false;

    if (!ok) {
        fprintf(stderr,
                "[-] Memory capture failed after %llu bytes; "
                "partial output retained at %s.\n",
                (unsigned long long)completed, partial);
        free(buffer);
        free(partial);
        return 1;
    }
    /* link(2) is an atomic, no-replace finalization in the same directory. */
    if (link(partial, path) != 0) {
        fprintf(stderr, "[-] Could not finalize %s: %s\n",
                path, strerror(errno));
        free(buffer);
        free(partial);
        return 1;
    }
    if (unlink(partial) != 0)
        fprintf(stderr, "[!] Saved %s, but could not remove duplicate %s: %s\n",
                path, partial, strerror(errno));

    printf("[+] Saved %llu bytes from %s0x%016llx to %s\n",
           (unsigned long long)length,
           address_kind, (unsigned long long)address, path);
    free(buffer);
    free(partial);
    return 0;
}

static bool capture_arguments(
    int argc, char **argv, int address_index,
    uint64_t *address, uint64_t *length) {
    const uint64_t maximum_length = 16U * 1024U * 1024U;
    return argc == address_index + 3 &&
           psvr2_parse_u64(argv[address_index], address) &&
           psvr2_parse_u64(argv[address_index + 1], length) &&
           *length && *length <= maximum_length && *length <= SIZE_MAX &&
           *address <= UINT64_MAX - (*length - 1U);
}

static int cmd_dumpmem(psvr2_shell *shell, int argc, char **argv) {
    uint64_t address, length;
    if (!capture_arguments(argc, argv, 1, &address, &length)) {
        puts("Usage: dumpmem <address> <length> <host-file>");
        return 1;
    }
    return capture_memory_to_file(
        address, length, argv[3], "", capture_kernel_memory,
        shell->runtime->krw);
}

static int cmd_dumpuser(psvr2_shell *shell, int argc, char **argv) {
    uint64_t address, length;
    if (!capture_arguments(argc, argv, 2, &address, &length)) {
        puts("Usage: dumpuser <task-address|comm> <user-va> "
             "<length> <host-file>");
        return 1;
    }

    uint64_t task = 0;
    if (!psvr2_parse_u64(argv[1], &task))
        task = psvr2_task_find(shell->runtime->krw, argv[1]);
    const psvr2_firmware_profile *profile =
        &shell->runtime->krw->ex->constants->fw;
    uint64_t mm = task ? psvr2_krw_read_ptr(
        shell->runtime->krw, task + profile->task_mm_offset) : 0;
    user_capture_context context = {
        .krw = shell->runtime->krw,
        .pgd = mm ? psvr2_krw_read_ptr(
            shell->runtime->krw, mm + profile->mm_pgd_offset) : 0
    };
    if (!task || !mm || !context.pgd) {
        fprintf(stderr, "[-] Could not resolve user page tables for %s.\n",
                argv[1]);
        return 1;
    }
    printf("[*] Task 0x%016llx, PGD 0x%016llx\n",
           (unsigned long long)task,
           (unsigned long long)context.pgd);
    return capture_memory_to_file(
        address, length, argv[4], "user ", capture_user_memory,
        &context);
}

static int cmd_readptr(psvr2_shell *shell, int argc, char **argv) {
    uint64_t address;
    if (argc != 2 || !psvr2_parse_u64(argv[1], &address)) return 1;
    printf("0x%016llx\n",
           (unsigned long long)psvr2_krw_read_ptr(
               shell->runtime->krw, address));
    return 0;
}

static int cmd_write(psvr2_shell *shell, int argc, char **argv) {
    uint64_t address, value;
    if (argc != 3 || !psvr2_parse_u64(argv[1], &address) ||
        !psvr2_parse_u64(argv[2], &value))
        return 1;
    return psvr2_krw_write_u64_slow(
               shell->runtime->krw, address, value) ? 0 : 1;
}

static int cmd_fwrite(psvr2_shell *shell, int argc, char **argv) {
    uint64_t address, value;
    if (argc != 3 || !psvr2_parse_u64(argv[1], &address) ||
        !psvr2_parse_u64(argv[2], &value))
        return 1;
    return psvr2_krw_write_u64_fast(
               shell->runtime->krw, address, value) ? 0 : 1;
}

static int cmd_testwrite(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return psvr2_krw_kernel_execution_probe(
               shell->runtime->krw, true) ? 0 : 1;
}

static int cmd_write8(psvr2_shell *shell, int argc, char **argv) {
    uint64_t address, value;
    if (argc != 3 || !psvr2_parse_u64(argv[1], &address) ||
        !psvr2_parse_u64(argv[2], &value) || value > UINT8_MAX)
        return 1;
    return psvr2_krw_write_byte(
               shell->runtime->krw, address, (uint8_t)value) ? 0 : 1;
}

static int cmd_writebuf(psvr2_shell *shell, int argc, char **argv) {
    uint64_t address;
    psvr2_buffer data = {0};
    if (argc != 3 || !psvr2_parse_u64(argv[1], &address) ||
        !psvr2_decode_hex(argv[2], &data)) {
        psvr2_buffer_free(&data);
        return 1;
    }
    bool ok = psvr2_krw_write_buffer(
        shell->runtime->krw, address, data.data, data.len, true);
    psvr2_buffer_free(&data);
    return ok ? 0 : 1;
}

static int cmd_kpte(psvr2_shell *shell, int argc, char **argv) {
    uint64_t virtual_address;
    psvr2_pte pte;
    if (argc != 2 ||
        !psvr2_parse_u64(argv[1], &virtual_address) ||
        !psvr2_get_pte(shell->runtime->krw, virtual_address, &pte)) {
        puts("unmapped");
        return 1;
    }
    char permissions[4], detail[128];
    psvr2_decode_pte(
        pte.value, pte.level, permissions, detail, sizeof(detail));
    printf("entry=0x%llx value=0x%llx %s %s\n",
           (unsigned long long)pte.address,
           (unsigned long long)pte.value, permissions, detail);
    return 0;
}

static int cmd_kmap(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    const psvr2_firmware_profile *profile =
        &shell->runtime->krw->ex->constants->fw;
    const struct {
        const char *name;
        uint64_t start;
        uint64_t end;
        uint64_t step;
    } ranges[] = {
        {"kernel", profile->kernel_map_start,
         profile->kernel_map_end, 0x200000},
        {"BSS", profile->bss_map_start,
         profile->bss_map_end, 0x1000}
    };
    for (unsigned range = 0; range < PSVR2_ARRAY_LEN(ranges); ++range) {
        printf("%s:\n", ranges[range].name);
        for (uint64_t address = ranges[range].start;
             address < ranges[range].end;
             address += ranges[range].step) {
            psvr2_pte pte;
            if (!psvr2_get_pte(shell->runtime->krw, address, &pte))
                continue;
            char permissions[4], detail[80];
            psvr2_decode_pte(
                pte.value, pte.level,
                permissions, detail, sizeof(detail));
            printf("  %016llx %s %s\n",
                   (unsigned long long)address, permissions, detail);
        }
    }
    return 0;
}

static uint64_t page_table_phys_to_virt(
    const psvr2_firmware_profile *profile, uint64_t physical) {
    return physical - profile->phys_offset +
           profile->page_offset_base;
}

static uint64_t kernel_canonical_address(uint64_t low_39_bits) {
    return UINT64_C(0xffffff8000000000) | low_39_bits;
}

static bool physical_in_mapping(uint64_t physical, uint64_t base,
                                uint64_t size) {
    return physical >= base && physical - base < size;
}

static int cmd_physmap(psvr2_shell *shell, int argc, char **argv) {
    uint64_t physical;
    uint64_t pgd[512], pmd[512], pte[512];
    unsigned matches = 0;

    const psvr2_firmware_profile *profile =
        &shell->runtime->krw->ex->constants->fw;
    if (argc != 2 || !psvr2_parse_u64(argv[1], &physical))
        return 1;
    if (!psvr2_krw_read(
            shell->runtime->krw, profile->swapper_pg_dir,
                        pgd, sizeof(pgd)))
        return 1;

    for (uint64_t pgd_index = 0; pgd_index < 512; ++pgd_index) {
        uint64_t pgd_value = pgd[pgd_index];
        if (!(pgd_value & 1))
            continue;
        if ((pgd_value & 3) == 1) {
            uint64_t base = pgd_value & profile->phys_mask &
                            ~UINT64_C(0x3fffffff);
            if (physical_in_mapping(physical, base, UINT64_C(0x40000000))) {
                uint64_t va = kernel_canonical_address(
                    (pgd_index << 30) + physical - base);
                printf("%016llx L1_BLOCK\n", (unsigned long long)va);
                ++matches;
            }
            continue;
        }

        uint64_t pmd_address = page_table_phys_to_virt(
            profile, pgd_value & profile->phys_mask);
        if (!psvr2_krw_read(shell->runtime->krw, pmd_address,
                            pmd, sizeof(pmd)))
            return 1;
        for (uint64_t pmd_index = 0; pmd_index < 512; ++pmd_index) {
            uint64_t pmd_value = pmd[pmd_index];
            if (!(pmd_value & 1))
                continue;
            if ((pmd_value & 3) == 1) {
                uint64_t base = pmd_value & profile->phys_mask &
                                ~UINT64_C(0x1fffff);
                if (physical_in_mapping(
                        physical, base, UINT64_C(0x200000))) {
                    uint64_t va = kernel_canonical_address(
                        (pgd_index << 30) | (pmd_index << 21));
                    va += physical - base;
                    printf("%016llx L2_BLOCK\n",
                           (unsigned long long)va);
                    ++matches;
                }
                continue;
            }

            uint64_t pte_address = page_table_phys_to_virt(
                profile, pmd_value & profile->phys_mask);
            if (!psvr2_krw_read(shell->runtime->krw, pte_address,
                                pte, sizeof(pte)))
                return 1;
            for (uint64_t pte_index = 0; pte_index < 512; ++pte_index) {
                uint64_t pte_value = pte[pte_index];
                if (!(pte_value & 1))
                    continue;
                uint64_t base =
                    pte_value & profile->phys_mask;
                if (!physical_in_mapping(physical, base, UINT64_C(0x1000)))
                    continue;
                uint64_t va = kernel_canonical_address(
                    (pgd_index << 30) | (pmd_index << 21) |
                    (pte_index << 12));
                va += physical - base;
                printf("%016llx L3_PAGE\n", (unsigned long long)va);
                ++matches;
            }
        }
    }
    if (!matches)
        puts("unmapped");
    return matches ? 0 : 1;
}

static int cmd_vma(psvr2_shell *shell, int argc, char **argv) {
    uint64_t task;
    if (argc != 2 || !psvr2_parse_u64(argv[1], &task)) return 1;
    const psvr2_firmware_profile *profile =
        &shell->runtime->krw->ex->constants->fw;
    uint64_t mm = psvr2_krw_read_ptr(
        shell->runtime->krw, task + profile->task_mm_offset);
    uint64_t current =
        mm ? psvr2_krw_read_ptr(shell->runtime->krw, mm) : 0;
    uint64_t seen[500];
    size_t count = 0;
    puts("START              END                SIZE       FLAGS");
    while (current && count < PSVR2_ARRAY_LEN(seen)) {
        for (size_t i = 0; i < count; ++i)
            if (seen[i] == current) return 0;
        seen[count++] = current;
        uint64_t start =
            psvr2_krw_read_ptr(shell->runtime->krw, current);
        uint64_t end =
            psvr2_krw_read_ptr(shell->runtime->krw, current + 8);
        uint64_t next =
            psvr2_krw_read_ptr(shell->runtime->krw, current + 0x10);
        uint64_t flags =
            psvr2_krw_read_ptr(shell->runtime->krw, current + 0x50);
        printf("%016llx  %016llx  %08llx   %c%c%c\n",
               (unsigned long long)start, (unsigned long long)end,
               (unsigned long long)(end - start),
               flags & 1 ? 'r' : '-', flags & 2 ? 'w' : '-',
               flags & 4 ? 'x' : '-');
        current = next;
    }
    return count ? 0 : 1;
}

static int cmd_base(psvr2_shell *shell, int argc, char **argv) {
    uint64_t task, virtual_address;
    if (argc != 3 || !psvr2_parse_u64(argv[1], &task) ||
        !psvr2_parse_u64(argv[2], &virtual_address))
        return 1;
    const psvr2_firmware_profile *profile =
        &shell->runtime->krw->ex->constants->fw;
    uint64_t mm = psvr2_krw_read_ptr(
        shell->runtime->krw, task + profile->task_mm_offset);
    uint64_t pgd = mm ? psvr2_krw_read_ptr(
        shell->runtime->krw, mm + profile->mm_pgd_offset) : 0;
    uint64_t kernel_address =
        pgd ? psvr2_user_ptwalk(
                  shell->runtime->krw, pgd, virtual_address) : 0;
    if (!kernel_address) return 1;
    printf("PGD: 0x%llx\nKernel VA: 0x%llx\n",
           (unsigned long long)pgd,
           (unsigned long long)kernel_address);
    return 0;
}

static bool patch_process(psvr2_shell *shell, const char *process,
                          uint64_t offset, const uint8_t *data,
                          size_t length) {
    uint64_t task =
        psvr2_task_find(shell->runtime->krw, process);
    const psvr2_firmware_profile *profile =
        &shell->runtime->krw->ex->constants->fw;
    uint64_t mm =
        task ? psvr2_krw_read_ptr(
                   shell->runtime->krw,
                   task + profile->task_mm_offset) : 0;
    uint64_t mmap =
        mm ? psvr2_krw_read_ptr(shell->runtime->krw, mm) : 0;
    uint64_t base =
        mmap ? psvr2_krw_read_ptr(shell->runtime->krw, mmap) : 0;
    uint64_t pgd = mm ? psvr2_krw_read_ptr(
        shell->runtime->krw, mm + profile->mm_pgd_offset) : 0;
    if (!base || !pgd) return false;

    for (size_t i = 0; i < length; ++i) {
        uint64_t address = psvr2_user_ptwalk(
            shell->runtime->krw, pgd, base + offset + i);
        uint8_t current;
        if (!address ||
            !psvr2_krw_read(
                shell->runtime->krw, address, &current, 1))
            return false;
        if (current != data[i] &&
            !psvr2_krw_write_byte(
                shell->runtime->krw, address, data[i]))
            return false;
    }
    return true;
}

static bool patch_process_wait(
    psvr2_shell *shell, const char *process,
    uint64_t offset, const uint8_t *data,
    size_t length, double timeout)
{
    double deadline = psvr2_now() + timeout;
    do {
        if (patch_process(
                shell, process, offset, data, length))
            return true;
        psvr2_sleep_ms(250);
    } while (psvr2_now() < deadline);
    return false;
}

static int cmd_patch(psvr2_shell *shell, int argc, char **argv) {
    uint64_t offset;
    psvr2_buffer data = {0};
    if (argc != 4 || !psvr2_parse_u64(argv[2], &offset) ||
        !psvr2_decode_hex(argv[3], &data)) {
        psvr2_buffer_free(&data);
        return 1;
    }
    bool ok = patch_process(
        shell, argv[1], offset, data.data, data.len);
    psvr2_buffer_free(&data);
    puts(ok ? "[+] Process memory patched."
            : "[-] Process patch failed.");
    return ok ? 0 : 1;
}

static int cmd_scan_bss(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    psvr2_safety_report report;
    return psvr2_memory_safety_audit(
               shell->runtime, &report) ? 0 : 1;
}

static bool print_module(
    void *opaque, uint64_t base, const char *name) {
    psvr2_krw *krw = opaque;
    psvr2_module_layout layout = {0};
    (void)psvr2_module_core_layout(krw, base, &layout);
    printf("%016llx %016llx %8u %s\n",
           (unsigned long long)base,
           (unsigned long long)layout.base,
           layout.size, name);
    return true;
}

static int cmd_modlist(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    psvr2_walk_result result = psvr2_modules_foreach_ex(
        shell->runtime->krw, print_module, shell->runtime->krw);
    if (result != PSVR2_WALK_COMPLETE)
        fprintf(stderr, "module traversal: %s\n",
                psvr2_walk_result_name(result));
    return result == PSVR2_WALK_COMPLETE ? 0 : 1;
}

static int cmd_backdoor(psvr2_shell *shell, int argc, char **argv) {
    if (argc != 2 || strlen(argv[1]) >= 256) return 1;
    size_t length = strlen(argv[1]) + 1;
    if (!psvr2_krw_write_buffer(
            shell->runtime->krw,
            shell->runtime->krw->ex->constants->fw.uevent_helper,
            argv[1], length, true))
        return 1;
    puts("[+] uevent_helper patched.");
    return 0;
}

static int cmd_et(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    const uint8_t zero = 0;
    const psvr2_firmware_profile *profile =
        &shell->runtime->krw->ex->constants->fw;
    puts("[*] Waiting for the live VrhmdMain patch site...");
    if (!profile->vrhmd_host_gate_offset ||
        !patch_process_wait(
            shell, "VrhmdMain",
            profile->vrhmd_host_gate_offset, &zero, 1, 5.0)) {
        puts("[-] Failed to patch the VrhmdMain host-type gate.");
        return 1;
    }
    psvr2_device *device = shell->runtime->krw->ex->usb;
    bool ok = psvr2_device_activate_eye_tracking(device);
    if (ok) {
        const psvr2_endpoint_query query = {
            .interface_number = -1,
            .alternate_setting = -1,
            .direction = LIBUSB_ENDPOINT_IN,
            .transfer_type = LIBUSB_TRANSFER_TYPE_BULK,
            .address = 0x87
        };
        psvr2_endpoint endpoint;
        if (psvr2_device_find_endpoints(
                device, &query, &endpoint, 1) == 1)
            printf("[+] ET activated; EP 0x%02x is on interface %u.\n",
                   endpoint.address, endpoint.interface_number);
        else {
            puts("[!] ET commands completed, but EP 0x87 is absent "
                 "from the active USB configuration.");
            ok = false;
        }
        puts("    Run psvr2_stream_viewer (ET at scale 2 by default).");
    }
    return ok ? 0 : 1;
}

const psvr2_shell_command psvr2_shell_memory_commands[] = {
    {"ps", cmd_ps, "List kernel tasks", false},
    {"hexdump", cmd_hexdump, "hexdump <addr> [length]", false},
    {"dumpmem", cmd_dumpmem,
     "dumpmem <addr> <length> <host-file>", false},
    {"dumpuser", cmd_dumpuser,
     "dumpuser <task|comm> <user-va> <length> <host-file>", false},
    {"readptr", cmd_readptr, "readptr <addr>", false},
    {"write", cmd_write, "write <addr> <u64> — verified STRB write", true},
    {"fwrite", cmd_fwrite, "fwrite <addr> <u64> — fast STR write", true},
    {"testwrite", cmd_testwrite,
     "Run reversible exact-image kernel write probe", true},
    {"write8", cmd_write8, "write8 <addr> <byte>", true},
    {"writebuf", cmd_writebuf, "writebuf <addr> <hex>", true},
    {"kpte", cmd_kpte, "kpte <kernel-va> — walk page tables", false},
    {"kmap", cmd_kmap, "Show kernel memory permissions", false},
    {"physmap", cmd_physmap,
     "physmap <physical-address> — find live kernel mappings", false},
    {"vma", cmd_vma, "vma <task-address> — list VMAs", false},
    {"base", cmd_base, "base <task> <user-va> — resolve kernel VA", false},
    {"patch", cmd_patch, "patch <process> <offset> <hex>", true},
    {"safety_audit", cmd_scan_bss,
     "Run exact-image read-only memory safety audit", false},
    {"scan_bss", cmd_scan_bss,
     "Alias for safety_audit (zero runs are not certified workspaces)", false},
    {"modlist", cmd_modlist, "List loaded kernel modules", false},
    {"backdoor", cmd_backdoor, "backdoor <path> — patch uevent_helper", true},
    {"et", cmd_et, "Patch and activate eye tracking", true}
};

const size_t psvr2_shell_memory_command_count =
    PSVR2_ARRAY_LEN(psvr2_shell_memory_commands);
