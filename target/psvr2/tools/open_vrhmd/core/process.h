/* Native process identity and record-lock checks shared by application/fan ownership. */
#ifndef OPEN_VRHMD_PROCESS_H
#define OPEN_VRHMD_PROCESS_H

#include <time.h>
#ifndef OPEN_VRHMD_PROC_ROOT
#define OPEN_VRHMD_PROC_ROOT "/proc"
#endif

struct owner_identity {
  dev_t executable_device;
  ino_t executable_inode;
  unsigned long long start_time;
};

/* The lock supplies the PID; /proc verifies executable and guards PID reuse. */
static inline int read_owner_identity(pid_t pid, struct owner_identity *identity) {
  char path[128], stat_line[4096];
  snprintf(path, sizeof(path), OPEN_VRHMD_PROC_ROOT "/%ld/exe", (long)pid);
  struct stat executable;
  if (stat(path, &executable) < 0) return -1;
  identity->executable_device = executable.st_dev;
  identity->executable_inode = executable.st_ino;

  snprintf(path, sizeof(path), OPEN_VRHMD_PROC_ROOT "/%ld/stat", (long)pid);
  FILE *file = fopen(path, "r");
  if (!file) return -1;
  int read_ok = fgets(stat_line, sizeof(stat_line), file) != NULL;
  fclose(file);
  if (!read_ok) { errno = EIO; return -1; }
  /* comm (field 2) may contain spaces and parentheses. */
  char *field = strrchr(stat_line, ')');
  if (!field) { errno = EINVAL; return -1; }
  ++field;
  for (int number = 3; number <= 22; ++number) {
    while (*field == ' ') ++field;
    if (!*field) { errno = EINVAL; return -1; }
    char *end = field;
    while (*end && *end != ' ' && *end != '\n') ++end;
    if (number == 22) {
      char *parsed;
      errno = 0;
      identity->start_time = strtoull(field, &parsed, 10);
      if (errno || parsed == field || parsed != end || *field == '-') {
        errno = EINVAL;
        return -1;
      }
      return 0;
    }
    field = end;
  }
  errno = EINVAL;
  return -1;
}

static inline int same_owner(const struct owner_identity *a,
                       const struct owner_identity *b) {
  return a->executable_device == b->executable_device &&
         a->executable_inode == b->executable_inode &&
         a->start_time == b->start_time;
}

static inline int lock_owner(int fd, pid_t *owner) {
  struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
  if (fcntl(fd, F_GETLK, &lock) < 0) return -1;
  *owner = lock.l_type == F_UNLCK ? 0 : lock.l_pid;
  return 0;
}

static inline int64_t monotonic_milliseconds(void) {
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time) < 0) return -1;
  return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

#endif
