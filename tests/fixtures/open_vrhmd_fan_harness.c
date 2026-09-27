/* Real host PIDs/record locks; independent Linux flock class modeled on a
 * separate inode because Darwin combines flock and POSIX locks. Synthetic
 * /proc and redirected sysfs prevent any headset access. */
#include "../../target/psvr2/tools/open_vrhmd/open_vrhmd.h"
#include <assert.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <sys/prctl.h>

static void proc_record(pid_t pid, const char *comm, const char *executable) {
  char path[512];
  snprintf(path, sizeof(path), OPEN_VRHMD_PROC_ROOT "/%ld", (long)pid);
  assert(mkdir(path, 0700) == 0 || errno == EEXIST);
  snprintf(path, sizeof(path), OPEN_VRHMD_PROC_ROOT "/%ld/exe", (long)pid);
  unlink(path);
  assert(symlink(executable, path) == 0);
  snprintf(path, sizeof(path), OPEN_VRHMD_PROC_ROOT "/%ld/stat", (long)pid);
  FILE *file = fopen(path, "w"); assert(file);
  fprintf(file, "%ld (%s) S", (long)pid, comm);
  for (int field = 4; field <= 22; ++field) fprintf(file, " %d", field == 22 ? 12345 : 0);
  fputc('\n', file); fclose(file);
  snprintf(path, sizeof(path), OPEN_VRHMD_PROC_ROOT "/%ld/comm", (long)pid);
  file = fopen(path, "w"); assert(file);
  fprintf(file, "%s\n", comm); fclose(file);
}

static int mock_prctl(int option, ...) {
  assert(option == PR_SET_NAME);
  proc_record(getpid(), "open_vrhmd_fan", getenv("TEST_BINARY"));
  return 0;
}

static int mock_open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args; va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args);
  }
  if (!strncmp(path, "/sys/", 5)) {
    if (getenv("FAIL_PWM") && strstr(path, "duty_cycle")) { errno = EIO; return -1; }
    int fd = open(getenv("SYSFS_LOG"), O_WRONLY | O_CREAT | O_APPEND, 0600);
    assert(fd >= 0);
    dprintf(fd, "\n%ld %s ", (long)getpid(), path);
    return fd;
  }
  if (!strncmp(path, "/dev/", 5) && strcmp(path, "/dev/null")) { errno = ENOENT; return -1; }
  return open(path, flags, mode);
}

static int reservations[1024];
static int mock_flock(int fd, int operation) {
  assert(fd >= 0 && fd < 1024);
  if (!reservations[fd]) {
    int reservation = open(FAN_LOCK_PATH ".flock", O_RDWR | O_CREAT, 0600);
    assert(reservation > 0);
    reservations[fd] = reservation;
  }
  return flock(reservations[fd], operation);
}
static int mock_close(int fd) {
  if (fd >= 0 && fd < 1024 && reservations[fd]) {
    close(reservations[fd]);
    reservations[fd] = 0;
  }
  return close(fd);
}

#define flock(fd, operation) mock_flock(fd, operation)
#define close mock_close
#define prctl mock_prctl
#define open mock_open
#include "../../target/psvr2/tools/open_vrhmd/core/fan_ctrl.c"
#undef open
#undef prctl
#undef close
#undef flock

void close_devices(void) {}

static pid_t owner_start(const char *scenario) {
  int ready[2]; assert(pipe(ready) == 0);
  pid_t pid = fork(); assert(pid >= 0);
  if (!pid) {
    close(ready[0]);
    proc_record(getpid(), !strcmp(scenario, "comm") ? "unrelated" : "open_vrhmd_fan",
                !strcmp(scenario, "exe") ? "/bin/sh" : getenv("TEST_BINARY"));
    struct sigaction handler = {0};
    handler.sa_handler = !strcmp(scenario, "timeout") ? SIG_IGN : fan_signal_stop;
    sigemptyset(&handler.sa_mask);
    assert(sigaction(SIGTERM, &handler, NULL) == 0);
    int fd = open(FAN_LOCK_PATH, O_RDWR | O_CREAT, 0600); assert(fd >= 0);
    assert(mock_flock(fd, LOCK_EX | LOCK_NB) == 0);
    char byte = 1;
    if (!strcmp(scenario, "gap")) {
      assert(write(ready[1], &byte, 1) == 1);
      usleep(150000);
    }
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    assert(fcntl(fd, F_SETLK, &lock) == 0);
    if (strcmp(scenario, "notready")) assert(fan_publish_ready(fd) == 0);
    if (strcmp(scenario, "gap")) assert(write(ready[1], &byte, 1) == 1);
    close(ready[1]);
    while (!fan_stop) usleep(10000);
    mock_close(fd);
    _exit(0);
  }
  close(ready[1]); char byte; assert(read(ready[0], &byte, 1) == 1); close(ready[0]);
  return pid;
}

static void reap(pid_t child) {
  int status;
  assert(waitpid(child, &status, 0) == child);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  proc_record(getpid(), "open_vrhmd", getenv("TEST_BINARY"));
  const char *scenario = argv[1];
  if (!strcmp(scenario, "init") || !strcmp(scenario, "initfail")) {
    int result = fan_ctrl_init();
    printf("init=%d\n", result);
    if (!strcmp(scenario, "initfail")) {
      assert(result < 0);
      assert(fan_ctrl_teardown() == 0);
      while (waitpid(-1, NULL, WNOHANG) > 0) {}
      return 0;
    }
    assert(result == 0);
    int fd = open(FAN_LOCK_PATH, O_RDWR); assert(fd >= 0);
    pid_t child; assert(lock_owner(fd, &child) == 0 && child > 1);
    struct stat before, after; assert(fstat(fd, &before) == 0);
    assert(fan_existing_ready(fd) == 0);
    close(fd);
    /* Normal display return keeps this daemon alive; reuse does not fork. */
    assert(kill(child, 0) == 0);
    assert(fan_ctrl_init() == 0);
    assert(fan_ctrl_teardown() == 0);
    reap(child);
    assert(stat(FAN_LOCK_PATH, &after) == 0 && before.st_ino == after.st_ino);
    assert(fan_ctrl_teardown() == 0);
    return 0;
  }
  pid_t child = owner_start(scenario);
  int reused = fan_ctrl_init();
  int result = fan_ctrl_teardown();
  printf("reuse=%d teardown=%d errno=%d\n", reused, result, errno);
  if (!strcmp(scenario, "timeout") || !strcmp(scenario, "comm") || !strcmp(scenario, "exe")) {
    assert(result < 0 && kill(child, 0) == 0);
    assert(access(getenv("SYSFS_LOG"), F_OK) < 0);
    assert(kill(child, SIGKILL) == 0);
  } else assert(result == 0);
  if (!strcmp(scenario, "notready") || !strcmp(scenario, "gap") ||
      !strcmp(scenario, "comm") || !strcmp(scenario, "exe")) assert(reused < 0);
  else assert(reused == 0);
  reap(child);
  return 0;
}
