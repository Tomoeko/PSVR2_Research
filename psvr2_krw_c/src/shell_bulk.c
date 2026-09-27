#include "shell_internal.h"
#include "stage1_internal.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool psvr2_shell_sync_bulk_generation(psvr2_shell *shell) {
    if (!shell || !shell->runtime || !shell->runtime->krw ||
        !shell->runtime->krw->ex || !shell->runtime->krw->ex->usb)
        return false;
    const uint64_t generation =
        shell->runtime->krw->ex->usb->connection_generation;
    if (shell->bulk_generation == generation)
        return true;
    shell->bulk_interface = 5;
    shell->bulk_in = 0;
    shell->bulk_out = 0;
    shell->bulk_packet = 0;
    shell->bulk_claimed = false;
    shell->bulk_generation = generation;
    return true;
}

bool psvr2_shell_find_bulk_endpoints(psvr2_shell *shell) {
    if (!psvr2_shell_sync_bulk_generation(shell))
        return false;
    psvr2_device *device = shell->runtime->krw->ex->usb;
    if (!device->handle)
        return false;
    if (shell->bulk_in && shell->bulk_out) {
        if (shell->bulk_claimed) return true;
        if (!psvr2_device_claim_interface(
                device, shell->bulk_interface))
            return false;
        shell->bulk_claimed = true;
        return true;
    }
    const psvr2_endpoint_query in_query = {
        .interface_number = 5,
        .alternate_setting = 0,
        .direction = LIBUSB_ENDPOINT_IN,
        .transfer_type = LIBUSB_TRANSFER_TYPE_BULK
    };
    const psvr2_endpoint_query out_query = {
        .interface_number = 5,
        .alternate_setting = 0,
        .direction = LIBUSB_ENDPOINT_OUT,
        .transfer_type = LIBUSB_TRANSFER_TYPE_BULK
    };
    psvr2_endpoint in_endpoint, out_endpoint;
    if (psvr2_device_find_endpoints(
            device, &in_query, &in_endpoint, 1) != 1 ||
        psvr2_device_find_endpoints(
            device, &out_query, &out_endpoint, 1) != 1)
        return false;
    shell->bulk_interface = in_endpoint.interface_number;
    shell->bulk_in = in_endpoint.address;
    shell->bulk_out = out_endpoint.address;
    shell->bulk_packet =
        in_endpoint.packet_size < out_endpoint.packet_size
            ? in_endpoint.packet_size : out_endpoint.packet_size;
    if (shell->bulk_claimed) return true;
    if (!psvr2_device_claim_interface(device, shell->bulk_interface))
        return false;
    shell->bulk_claimed = true;
    return true;
}

static bool upload_name_is_safe(const char *name) {
    if (!name || !*name) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-')
            return false;
    return true;
}

static bool remote_path_is_safe(const char *path) {
    if (!path || path[0] != '/') return false;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
        if (!isalnum(*p) && *p != '/' && *p != '.' &&
            *p != '_' && *p != '-')
            return false;
    return true;
}

