/*
 * input_verify.c — PSVR2 Controller Verification Tool
 * Decodes 16-byte packets from /dev/fast_input and prints them.
 *
 * Automatically triggers input takeover via /proc/stage3 if needed.
 *
 * Packet format (16 bytes, little-endian):
 *   [0:2]   'CT'        header
 *   [2:4]   int16 lx    left stick X
 *   [4:6]   int16 ly    left stick Y
 *   [6:8]   int16 rx    right stick X
 *   [8:10]  int16 ry    right stick Y
 *   [10:14] uint32 btns button bitmask
 *   [14:16] padding     (DMA alignment)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define DEV_PATH      "/dev/fast_input"
#define PROC_PATH     "/proc/stage3"
#define PACKET_SIZE   16

/* Default data endpoint to takeover (can be overridden via argv[1]) */
#define DEFAULT_EP    2

struct controller_packet {
    char header[2];      /* 'CT' */
    int16_t lx, ly;
    int16_t rx, ry;
    uint32_t buttons;
    uint8_t pad[2];      /* DMA alignment padding */
} __attribute__((packed));

/* Attempt to trigger input takeover via /proc/stage3 */
static int trigger_input_takeover(int ep_idx)
{
    int fd;
    char cmd[32];
    int n;

    fd = open(PROC_PATH, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Warning: Could not open %s: %s\n",
                PROC_PATH, strerror(errno));
        return -1;
    }

    n = snprintf(cmd, sizeof(cmd), "input %d", ep_idx);
    if (write(fd, cmd, n) < 0) {
        fprintf(stderr, "Warning: Could not write to %s: %s\n",
                PROC_PATH, strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    fprintf(stderr, "[*] Sent '%s' to %s\n", cmd, PROC_PATH);

    /* Give the kernel module time to set up /dev/fast_input */
    usleep(500000); /* 500ms */
    return 0;
}

/* Button name lookup table (matches PSVR2 bitmask order) */
static const char *btn_names[] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR",
    "Sel", "Strt", "UP", "DWN", "LFT", "RGT", "L3", "R3"
};

int main(int argc, char **argv)
{
    int fd;
    struct controller_packet pkt;
    ssize_t n;
    int ep_idx = DEFAULT_EP;
    uint32_t pps_count = 0;
    struct timespec ts_start, ts_now;
    int first = 1;

    /* Optional: specify endpoint index */
    if (argc > 1) {
        ep_idx = atoi(argv[1]);
        if (ep_idx < 1 || ep_idx > 9) {
            fprintf(stderr, "Usage: %s [endpoint_idx 1-9] (default: %d)\n",
                    argv[0], DEFAULT_EP);
            return 1;
        }
    }

    /* Try to open /dev/fast_input first — if it fails, trigger takeover */
    fd = open(DEV_PATH, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[*] %s not ready, triggering input takeover on ep%d...\n",
                DEV_PATH, ep_idx);
        trigger_input_takeover(ep_idx);

        fd = open(DEV_PATH, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "Error: Could not open %s after takeover: %s\n",
                    DEV_PATH, strerror(errno));
            fprintf(stderr, "Make sure the kernel module is loaded.\n");
            return 1;
        }
    }

    printf("Listening for controller data on %s (ep%d)...\n", DEV_PATH, ep_idx);
    printf("Format: [LX, LY] [RX, RY] | Buttons | PPS\n");
    printf("------------------------------------------\n");

    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (1) {
        n = read(fd, &pkt, sizeof(pkt));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            perror("read");
            break;
        }

        if (n >= 14) { /* Accept both 14 and 16 byte packets */
            if (pkt.header[0] == 'C' && pkt.header[1] == 'T') {
                pps_count++;

                /* Calculate PPS every second */
                clock_gettime(CLOCK_MONOTONIC, &ts_now);
                long elapsed_ms = (ts_now.tv_sec - ts_start.tv_sec) * 1000 +
                                  (ts_now.tv_nsec - ts_start.tv_nsec) / 1000000;

                /* Print active buttons as names */
                char btn_str[128] = "";
                int btn_pos = 0;
                for (int i = 0; i < 16; i++) {
                    if (pkt.buttons & (1u << i)) {
                        if (btn_pos > 0)
                            btn_pos += snprintf(btn_str + btn_pos,
                                                sizeof(btn_str) - btn_pos, "+");
                        btn_pos += snprintf(btn_str + btn_pos,
                                            sizeof(btn_str) - btn_pos,
                                            "%s", btn_names[i]);
                    }
                }
                if (btn_pos == 0) strcpy(btn_str, "---");

                if (elapsed_ms >= 1000 || first) {
                    uint32_t pps = first ? 0 : pps_count;
                    printf("\r[%6d,%6d] [%6d,%6d] | %-20s | %4u pps",
                        pkt.lx, pkt.ly, pkt.rx, pkt.ry, btn_str, pps);
                    fflush(stdout);
                    pps_count = 0;
                    ts_start = ts_now;
                    first = 0;
                } else {
                    printf("\r[%6d,%6d] [%6d,%6d] | %-20s |",
                        pkt.lx, pkt.ly, pkt.rx, pkt.ry, btn_str);
                    fflush(stdout);
                }
            } else {
                printf("\nWarning: Invalid header: 0x%02x 0x%02x\n",
                       (unsigned char)pkt.header[0],
                       (unsigned char)pkt.header[1]);
            }
        }
    }

    close(fd);
    return 0;
}
