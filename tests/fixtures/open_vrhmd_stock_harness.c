/* Synthetic /proc records; never signal real PIDs or open hardware devices. */
#include "../../target/psvr2/tools/open_vrhmd/open_vrhmd.h"

static int signals_sent;
static int stock_reads;
static int teardown_calls;
static int ioctl_calls;

static int mock_open(const char *path, int flags, ...) {
  (void)path;
  (void)flags;
  if (getenv("TEST_CALL_OPEN")) return open("/dev/null", O_RDWR);
  errno = ENOENT;
  return -1;
}

static int mock_kill(pid_t pid, int signal_number) {
  if (signal_number != SIGKILL || pid != 96) abort();
  ++signals_sent;
  if (getenv("TEST_SIGNAL_FAIL")) { errno = EPERM; return -1; }
  if (!getenv("TEST_STOCK_HOLD")) {
    char path[512];
    snprintf(path, sizeof(path), OPEN_VRHMD_PROC_ROOT "/96/exe");
    if (unlink(path) < 0) abort();
  }
  if (ioctl_calls) abort();
  return 0;
}

static ssize_t mock_readlink(const char *path, char *buffer, size_t length) {
  if (strstr(path, "/96/exe") && ++stock_reads == 2 && getenv("TEST_PID_REUSED")) {
    char stat_path[512];
    snprintf(stat_path, sizeof(stat_path), OPEN_VRHMD_PROC_ROOT "/96/stat");
    FILE *file = fopen(stat_path, "w");
    if (!file) abort();
    fprintf(file, "96 (VrhmdMain) S");
    for (int field = 4; field <= 22; ++field)
      fprintf(file, " %d", field == 22 ? 54321 : 0);
    fputc('\n', file);
    fclose(file);
  }
  return readlink(path, buffer, length);
}

static int __attribute__((unused)) mock_ioctl(int fd, unsigned long command, ...) {
  (void)fd; (void)command;
  ++ioctl_calls;
  abort(); /* Takeover must never enter the unsafe kernel STREAMOFF. */
}
#define ioctl mock_ioctl
#define open mock_open
#define kill mock_kill
#define readlink mock_readlink
#include "../../target/psvr2/tools/open_vrhmd/core/device.c"
#undef readlink
#undef kill
#undef open
#undef ioctl

int cmd_stop(void) { ++teardown_calls; return getenv("TEST_STOP_FAIL") ? -1 : 0; }

int main(void) {
  if (!stock_executable_path(OPEN_VRHMD_STOCK_RAM_PATH) ||
      !stock_executable_path(OPEN_VRHMD_STOCK_ROOT_PATH " (deleted)") ||
      stock_executable_path(OPEN_VRHMD_STOCK_RAM_PATH ".other")) abort();
  if (getenv("TEST_CALL_OPEN")) {
    int result = open_devices();
    int closed = 1;
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); ++i)
      if (*devices[i].fd >= 0) closed = 0;
    printf("open=%d signals=%d teardown=%d closed=%d\n", result, signals_sent, teardown_calls, closed);
    close_devices();
  } else {
    int result = take_over_stock_runtime();
    printf("takeover=%d signals=%d errno=%d\n", result, signals_sent, errno);
  }
  if (ioctl_calls) abort();
  return 0;
}