static bool append_upload_manifest_entry(
    psvr2_buffer *manifest, const char *path, bool first,
    uint64_t *file_size, uint64_t *total_size) {
    struct stat status;
    if (stat(path, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0)
        return false;
    const char *name = psvr2_basename(path);
    if (!upload_name_is_safe(name) ||
        (uint64_t)status.st_size > UINT64_MAX - *total_size)
        return false;
    char size_text[32];
    int size_length = snprintf(
        size_text, sizeof(size_text), "%llu",
        (unsigned long long)status.st_size);
    if (size_length <= 0 || (size_t)size_length >= sizeof(size_text))
        return false;
    if ((!first && !psvr2_buffer_append(manifest, " ", 1)) ||
        !psvr2_buffer_append(manifest, "/tmp/", 5) ||
        !psvr2_buffer_append(manifest, name, strlen(name)) ||
        !psvr2_buffer_append(manifest, " ", 1) ||
        !psvr2_buffer_append(
            manifest, size_text, (size_t)size_length))
        return false;
    *file_size = (uint64_t)status.st_size;
    *total_size += (uint64_t)status.st_size;
    return true;
}

static bool send_upload_file(
    psvr2_shell *shell, const char *path, uint64_t file_size,
    uint64_t total_size,
    uint64_t *sent, double started) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "[-] Could not open %s: %s\n",
                path, strerror(errno));
        return false;
    }
    uint8_t buffer[64U * 1024U];
    bool ok = true;
    uint64_t remaining = file_size;
    while (remaining) {
        size_t wanted = (size_t)psvr2_min_u64(
            remaining, sizeof(buffer));
        size_t available = fread(buffer, 1, wanted, file);
        if (!available) {
            ok = false;
            break;
        }
        size_t offset = 0;
        while (offset < available) {
            int transferred = 0;
            int chunk = (int)psvr2_min_size(
                16384U, available - offset);
            int error = libusb_bulk_transfer(
                shell->runtime->krw->ex->usb->handle,
                shell->bulk_out, buffer + offset, chunk,
                &transferred, 5000);
            if (error != 0 || transferred <= 0) {
                fprintf(stderr,
                        "\n[-] Bulk OUT 0x%02x failed: %s "
                        "(transferred=%d).\n",
                        shell->bulk_out, libusb_error_name(error),
                        transferred);
                ok = false;
                break;
            }
            offset += (size_t)transferred;
            *sent += (uint64_t)transferred;
            fprintf(stdout, "\r    %llu/%llu bytes (%.0f B/s)",
                    (unsigned long long)*sent,
                    (unsigned long long)total_size,
                    (double)*sent /
                        psvr2_max_double(0.001, psvr2_now() - started));
            fflush(stdout);
        }
        remaining -= available;
        if (!ok || available < wanted) {
            ok = false;
            break;
        }
    }
    if (ferror(file)) ok = false;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool stage1_status(psvr2_shell *shell,
                          char *output, size_t output_size) {
    return psvr2_stage1_read_status(
        shell->runtime, output, output_size);
}

static bool wait_for_upload_completion(
    psvr2_shell *shell, uint64_t expected) {
    double deadline = psvr2_now() + 30.0;
    char status[256];
    do {
        if (!stage1_status(shell, status, sizeof(status)))
            return false;
        if (!strncmp(status, "ERR recv", 8)) {
            fprintf(stderr, "[-] Stage1 upload failed: %s\n", status);
            return false;
        }
        static const char prefix[] = "OK recv done ";
        if (!strncmp(status, prefix, sizeof(prefix) - 1)) {
            const char *value = status + sizeof(prefix) - 1;
            errno = 0;
            char *end = NULL;
            unsigned long long received =
                strtoull(value, &end, 10);
            if (errno || end == value ||
                (uint64_t)received != expected) {
                fprintf(
                    stderr,
                    "[-] Stage1 completed with an unexpected byte count: "
                    "%s\n", status);
                return false;
            }
            return true;
        }
        psvr2_sleep_ms(10);
    } while (psvr2_now() < deadline);
    fputs("[-] Timed out waiting for Stage1 to close and publish "
          "the uploaded file.\n", stderr);
    return false;
}

uint64_t psvr2_shell_stage1_send_size(const char *status) {
    if (!status) return 0;
    if (strncmp(status, "OK send ", 8) != 0 &&
        strncmp(status, "WAIT send ", 10) != 0)
        return 0;
    const char *value = strrchr(status, '/');
    if (value)
        ++value;
    else if (!strncmp(status, "OK send started ", 16) ||
             !strncmp(status, "OK send done ", 13))
        value = strrchr(status, ' ');
    if (!value) return 0;
    while (*value == ' ') ++value;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    return !errno && end != value ? (uint64_t)parsed : 0;
}

static void stop_stage1_send_best_effort(psvr2_shell *shell) {
    int64_t result = -1;
    (void)psvr2_stage1_exec(shell->runtime, "echo 'send_stop' > /proc/stage1",
                            10, &result, NULL);
}

