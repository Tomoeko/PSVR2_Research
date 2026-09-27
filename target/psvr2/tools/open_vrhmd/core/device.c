#include "../open_vrhmd.h"
#include <dirent.h>
#include <limits.h>
#include <time.h>

#ifndef OPEN_VRHMD_LOCK_PATH
#define OPEN_VRHMD_LOCK_PATH "/tmp/open_vrhmd.lock"
#endif
#ifndef OPEN_VRHMD_PROC_ROOT
#define OPEN_VRHMD_PROC_ROOT "/proc"
#endif
#ifndef OPEN_VRHMD_STOP_WAIT_MS
#define OPEN_VRHMD_STOP_WAIT_MS 10000
#endif
#ifndef OPEN_VRHMD_STOCK_RAM_PATH
#define OPEN_VRHMD_STOCK_RAM_PATH "/tmp/vrhmd_main.elf"
#endif
#ifndef OPEN_VRHMD_STOCK_ROOT_PATH
#define OPEN_VRHMD_STOCK_ROOT_PATH "/sie/bin/vrhmd_main.elf"
#endif
#ifndef OPEN_VRHMD_STOCK_WAIT_MS
#define OPEN_VRHMD_STOCK_WAIT_MS 6000
#endif

static int application_lock_fd = -1;
static struct sigaction saved_interrupt, saved_terminate;
volatile sig_atomic_t application_stop_requested;

static void request_application_stop(int signal_number) {
  (void)signal_number;
  application_stop_requested = 1;
}

#include "process.h"

int application_lock_acquire(int stop_existing) {
  if (application_lock_fd >= 0) { errno = EALREADY; return -1; }
  /* Never unlink this file: replacing its inode would allow two owners. */
  int flags = O_RDWR | O_CREAT | O_CLOEXEC;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  int fd = open(OPEN_VRHMD_LOCK_PATH, flags, 0600);
  if (fd < 0) { ERR("open application lock"); return -1; }
  struct stat lock_file;
  if (fstat(fd, &lock_file) < 0 || !S_ISREG(lock_file.st_mode) || lock_file.st_nlink != 1) {
    LOG("Application lock must be a regular file with one link");
    close(fd);
    errno = EINVAL;
    return -1;
  }

  struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
  struct owner_identity signaled_identity = {0};
  pid_t signaled_pid = 0;
  int64_t start = monotonic_milliseconds();
  if (start < 0) goto failed;
  while (fcntl(fd, F_SETLK, &lock) < 0) {
    if (errno != EACCES && errno != EAGAIN) goto failed;
    int64_t now = monotonic_milliseconds();
    if (now < 0) goto failed;
    if (stop_existing && now - start >= OPEN_VRHMD_STOP_WAIT_MS) {
      LOG("Application owner did not finish cleanup within %dms",
          OPEN_VRHMD_STOP_WAIT_MS);
      errno = ETIMEDOUT;
      goto failed;
    }
    pid_t owner;
    if (lock_owner(fd, &owner) < 0) goto failed;
    if (!owner) { usleep(10000); continue; }
    if (!stop_existing) {
      LOG("Headset is owned by open_vrhmd PID %ld; use 'stop' first", (long)owner);
      errno = EBUSY;
      goto failed;
    }
    struct owner_identity identity;
    if (read_owner_identity(owner, &identity) < 0) {
      if (errno == ENOENT || errno == ESRCH) { usleep(10000); continue; }
      ERR("verify application owner PID %ld", (long)owner);
      goto failed;
    }
    if (!signaled_pid) {
      struct owner_identity self, confirmed;
      pid_t current_owner;
      if (read_owner_identity(getpid(), &self) < 0) goto failed;
      if (identity.executable_device != self.executable_device ||
          identity.executable_inode != self.executable_inode) {
        LOG("Refusing to signal lock owner PID %ld: executable differs", (long)owner);
        errno = EBUSY;
        goto failed;
      }
      if (lock_owner(fd, &current_owner) < 0) goto failed;
      if (current_owner != owner || read_owner_identity(owner, &confirmed) < 0 ||
          !same_owner(&identity, &confirmed)) { usleep(10000); continue; }
      LOG("Stopping open_vrhmd owner PID %ld...", (long)owner);
      if (kill(owner, SIGTERM) < 0) {
        if (errno == ESRCH) continue;
        ERR("SIGTERM application owner PID %ld", (long)owner);
        goto failed;
      }
      signaled_pid = owner;
      signaled_identity = identity;
    } else if (owner != signaled_pid || !same_owner(&identity, &signaled_identity)) {
      LOG("Another process acquired the headset while waiting for stop");
      errno = EBUSY;
      goto failed;
    }
    usleep(100000);
  }

  application_stop_requested = 0;
  struct sigaction action = {0};
  action.sa_handler = request_application_stop;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, &saved_interrupt) < 0) goto failed;
  if (sigaction(SIGTERM, &action, &saved_terminate) < 0) {
    int saved_errno = errno;
    sigaction(SIGINT, &saved_interrupt, NULL);
    errno = saved_errno;
    goto failed;
  }
  application_lock_fd = fd;
  return 0;

