/* Shared application interface. Kernel payloads live in display/abi.h. */
#ifndef OPEN_VRHMD_H
#define OPEN_VRHMD_H

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio/audio.h"
#include "core/thermal.h"
#include "display/abi.h"
#include "render/gpu.h"

extern int fd_dsi;
extern int fd_mmsys;
extern int fd_dscenc;
extern int fd_mutex;
extern int fd_lhc;
extern int fd_rdma;
extern int fd_slicer;
extern int fd_ion;
extern int fd_dprx;

/* Capture errno before formatting so diagnostics report the failing operation. */
#define LOG(fmt, ...) \
  do { \
    fprintf(stderr, "[open_vrhmd] " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
  } while (0)
#define ERR(fmt, ...) \
  do { \
    int saved_errno_ = errno; \
    fprintf(stderr, "[open_vrhmd] ERR: " fmt " (errno=%d: %s)\n", \
            ##__VA_ARGS__, saved_errno_, strerror(saved_errno_)); \
    fflush(stderr); \
  } while (0)

/* core/device.c */
/* Process-scoped ownership: fan children do not inherit POSIX record locks. */
int application_lock_acquire(int stop_existing);
void application_lock_release(void);
extern volatile sig_atomic_t application_stop_requested;
int open_devices(void);
void close_devices(void);
void auto_takeover(int idx);

/* core/devmem.c */
uint32_t devmem_read32(int fd_mem, uint64_t phys_addr);
void devmem_write32(int fd_mem, uint64_t phys_addr, uint32_t val);

/* display/display.c */
int cmd_ungate(void);
int cmd_go(uint8_t r, uint8_t g, uint8_t b, int pattern);
int cmd_stop(void);

extern int fb_share_fd;
extern int ion_handle_id;

/* render/color.c */
void hsv2rgb(int h, int s, int v, uint8_t *ro, uint8_t *go, uint8_t *bo);

extern int g_hardware_powered_on;
void perform_tuning(void);
int cmd_info(void);

#endif
