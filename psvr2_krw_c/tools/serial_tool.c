#include "psvr2/common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int open_serial(const char *path) {
    /*
     * Open before carrier detect is asserted.  macOS can otherwise block in
     * open(2) forever, which prevents us from applying CLOCAL below even
     * though the ACM data endpoints are already active.
     */
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) { close(fd); return -1; }
    cfmakeraw(&tty);
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~(CSTOPB | CRTSCTS);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 10;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) { close(fd); return -1; }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    {
        int modem_bits = TIOCM_DTR | TIOCM_RTS;
        (void)ioctl(fd, TIOCMBIS, &modem_bits);
    }
    return fd;
}

static bool write_all(int fd, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; len -= (size_t)n;
    }
    return true;
}

static char *base64_encode(const uint8_t *data, size_t len, size_t *out_len) {
    if (len > (SIZE_MAX - 4) / 4 * 3) return NULL;
    size_t raw = ((len + 2) / 3) * 4;
    size_t lines = (raw + 75) / 76;
    char *out = malloc(raw + lines + 1);
    if (!out) return NULL;
    size_t src = 0, dst = 0, column = 0;
    while (src < len) {
        size_t remaining = len - src;
        uint32_t a = data[src++];
        uint32_t b = remaining > 1 ? data[src++] : 0;
        uint32_t c = remaining > 2 ? data[src++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[dst++] = b64_table[(triple >> 18) & 63];
        out[dst++] = b64_table[(triple >> 12) & 63];
        out[dst++] = remaining > 1 ? b64_table[(triple >> 6) & 63] : '=';
        out[dst++] = remaining > 2 ? b64_table[triple & 63] : '=';
        column += 4;
        if (column == 76 || src == len) {
            out[dst++] = '\n';
            column = 0;
        }
    }
    out[dst] = '\0';
    *out_len = dst;
    return out;
}

static int b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool base64_decode(const char *text, size_t len, psvr2_buffer *out) {
    uint8_t quartet[4];
    unsigned q = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '=' || b64_value(c) >= 0) quartet[q++] = c;
        else if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        else return false;
        if (q == 4) {
            int a = b64_value(quartet[0]), b = b64_value(quartet[1]);
            int cv = quartet[2] == '=' ? 0 : b64_value(quartet[2]);
            int d = quartet[3] == '=' ? 0 : b64_value(quartet[3]);
            if (a < 0 || b < 0 || cv < 0 || d < 0) return false;
            uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                              ((uint32_t)cv << 6) | (uint32_t)d;
            uint8_t bytes[] = {(uint8_t)(triple >> 16),
                               (uint8_t)(triple >> 8), (uint8_t)triple};
            size_t n = quartet[2] == '=' ? 1 : quartet[3] == '=' ? 2 : 3;
            if (!psvr2_buffer_append(out, bytes, n)) return false;
            q = 0;
        }
    }
    return q == 0;
}

static void drain_serial(int fd) {
    (void)tcflush(fd, TCIFLUSH);
}

static bool read_until(int fd, const char *marker, psvr2_buffer *out,
                       double timeout) {
    double deadline = psvr2_now() + timeout;
    uint8_t buf[4096];
    while (psvr2_now() < deadline) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (!psvr2_buffer_append(out, buf, (size_t)n)) return false;
            char zero = '\0';
            if (!psvr2_buffer_append(out, &zero, 1)) return false;
            --out->len;
            if (strstr((char *)out->data, marker)) return true;
        }
    }
    return false;
}

static bool read_until_occurrences(int fd, const char *marker, unsigned wanted,
                                   psvr2_buffer *out, double timeout) {
    double deadline = psvr2_now() + timeout;
    uint8_t buf[4096];
    while (psvr2_now() < deadline) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) continue;
        if (!psvr2_buffer_append(out, buf, (size_t)n)) return false;
        char zero = '\0';
        if (!psvr2_buffer_append(out, &zero, 1)) return false;
        --out->len;
        unsigned count = 0;
        char *p = (char *)out->data;
        while ((p = strstr(p, marker)) != NULL) { ++count; p += strlen(marker); }
        if (count >= wanted) return true;
    }
    return false;
}