bool psvr2_shell_stop_and_drain_bulk_in(psvr2_shell *shell) {
    int64_t result = -1;
    char status[PSVR2_STAGE1_STATUS_SIZE] = {0};
    if (!shell || !shell->runtime ||
        !psvr2_shell_find_bulk_endpoints(shell) || !shell->bulk_in ||
        !psvr2_stage1_exec(shell->runtime,
                           "echo 'send_prepare' > /proc/stage1",
                           10, &result, NULL) ||
        result != 0 ||
        !psvr2_stage1_read_status(shell->runtime, status, sizeof(status)) ||
        strncmp(status, "OK send_prepare ", 16)) {
        fprintf(stderr, "[-] Stage1 bulk-IN preparation failed: %s\n",
                status[0] ? status : "current send_prepare protocol unavailable");
        return false;
    }

    /*
     * Preparation stops the prior sender and cancels native IN requests
     * before the host drains their controller tail. It cannot prevent a
     * concurrent device producer from queuing more data. A
     * zero-byte timeout is the synchronization point; bounded retries keep a
     * noisy or otherwise unexpected endpoint fail-closed.
     */
    uint8_t buffer[64U * 1024U];
    uint64_t discarded = 0;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        int transferred = 0;
        int error = libusb_bulk_transfer(
            shell->runtime->krw->ex->usb->handle,
            shell->bulk_in, buffer, (int)sizeof(buffer),
            &transferred, 20);
        if (transferred > 0) {
            discarded += (uint64_t)transferred;
            if (error != 0 && error != LIBUSB_ERROR_TIMEOUT) {
                fprintf(stderr,
                        "[-] Bulk-IN drain failed after %llu stale bytes: "
                        "%s.\n",
                        (unsigned long long)discarded,
                        libusb_error_name(error));
                return false;
            }
            continue;
        }
        if (error == LIBUSB_ERROR_TIMEOUT) {
            if (discarded)
                printf("[*] Discarded %llu stale bulk-IN bytes.\n",
                       (unsigned long long)discarded);
            return true;
        }
        if (error == 0)
            continue;
        fprintf(stderr, "[-] Bulk-IN drain failed: %s.\n",
                libusb_error_name(error));
        return false;
    }
    fprintf(stderr,
            "[-] Bulk-IN endpoint did not become idle after discarding "
            "%llu bytes.\n",
            (unsigned long long)discarded);
    return false;
}

int psvr2_shell_fast_upload(psvr2_shell *shell,
                            int argc, char **argv) {
    if (argc < 2 || !psvr2_stage1_discover(shell->runtime) ||
        !psvr2_shell_find_bulk_endpoints(shell) || !shell->bulk_out)
        return 1;

    psvr2_buffer manifest = {0};
    uint64_t total_size = 0;
    size_t file_count = (size_t)(argc - 1);
    uint64_t *file_sizes = calloc(
        file_count, sizeof(*file_sizes));
    if (!file_sizes) return 1;
    for (int index = 1; index < argc; ++index) {
        if (!append_upload_manifest_entry(
                &manifest, argv[index], index == 1,
                &file_sizes[index - 1], &total_size)) {
            fprintf(stderr,
                    "[-] Invalid upload input or unsupported filename: %s\n",
                    argv[index]);
            goto fail;
        }
    }

    if (manifest.len > INT_MAX ||
        manifest.len > SIZE_MAX - 64U)
        goto fail;
    char *command_line = malloc(manifest.len + 64U);
    if (!command_line) goto fail;
    snprintf(command_line, manifest.len + 64U,
             argc == 2
                 ? "echo \"recv %.*s\" > /proc/stage1"
                 : "echo \"recv_multi %.*s\" > /proc/stage1",
             (int)manifest.len, (char *)manifest.data);
    int64_t result = -1;
    if (!psvr2_stage1_exec(
            shell->runtime, command_line, 10, &result, NULL) ||
        result != 0) {
        fprintf(stderr,
                "[-] Stage1 receive setup failed (helper=%lld).\n",
                (long long)result);
        free(command_line);
        goto fail;
    }
    free(command_line);

    char setup_status[256] = {0};
    if (!stage1_status(shell, setup_status, sizeof(setup_status)) ||
        (strncmp(setup_status, "OK recv ", 8) != 0 &&
         strncmp(setup_status, "OK recv_multi ", 14) != 0)) {
        fprintf(stderr, "[-] Stage1 rejected receive setup: %s\n",
                setup_status[0] ? setup_status : "unreadable status");
        goto fail;
    }

    uint64_t sent = 0;
    double start = psvr2_now();
    for (int index = 1; index < argc; ++index)
        if (!send_upload_file(
                shell, argv[index], file_sizes[index - 1],
                total_size, &sent, start))
            goto fail;
    if (sent != total_size) goto fail;
    if (shell->bulk_packet &&
        total_size % shell->bulk_packet == 0) {
        int transferred;
        (void)libusb_bulk_transfer(
            shell->runtime->krw->ex->usb->handle,
            shell->bulk_out, NULL, 0, &transferred, 5000);
    }
    putchar('\n');
    if (!wait_for_upload_completion(shell, total_size))
        goto fail;
    psvr2_buffer_free(&manifest);
    free(file_sizes);
    return 0;

fail:
    psvr2_buffer_free(&manifest);
    free(file_sizes);
    return 1;
}

