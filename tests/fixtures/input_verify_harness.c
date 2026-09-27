/* Exercise route selection without opening a device or procfs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static int simulated_open(const char *path, int flags, ...) {
    if (!strcmp(path, "/proc/stage3")) {
        if (getenv("VERIFY_FAIL_PROC")) { errno = ENOENT; return -1; }
        return flags == O_WRONLY ? 10 : -1;
    }
    if (!strcmp(path, "/dev/fast_input")) {
        fputs("DEVICE_OPEN\n", stderr);
        if (getenv("VERIFY_FAIL_DEVICE")) { errno = ENOENT; return -1; }
        return flags == O_RDONLY ? 11 : -1;
    }
    errno = ENOENT;
    return -1;
}

static ssize_t simulated_write(int fd, const void *data, size_t length) {
    static int interrupted;
    if (fd != 10) { errno = EBADF; return -1; }
    if (getenv("VERIFY_EINTR_WRITE") && !interrupted++) {
        errno = EINTR;
        return -1;
    }
    fprintf(stderr, "ROUTE=%.*s\n", (int)length, (const char *)data);
    return (ssize_t)length - (getenv("VERIFY_SHORT_WRITE") ? 1 : 0);
}

static int simulated_close(int fd) { (void)fd; return 0; }
static int simulated_usleep(useconds_t delay) { (void)delay; return 0; }
static ssize_t simulated_read(int fd, void *data, size_t length) {
    (void)fd; (void)data; (void)length;
    if (getenv("VERIFY_FAIL_READ")) { errno = EIO; return -1; }
    return 0;
}

#define open simulated_open
#define write simulated_write
#define close simulated_close
#define usleep simulated_usleep
#define read simulated_read
#include "../../target/psvr2/tools/input_verify.c"
