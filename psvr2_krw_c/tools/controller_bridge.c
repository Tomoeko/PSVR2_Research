#include "psvr2/device.h"

#include <errno.h>
#include <fcntl.h>
#include <libusb.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define CONTROLLER_VID 0x0079
#define CONTROLLER_PID 0x0122

static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }

static int serial_open(const char *path) {
    int fd = open(path, O_WRONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios tty;
    if (tcgetattr(fd, &tty)) { close(fd); return -1; }
    cfmakeraw(&tty);
#ifdef B921600
    cfsetispeed(&tty, B921600); cfsetospeed(&tty, B921600);
#else
    cfsetispeed(&tty, B115200); cfsetospeed(&tty, B115200);
#endif
    tty.c_cflag |= CLOCAL | CREAD;
    if (tcsetattr(fd, TCSANOW, &tty)) { close(fd); return -1; }
    return fd;
}

static bool find_interrupt_in(libusb_device_handle *handle, int *iface_out,
                              uint8_t *endpoint_out) {
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(libusb_get_device(handle), &cfg)) return false;
    const psvr2_endpoint_query query = {
        .interface_number = -1,
        .alternate_setting = -1,
        .direction = LIBUSB_ENDPOINT_IN,
        .transfer_type = LIBUSB_TRANSFER_TYPE_INTERRUPT
    };
    psvr2_endpoint endpoint;
    bool found =
        psvr2_usb_find_endpoints(cfg, &query, &endpoint, 1) == 1;
    libusb_free_config_descriptor(cfg);
    if (found) {
        *iface_out = endpoint.interface_number;
        *endpoint_out = endpoint.address;
    }
    return found;
}

static uint32_t button_mask(const uint8_t *d, size_t n) {
    if (n < 7) return 0;
    uint8_t b5 = d[5], b6 = d[6], hat = b5 & 0x0f;
    uint32_t result = 0;
    if (b5 & 0x20) result |= 1U << 0;  /* A */
    if (b5 & 0x10) result |= 1U << 1;  /* B */
    if (b5 & 0x80) result |= 1U << 2;  /* X */
    if (b5 & 0x40) result |= 1U << 3;  /* Y */
    for (unsigned i = 0; i < 8; ++i) if (b6 & (1U << i)) result |= 1U << (i + 4);
    if (hat == 0 || hat == 1 || hat == 7) result |= 1U << 10;
    if (hat == 3 || hat == 4 || hat == 5) result |= 1U << 11;
    if (hat == 5 || hat == 6 || hat == 7) result |= 1U << 12;
    if (hat == 1 || hat == 2 || hat == 3) result |= 1U << 13;
    return result;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <serial-device> [--test]\n", argv[0]);
        return 1;
    }
    bool test = argc == 3 && !strcmp(argv[2], "--test");
    int serial = test ? -1 : serial_open(argv[1]);
    if (!test && serial < 0) { perror(argv[1]); return 1; }
    libusb_context *ctx = NULL;
    libusb_device_handle *controller = NULL;
    int iface = -1;
    uint8_t endpoint = 0;
    bool claimed = false;
    bool detached = false;
    int result = 1;
    if (libusb_init(&ctx)) goto cleanup;
    controller =
        libusb_open_device_with_vid_pid(
            ctx, CONTROLLER_VID, CONTROLLER_PID);
    if (!controller) {
        fputs("GuliKit controller not found.\n", stderr);
        goto cleanup;
    }
    if (!find_interrupt_in(controller, &iface, &endpoint)) {
        fputs("Controller interrupt endpoint not found.\n", stderr);
        goto cleanup;
    }
    if (libusb_kernel_driver_active(controller, iface) == 1) {
        if (libusb_detach_kernel_driver(controller, iface) == 0)
            detached = true;
    }
    if (libusb_claim_interface(controller, iface)) {
        fputs("Could not claim controller interface.\n", stderr);
        goto cleanup;
    }
    claimed = true;
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    printf("[+] Controller EP 0x%02x -> %s\n", endpoint,
           test ? "stdout test mode" : argv[1]);
    uint8_t report[64], packet[16];
    uint64_t packets = 0;
    double epoch = psvr2_now();
    result = 0;
    while (running) {
        int transferred = 0;
        int rc = libusb_interrupt_transfer(controller, endpoint, report,
                                            sizeof(report), &transferred, 100);
        if (rc == LIBUSB_ERROR_TIMEOUT) continue;
        if (rc || transferred < 7) {
            result = 1;
            break;
        }
        int16_t lx = (int16_t)(((int)report[0] - 128) * 256);
        int16_t ly = (int16_t)(((int)report[1] - 128) * 256);
        int16_t rx = (int16_t)(((int)report[2] - 128) * 256);
        int16_t ry = (int16_t)(((int)report[3] - 128) * 256);
        memset(packet, 0, sizeof(packet));
        packet[0] = 'C'; packet[1] = 'T';
        psvr2_store_le16(packet + 2, (uint16_t)lx);
        psvr2_store_le16(packet + 4, (uint16_t)ly);
        psvr2_store_le16(packet + 6, (uint16_t)rx);
        psvr2_store_le16(packet + 8, (uint16_t)ry);
        psvr2_store_le32(packet + 10, button_mask(report, (size_t)transferred));
        if (test) {
            fprintf(stdout, "\rlx=%6d ly=%6d rx=%6d ry=%6d buttons=%08x",
                    lx, ly, rx, ry, psvr2_load_le32(packet + 10));
            fflush(stdout);
        } else {
            ssize_t n = write(serial, packet, sizeof(packet));
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                result = 1;
                break;
            }
        }
        ++packets;
        if (!test && psvr2_now() - epoch >= 1) {
            printf("\r[+] %llu packets/sec", (unsigned long long)packets);
            fflush(stdout); packets = 0; epoch = psvr2_now();
        }
    }
    putchar('\n');
cleanup:
    if (claimed)
        (void)libusb_release_interface(controller, iface);
    if (detached)
        (void)libusb_attach_kernel_driver(controller, iface);
    if (controller) libusb_close(controller);
    if (ctx) libusb_exit(ctx);
    if (serial >= 0) close(serial);
    return result;
}