int psvr2_shell_fast_download(psvr2_shell *shell,
                             int argc, char **argv) {
    if (argc < 2 || argc > 4 ||
        !psvr2_stage1_discover(shell->runtime) ||
        !psvr2_shell_find_bulk_endpoints(shell) || !shell->bulk_in)
        return 1;
    uint64_t size_override = 0;
    if (argc == 4 &&
        !psvr2_parse_u64(argv[3], &size_override))
        return 1;
    if (!remote_path_is_safe(argv[1])) {
        fprintf(stderr,
                "[-] Remote path contains unsupported characters: %s\n",
                argv[1]);
        return 1;
    }

    int64_t result;
    if (!psvr2_shell_stop_and_drain_bulk_in(shell)) {
        return 1;
    }

    char request[1400];
    if (size_override)
        snprintf(request, sizeof(request),
                 "echo 'send %s %llu' > /proc/stage1",
                 argv[1], (unsigned long long)size_override);
    else
        snprintf(request, sizeof(request),
                 "echo 'send %s' > /proc/stage1", argv[1]);
    if (!psvr2_stage1_exec(
            shell->runtime, request, 10, &result, NULL) ||
        result != 0) {
        fprintf(stderr,
                "[-] Stage1 send setup failed (helper=%lld).\n",
                (long long)result);
        stop_stage1_send_best_effort(shell);
        return 1;
    }

    char status[512] = {0};
    uint64_t size = 0;
    double status_deadline = psvr2_now() + 2.0;
    do {
        if (!stage1_status(shell, status, sizeof(status))) {
            stop_stage1_send_best_effort(shell);
            return 1;
        }
        if (!strncmp(status, "ERR send ", 9)) {
            fprintf(stderr, "[-] Stage1 sender rejected the transfer: %s\n",
                    status);
            stop_stage1_send_best_effort(shell);
            return 1;
        }
        size = psvr2_shell_stage1_send_size(status);
    } while (!size && psvr2_now() < status_deadline);
    if (!size) {
        fprintf(stderr,
                "[-] Could not parse Stage1 status: %s\n", status);
        stop_stage1_send_best_effort(shell);
        return 1;
    }
    if (size_override && size != size_override) {
        fprintf(stderr, "[-] Stage1 reported %llu bytes; expected %llu.\n",
                (unsigned long long)size, (unsigned long long)size_override);
        stop_stage1_send_best_effort(shell);
        return 1;
    }

    const char *local =
        argc >= 3 ? argv[2] : psvr2_basename(argv[1]);
    size_t partial_length = strlen(local) + sizeof(".partial");
    char *partial = malloc(partial_length);
    if (!partial) {
        stop_stage1_send_best_effort(shell);
        return 1;
    }
    snprintf(partial, partial_length, "%s.partial", local);
    FILE *file = psvr2_shell_open_output_exclusive(partial);
    if (!file) {
        stop_stage1_send_best_effort(shell);
        free(partial);
        return 1;
    }
    const size_t transfer_buffer_size = 4U * 1024U * 1024U;
    uint8_t *buffer = malloc(transfer_buffer_size);
    if (!buffer) {
        fclose(file);
        stop_stage1_send_best_effort(shell);
        free(partial);
        return 1;
    }
    uint64_t received = 0;
    unsigned zero_length_packets = 0;
    double start = psvr2_now();
    while (received < size) {
        int transferred = 0;
        int wanted = (int)transfer_buffer_size;
        int error = libusb_bulk_transfer(
            shell->runtime->krw->ex->usb->handle,
            shell->bulk_in, buffer, wanted, &transferred, 20);
        if ((!error || error == LIBUSB_ERROR_TIMEOUT) &&
            transferred == 0 && zero_length_packets++ < 256) {
            /*
             * The stock Sony endpoint can emit a ZLP between Stage1's
             * asynchronous "send started" status and its first queued data
             * request. Treat bounded ZLPs as pacing, not EOF.
             */
            char current_status[512] = {0};
            if (!stage1_status(
                    shell, current_status, sizeof(current_status)) ||
                strstr(current_status, "ERR send")) {
                fprintf(stderr,
                        "[-] Stage1 sender failed after a zero-length "
                        "packet: %s\n", current_status);
                goto download_fail;
            }
            continue;
        }
        bool timed_out_with_data =
            error == LIBUSB_ERROR_TIMEOUT && transferred > 0;
        if ((error && !timed_out_with_data) || transferred <= 0) {
            fprintf(stderr,
                    "[-] Bulk IN 0x%02x failed after %llu bytes: %s "
                    "(transferred=%d).\n",
                    shell->bulk_in, (unsigned long long)received,
                    libusb_error_name(error), transferred);
            goto download_fail;
        }
        zero_length_packets = 0;
        size_t accepted = (size_t)psvr2_min_u64(
            (uint64_t)transferred, size - received);
        if (fwrite(buffer, 1, accepted, file) != accepted) {
            fprintf(stderr,
                    "[-] Host write failed after %llu bytes.\n",
                    (unsigned long long)received);
            goto download_fail;
        }
        received += (uint64_t)accepted;
        fprintf(stdout, "\r    %llu/%llu bytes (%.0f B/s)",
                (unsigned long long)received,
                (unsigned long long)size,
                (double)received /
                    psvr2_max_double(0.001, psvr2_now() - start));
        fflush(stdout);
    }
    free(buffer);
    if (fclose(file) != 0) {
        file = NULL;
        fprintf(stderr, "[-] Could not finalize %s.\n", partial);
        goto download_fail_closed;
    }
    file = NULL;
    stop_stage1_send_best_effort(shell);
    if (!psvr2_shell_finalize_output_exclusive(partial, local)) {
        goto download_fail_closed;
    }
    printf("\n[+] Downloaded to %s\n", local);
    free(partial);
    return 0;

download_fail:
    free(buffer);
    if (file) fclose(file);
download_fail_closed:
    stop_stage1_send_best_effort(shell);
    fprintf(stderr, "[-] Partial download retained at %s.\n", partial);
    free(partial);
    return 1;
}

static int cmd_probe_in(psvr2_shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!psvr2_shell_find_bulk_endpoints(shell)) return 1;
    printf("interface=%d IN=0x%02x OUT=0x%02x max_packet=%u\n",
           shell->bulk_interface, shell->bulk_in,
           shell->bulk_out, shell->bulk_packet);
    return shell->bulk_in ? 0 : 1;
}

const psvr2_shell_command psvr2_shell_transfer_commands[] = {
    {"fast_upload", psvr2_shell_fast_upload,
     "fast_upload <files...> — Stage1 bulk OUT", true},
    {"fast_download", psvr2_shell_fast_download,
     "fast_download <remote> [local] [size]", true},
    {"probe_in", cmd_probe_in,
     "Probe Stage1 bulk endpoints", false}
};

const size_t psvr2_shell_transfer_command_count =
    PSVR2_ARRAY_LEN(psvr2_shell_transfer_commands);