static bool serial_upload(int fd, const char *local, const char *remote_arg) {
    static const char ready_marker[] = "===PSVR2_UPLOAD_READY===";
    static const char done_marker[] = "===PSVR2_UPLOAD_DONE===";
    psvr2_buffer file = {0};
    if (!psvr2_read_file(local, &file)) return false;
    const char *remote = remote_arg ? remote_arg : psvr2_basename(local);
    char path[1200];
    if (remote[0] == '/') snprintf(path, sizeof(path), "%s", remote);
    else snprintf(path, sizeof(path), "/tmp/%s", remote);
    char command[1600];
    int n = snprintf(command, sizeof(command),
        "echo %s;(while read L;do [ \"$L\" = EOF ]&&break;echo \"$L\";"
        "done)|base64 -d>'%s';echo %s\n",
        ready_marker, path, done_marker);
    if (n < 0 || (size_t)n >= sizeof(command)) { psvr2_buffer_free(&file); return false; }
    drain_serial(fd);
    if (!write_all(fd, command, (size_t)n)) { psvr2_buffer_free(&file); return false; }
    psvr2_buffer response = {0};
    if (!read_until_occurrences(fd, ready_marker, 2, &response, 10)) {
        psvr2_buffer_free(&response);
        psvr2_buffer_free(&file);
        return false;
    }
    psvr2_buffer_free(&response);
    size_t encoded_len;
    char *encoded = base64_encode(file.data, file.len, &encoded_len);
    if (!encoded) { psvr2_buffer_free(&file); return false; }
    double start = psvr2_now();
    for (size_t off = 0; off < encoded_len; off += 4096) {
        size_t block = psvr2_min_size(4096, encoded_len - off);
        if (!write_all(fd, encoded + off, block)) {
            free(encoded); psvr2_buffer_free(&file); return false;
        }
        fprintf(stdout, "\r    %zu/%zu encoded bytes (%.0f KiB/s)",
                off + block, encoded_len,
                (off + block) / 1024.0 /
                    psvr2_max_double(0.001, psvr2_now() - start));
        fflush(stdout);
    }
    free(encoded);
    if (!write_all(fd, "EOF\n", 4)) { psvr2_buffer_free(&file); return false; }
    if (!read_until(fd, done_marker, &response, 10)) {
        psvr2_buffer_free(&response);
        psvr2_buffer_free(&file);
        return false;
    }
    psvr2_buffer_free(&response);
    n = snprintf(command, sizeof(command), "echo SIZE$(wc -c < '%s')SIZE\n", path);
    if (n < 0 || !write_all(fd, command, (size_t)n)) {
        psvr2_buffer_free(&file); return false;
    }
    char expected[64];
    snprintf(expected, sizeof(expected), "SIZE%zuSIZE", file.len);
    bool ok = read_until(fd, expected, &response, 10);
    printf("\n%s %s (%zu bytes)\n", ok ? "[+] Uploaded" : "[-] Verification failed:",
           path, file.len);
    psvr2_buffer_free(&response);
    psvr2_buffer_free(&file);
    return ok;
}

static bool valid_b64_line(const char *line, size_t len) {
    if (len < 4) return false;
    for (size_t i = 0; i < len; ++i)
        if (line[i] != '=' && b64_value((unsigned char)line[i]) < 0) return false;
    return true;
}