failed: {
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
  }
}

void application_lock_release(void) {
  if (application_lock_fd < 0) return;
  close(application_lock_fd);
  application_lock_fd = -1;
  sigaction(SIGINT, &saved_interrupt, NULL);
  sigaction(SIGTERM, &saved_terminate, NULL);
}

int fd_dsi = -1;
int fd_mmsys = -1;
int fd_dscenc = -1;
int fd_mutex = -1;
int fd_lhc = -1;
int fd_rdma = -1;
int fd_slicer = -1;
int fd_ion = -1;
int fd_dprx = -1;

static const struct device_node {
  const char *path;
  int *fd;
  int required;
} devices[] = {
  {"/dev/mmsys", &fd_mmsys, 1}, {"/dev/dsi", &fd_dsi, 1},
  {"/dev/dscenc", &fd_dscenc, 0}, {"/dev/mutex", &fd_mutex, 0},
  {"/dev/lhc", &fd_lhc, 0}, {"/dev/rdma0", &fd_rdma, 0},
  {"/dev/slicer", &fd_slicer, 0}, {"/dev/ion", &fd_ion, 0},
  {"/dev/dprx", &fd_dprx, 0},
};

void auto_takeover(int idx) {
  int fd = open("/proc/stage3", O_WRONLY | O_CLOEXEC);
  if (fd < 0) return;
  char command[32];
  int length = snprintf(command, sizeof(command), "takeover %d", idx);
  if (length > 0 && (size_t)length < sizeof(command)) {
    if (write(fd, command, length) != length) ERR("stage3 takeover %d", idx);
    else usleep(100000);
  }
  close(fd);
}

/* WARPA/FE is Sony's tracking path, not a display ownership prerequisite.
 * Its exact 06.00 STREAMOFF has an unbounded kthread_stop, so never invoke it.
 * TASK_COMM_LEN is 16; the driver creates "WarpaFeTriggerThread". */
static int warpa_worker_state(pid_t pid) {
  char path[128], comm[32], line[4096];
  snprintf(path, sizeof(path), OPEN_VRHMD_PROC_ROOT "/%ld/comm", (long)pid);
  FILE *file = fopen(path, "r");
  if (!file) return errno == ENOENT || errno == ESRCH ? 0 : -1;
  int read_ok = fgets(comm, sizeof(comm), file) != NULL;
  fclose(file);
  if (!read_ok) { errno = EIO; return -1; }
  if (strcmp(comm, "WarpaFeTriggerT\n") && strcmp(comm, "WarpaFeTriggerThread\n"))
    return 0;
  snprintf(path, sizeof(path), OPEN_VRHMD_PROC_ROOT "/%ld/stat", (long)pid);
  file = fopen(path, "r");
  if (!file) return errno == ENOENT || errno == ESRCH ? 0 : -1;
  read_ok = fgets(line, sizeof(line), file) != NULL;
  fclose(file);
  if (!read_ok) { errno = EIO; return -1; }
  char *field = strrchr(line, ')');
  if (!field) { errno = EINVAL; return -1; }
  ++field;
  char state = 0;
  for (int number = 3; number <= 9; ++number) {
    while (*field == ' ') ++field;
    if (!*field) { errno = EINVAL; return -1; }
    char *end = field;
    while (*end && *end != ' ' && *end != '\n') ++end;
    if (number == 3) state = *field;
    if (number == 9) {
      char *parsed;
      errno = 0;
      unsigned long long flags = strtoull(field, &parsed, 10);
      if (errno || parsed == field || parsed != end || *field == '-') {
        errno = EINVAL;
        return -1;
      }
      /* PF_KTHREAD from the pinned 06.00 kernel; comm alone is insufficient. */
      return (flags & 0x00200000) && state != 'Z' && state != 'X';
    }
    field = end;
  }
  errno = EINVAL;
  return -1;
}

static int find_warpa_worker(void) {
  DIR *directory = opendir(OPEN_VRHMD_PROC_ROOT);
  if (!directory) return -1;
  int error = 0;
  pid_t active_pid = 0;
  size_t inspected = 0;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(directory);
    if (!entry) { error = errno; break; }
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
    char *end;
    long value = strtol(entry->d_name, &end, 10);
    if (errno || *end || value <= 1 || value > INT_MAX) continue;
    if (++inspected > 4096) { error = EOVERFLOW; break; }
    int active = warpa_worker_state((pid_t)value);
    if (active < 0) { error = errno; break; }
    if (active) {
      active_pid = (pid_t)value;
      break;
    }
  }
  closedir(directory);
  if (error) { errno = error; return -1; }
  return active_pid;
}

static void report_warpa_worker(void) {
  int active = find_warpa_worker();
  if (active < 0) LOG("WARPA tracking state unknown; leaving it unchanged (no STREAMOFF)");
  else if (active)
    LOG("Leaving Sony WARPA tracking worker PID %d unchanged (no STREAMOFF)", active);
  else LOG("WARPA tracking worker not present; skipping non-idempotent STREAMOFF");
}

struct stock_process {
  pid_t pid;
  struct owner_identity identity;
};

static int stock_executable_path(const char *path) {
  size_t length = strlen(path);
  static const char deleted[] = " (deleted)";
  if (length >= sizeof(deleted) - 1 &&
      !strcmp(path + length - (sizeof(deleted) - 1), deleted))
    length -= sizeof(deleted) - 1;
  const char *allowed[] = {OPEN_VRHMD_STOCK_RAM_PATH, OPEN_VRHMD_STOCK_ROOT_PATH};
  for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i)
    if (length == strlen(allowed[i]) && !strncmp(path, allowed[i], length)) return 1;
  return 0;
}

/* VrhmdMain renames comm on 06.00; identify its executable, not its task name.
 * A missing exe also covers kernel threads and zombies with released files. */
static int stock_process_identity(pid_t pid, struct owner_identity *identity) {
  char path[128], executable[512];
  snprintf(path, sizeof(path), OPEN_VRHMD_PROC_ROOT "/%ld/exe", (long)pid);
  ssize_t length = readlink(path, executable, sizeof(executable) - 1);
  if (length < 0) return errno == ENOENT || errno == ESRCH ? 0 : -1;
  executable[length] = '\0';
  if (!stock_executable_path(executable)) return 0;
  if (read_owner_identity(pid, identity) < 0)
    return errno == ENOENT || errno == ESRCH ? 0 : -1;
  return 1;
}

static int find_stock_processes(struct stock_process *processes, size_t capacity) {
  DIR *directory = opendir(OPEN_VRHMD_PROC_ROOT);
  if (!directory) return -1;
  size_t count = 0;
  int error = 0;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(directory);
    if (!entry) { error = errno; break; }
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
    char *end;
    errno = 0;
    long value = strtol(entry->d_name, &end, 10);
    if (errno || *end || value <= 1 || value > INT_MAX || value == getpid()) continue;
    struct owner_identity identity;
    int found = stock_process_identity((pid_t)value, &identity);
    if (found < 0) { error = errno; break; }
    if (!found) continue;
    if (count == capacity) { error = EOVERFLOW; break; }
    processes[count++] = (struct stock_process){(pid_t)value, identity};
  }
  closedir(directory);
  if (error) { errno = error; return -1; }
  return (int)count;
}

static int take_over_stock_runtime(void) {
  struct stock_process processes[32];
  int count = find_stock_processes(processes, sizeof(processes) / sizeof(processes[0]));
  if (count < 0) { ERR("inspect stock runtime"); return -1; }
  if (!count) { report_warpa_worker(); return 0; }
  for (int i = 0; i < count; ++i) {
    struct owner_identity confirmed;
    int found = stock_process_identity(processes[i].pid, &confirmed);
    if (found < 0) { ERR("verify stock PID %ld", (long)processes[i].pid); return -1; }
    if (!found || !same_owner(&processes[i].identity, &confirmed)) continue;
    LOG("Found stock VrhmdMain executable at PID %ld. Terminating...",
        (long)processes[i].pid);
    if (kill(processes[i].pid, SIGKILL) < 0 && errno != ESRCH) {
      ERR("terminate stock PID %ld", (long)processes[i].pid);
      return -1;
    }
  }
  int64_t start = monotonic_milliseconds();
  if (start < 0) return -1;
  for (;;) {
    count = find_stock_processes(processes, sizeof(processes) / sizeof(processes[0]));
    if (count < 0) { ERR("wait for stock runtime"); return -1; }
    if (!count) break;
    int64_t now = monotonic_milliseconds();
    if (now < 0) return -1;
    if (now - start >= OPEN_VRHMD_STOCK_WAIT_MS) {
      LOG("Stock VrhmdMain did not release the hardware within %dms",
          OPEN_VRHMD_STOCK_WAIT_MS);
      errno = ETIMEDOUT;
      return -1;
    }
    usleep(100000);
  }
  report_warpa_worker();
  usleep(500000);
  return 1;
}

int open_devices(void) {
  int takeover = take_over_stock_runtime();
  if (takeover < 0) return -1;
  LOG("Opening device nodes...");
  for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
    const struct device_node *node = &devices[i];
    if (*node->fd < 0) *node->fd = open(node->path, O_RDWR | O_CLOEXEC);
    if (*node->fd >= 0) LOG("  %s fd=%d", node->path, *node->fd);
    else if (node->required) {
      ERR("open %s", node->path);
      close_devices();
      return -1;
    } else LOG("  %s not available (non-fatal)", node->path);
  }
  if (takeover) {
    LOG("Tearing down the previous display pipeline...");
    if (cmd_stop() < 0) { close_devices(); return -1; }
  }
  return 0;
}

void close_devices(void) {
  for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
    if (*devices[i].fd >= 0) close(*devices[i].fd);
    *devices[i].fd = -1;
  }
}