static bool serial_download(int fd, const char *remote, const char *local) {
    drain_serial(fd);
    char command[1600];
    int n = snprintf(command, sizeof(command),
        "echo ===B64START===;base64 '%s';echo ===B64END===\n", remote);
    if (n < 0 || (size_t)n >= sizeof(command) ||
        !write_all(fd, command, (size_t)n)) return false;
    psvr2_buffer raw = {0};
    if (!read_until_occurrences(fd, "===B64END===", 2, &raw, 300)) {
        psvr2_buffer_free(&raw); return false;
    }
    char *start = strstr((char *)raw.data, "===B64START===");
    char *second_start = start ? strstr(start + 14, "===B64START===") : NULL;
    if (second_start) start = second_start;
    char *end = start ? strstr(start + 14, "===B64END===") : NULL;
    if (!start || !end) { psvr2_buffer_free(&raw); return false; }
    start = strchr(start, '\n');
    if (!start) { psvr2_buffer_free(&raw); return false; }
    ++start;
    psvr2_buffer clean = {0};
    char *line = start;
    while (line < end) {
        char *nl = memchr(line, '\n', (size_t)(end - line));
        char *line_end = nl ? nl : end;
        while (line_end > line && (line_end[-1] == '\r' || line_end[-1] == ' '))
            --line_end;
        size_t line_len = (size_t)(line_end - line);
        if (valid_b64_line(line, line_len)) {
            (void)psvr2_buffer_append(&clean, line, line_len);
            char nlch = '\n';
            (void)psvr2_buffer_append(&clean, &nlch, 1);
        }
        line = nl ? nl + 1 : end;
    }
    psvr2_buffer decoded = {0};
    bool ok = base64_decode((char *)clean.data, clean.len, &decoded) &&
              psvr2_write_file(local, decoded.data, decoded.len);
    if (ok) printf("[+] Downloaded %zu bytes to %s\n", decoded.len, local);
    else puts("[-] Download failed.");
    psvr2_buffer_free(&raw);
    psvr2_buffer_free(&clean);
    psvr2_buffer_free(&decoded);
    return ok;
}

static int shell_mode(int fd) {
    uint8_t buf[4096];
    fd_set readfds;
    struct termios saved_stdin;
    bool restore_stdin = false;
    if (isatty(STDIN_FILENO) &&
        tcgetattr(STDIN_FILENO, &saved_stdin) == 0) {
        struct termios raw_stdin = saved_stdin;
        cfmakeraw(&raw_stdin);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_stdin) == 0)
            restore_stdin = true;
    }
    fputs("[*] Interactive serial shell; press Ctrl-] to disconnect.\n",
          stderr);
    (void)write_all(fd, "\r", 1);

    int result = 0;
    for (;;) {
        FD_ZERO(&readfds); FD_SET(fd, &readfds); FD_SET(STDIN_FILENO, &readfds);
        int maxfd = fd > STDIN_FILENO ? fd : STDIN_FILENO;
        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            result = 1;
            break;
        }
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
            uint8_t *escape = memchr(buf, 0x1d, (size_t)n);
            size_t send = escape
                ? (size_t)(escape - buf) : (size_t)n;
            if (send && !write_all(fd, buf, send)) {
                result = 1;
                break;
            }
            if (escape) break;
        }
        if (FD_ISSET(fd, &readfds)) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0 && !write_all(STDOUT_FILENO, buf, (size_t)n)) {
                result = 1;
                break;
            }
        }
    }
    if (restore_stdin)
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_stdin);
    return result;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
          "Usage: %s <device> shell\n"
          "       %s <device> send <local-file> [remote-path]\n"
          "       %s <device> receive <remote-path> [local-file]\n"
          "       %s <device> command <text>\n",
          argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }
    int fd = open_serial(argv[1]);
    if (fd < 0) { perror(argv[1]); return 1; }
    int result = 0;
    if (!strcmp(argv[2], "shell")) result = shell_mode(fd);
    else if (!strcmp(argv[2], "command") && argc >= 4) {
        char *cmd = malloc(strlen(argv[3]) + 2);
        if (!cmd) result = 1;
        else {
            snprintf(cmd, strlen(argv[3]) + 2, "%s\n", argv[3]);
            result = write_all(fd, cmd, strlen(cmd)) ? 0 : 1;
            free(cmd);
        }
    } else if (!strcmp(argv[2], "send") && argc >= 4) {
        result = serial_upload(fd, argv[3], argc > 4 ? argv[4] : NULL) ? 0 : 1;
    } else if (!strcmp(argv[2], "receive") && argc >= 4) {
        result = serial_download(fd, argv[3],
                  argc > 4 ? argv[4] : psvr2_basename(argv[3])) ? 0 : 1;
    } else result = 1;
    close(fd);
    return result;
}
