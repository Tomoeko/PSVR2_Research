#include "../open_vrhmd.h"
#include <stdarg.h>
#include <sys/prctl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include "process.h"

/* PWM sysfs paths */
#define FAN_PWM_EXPORT    "/sys/class/pwm/pwmchip0/export"
#define FAN_PWM_UNEXPORT  "/sys/class/pwm/pwmchip0/unexport"
#define FAN_PWM_ENABLE    "/sys/class/pwm/pwmchip0/pwm20/enable"
#define FAN_PWM_PERIOD    "/sys/class/pwm/pwmchip0/pwm20/period"
#define FAN_PWM_DUTY      "/sys/class/pwm/pwmchip0/pwm20/duty_cycle"

#define I2C_EXT_TEMP_BUS  2
#define I2C_EXT_TEMP_ADDR 0x49

/* Fan duty hysteresis table (from vrhmd binary at 0xBD388) */
static const uint8_t fan_duty_table[4][3] = {
  { 80, 60, 39 },
  { 46, 44, 37 },
  { 38, 42, 33 },
  { 30, 38,  0 },
};
#define FAN_DUTY_ROWS 4
#define FAN_DEFAULT_DUTY 30
#define FAN_PERIOD_NS 1000000

/* Thermal emergency thresholds */
#define THERMAL_EMERGENCY_SOC_MDEG  85000
#define THERMAL_CAUTION_SOC_MDEG    84000
#define THERMAL_EMERGENCY_EXT_DEG   55

#ifndef FAN_CMD_FIFO
#define FAN_CMD_FIFO "/tmp/open_vrhmd_fan.cmd"
#endif
#ifndef FAN_LOCK_PATH
#define FAN_LOCK_PATH "/tmp/open_vrhmd_fan.lock"
#endif
#ifndef FAN_STOP_WAIT_MS
#define FAN_STOP_WAIT_MS 10000
#endif

static volatile sig_atomic_t fan_stop = 0;

static void fan_signal_stop(int sig) {
  (void)sig;
  fan_stop = 1;
}

static void klog(const char *prio, const char *fmt, ...) {
  int fd = open("/dev/kmsg", O_WRONLY);
  if (fd < 0) return;

  char buf[256];
  va_list args;
  va_start(args, fmt);
  int n = snprintf(buf, sizeof(buf), "%s[open_vrhmd_fan] ", prio);
  if (n < 0) {
    va_end(args);
    close(fd);
    return;
  }
  if (n < (int)sizeof(buf)) {
    vsnprintf(buf + n, sizeof(buf) - n, fmt, args);
  }
  va_end(args);

  /* Ensure newline for kmsg */
  size_t len = strlen(buf);
  if (len > 0 && len < sizeof(buf) - 1 && buf[len-1] != '\n') {
    buf[len] = '\n';
    buf[len+1] = '\0';
  }

  ssize_t ret = write(fd, buf, strlen(buf));
  (void)ret;
  close(fd);
}

static int fan_sysfs_write(const char *path, const char *val) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  size_t len = strlen(val);
  ssize_t ret = write(fd, val, len);
  int saved_errno = errno;
  close(fd);
  errno = ret >= 0 && (size_t)ret != len ? EIO : saved_errno;
  return ret < 0 || (size_t)ret != len ? -1 : 0;
}

static int fan_pwm_export(void) {
  if (fan_sysfs_write(FAN_PWM_EXPORT, "20") < 0 && errno != EBUSY)
    return -1;
  usleep(100000);
  return 0;
}

static int fan_pwm_set_duty(int duty_percent) {
  if (duty_percent < 0 || duty_percent > 100) {
    errno = EINVAL;
    return -1;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", FAN_PERIOD_NS);
  if (fan_sysfs_write(FAN_PWM_PERIOD, buf) < 0) return -1;
  int duty_ns = (FAN_PERIOD_NS * duty_percent) / 100;
  snprintf(buf, sizeof(buf), "%d", duty_ns);
  if (fan_sysfs_write(FAN_PWM_DUTY, buf) < 0) return -1;
  return fan_sysfs_write(FAN_PWM_ENABLE, "1");
}

static int fan_read_soc_temp_mdeg(void) {
  int fd = open("/dev/mtk_wrapper_thermal", O_RDWR);
  if (fd < 0) return -1;
  int max_temp = -999999;
  for (int s = 0; s < 8; s++) {
    struct thermal_get_from_id args = { .in_bank_id = 0, .in_sensor_id = s };
    if (ioctl(fd, THERMAL_GET_FROM_ID_CMD, &args) == 0) {
      if (args.out_temperature > max_temp) max_temp = args.out_temperature;
    }
  }
  close(fd);
  return max_temp;
}

static int fan_read_ext_temp_deg(void) {
  char path[32];
  snprintf(path, sizeof(path), "/dev/i2c-%d", I2C_EXT_TEMP_BUS);
  int fd = open(path, O_RDWR);
  if (fd < 0) return -999;
  uint8_t reg = 0x00;
  uint8_t data[2] = {0};
  struct i2c_msg_user msgs[2];
  msgs[0].addr = I2C_EXT_TEMP_ADDR; msgs[0].flags = 0; msgs[0].len = 1; msgs[0].buf = &reg;
  msgs[1].addr = I2C_EXT_TEMP_ADDR; msgs[1].flags = 1; msgs[1].len = 2; msgs[1].buf = data;
  struct i2c_rdwr_data rdwr = { .msgs = msgs, .nmsgs = 2 };
  int ret = ioctl(fd, I2C_RDWR, &rdwr);
  close(fd);
  if (ret < 0) return -999;
  int16_t raw = (int16_t)((data[0] << 8) | data[1]) >> 8;
  return raw;
}

static int fan_lookup_duty(int current_duty, int temp_deg) {
  /* Recover from the emergency setting through the highest normal state. */
  if (current_duty == 100)
    return fan_duty_table[0][0];
  int cur_row = FAN_DUTY_ROWS - 1;
  for (int i = 0; i < FAN_DUTY_ROWS; i++) {
    if (current_duty == fan_duty_table[i][0]) { cur_row = i; break; }
  }
  if (cur_row > 0) {
    int higher_row = cur_row - 1;
    if (temp_deg >= fan_duty_table[higher_row][1]) return fan_duty_table[higher_row][0];
  }
  if (cur_row < FAN_DUTY_ROWS - 1) {
    if (temp_deg < fan_duty_table[cur_row][2]) return fan_duty_table[cur_row + 1][0];
  }
  return current_duty;
}

static int fan_owner_identity(pid_t pid, struct owner_identity *identity);

/* The record is written only after PWM initialization, while both locks are held. */
static int fan_publish_ready(int lock_fd) {
  struct owner_identity identity;
  if (read_owner_identity(getpid(), &identity) < 0) return -1;
  char record[80];
  int size = snprintf(record, sizeof(record), "%ld %llu\n", (long)getpid(), identity.start_time);
  if (pwrite(lock_fd, record, (size_t)size, 0) != size || ftruncate(lock_fd, size) < 0)
    return -1;
  return 0;
}

static int fan_existing_ready(int lock_fd) {
  pid_t owner, confirmed_owner;
  struct owner_identity identity, self, confirmed;
  if (lock_owner(lock_fd, &owner) < 0 || owner <= 1 ||
      fan_owner_identity(owner, &identity) < 0 || read_owner_identity(getpid(), &self) < 0)
    return -1;
  if (identity.executable_device != self.executable_device ||
      identity.executable_inode != self.executable_inode) { errno = EBUSY; return -1; }
  char record[80] = {0}, extra;
  long ready_pid;
  unsigned long long ready_start;
  ssize_t size = pread(lock_fd, record, sizeof(record) - 1, 0);
  if (size <= 0 || sscanf(record, "%ld %llu %c", &ready_pid, &ready_start, &extra) != 2 ||
      ready_pid != owner || ready_start != identity.start_time) { errno = EBUSY; return -1; }
  if (lock_owner(lock_fd, &confirmed_owner) < 0 || confirmed_owner != owner ||
      fan_owner_identity(owner, &confirmed) < 0 || !same_owner(&identity, &confirmed)) {
    errno = EBUSY;
    return -1;
  }
  return 0;
}

static void fan_thermal_loop(int lock_fd, int ready_fd) {
  if (fan_pwm_export() < 0) {
    klog("<3>", "ERR: Failed to export fan PWM");
    close(ready_fd);
    return;
  }

  /* Command FIFO for status queries */
  unlink(FAN_CMD_FIFO);
  if (mkfifo(FAN_CMD_FIFO, 0666) < 0 && errno != EEXIST) {
    klog("<3>", "ERR: Failed to create command FIFO");
  }
  /* Keeping a writer open avoids perpetual POLLHUP when a client disconnects. */
  int cmd_fd = open(FAN_CMD_FIFO, O_RDWR | O_NONBLOCK);
  if (cmd_fd < 0)
    klog("<3>", "ERR: Failed to open command FIFO (errno=%d)", errno);

  int current_duty = FAN_DEFAULT_DUTY;
  if (fan_stop || fan_pwm_set_duty(current_duty) < 0) {
    klog("<3>", "ERR: Failed to set initial fan duty (errno=%d)", errno);
    if (cmd_fd >= 0) close(cmd_fd);
    unlink(FAN_CMD_FIFO);
    close(ready_fd);
    return;
  }
  char owned = 1;
  int ready = !fan_stop && fan_publish_ready(lock_fd) == 0 && write(ready_fd, &owned, 1) == 1;
  close(ready_fd);
  if (!ready) {
    if (cmd_fd >= 0) close(cmd_fd);
    unlink(FAN_CMD_FIFO);
    return;
  }
  klog("<6>", "Daemon started. Target duty: %d%%. Monitoring SoC/Ext temps...", current_duty);

  struct pollfd pfd = { .fd = cmd_fd, .events = POLLIN };

  while (!fan_stop) {
    int soc_mdeg = fan_read_soc_temp_mdeg();
    int ext_deg = fan_read_ext_temp_deg();
    int soc_deg = soc_mdeg / 1000;

    if (soc_mdeg >= THERMAL_EMERGENCY_SOC_MDEG || ext_deg >= THERMAL_EMERGENCY_EXT_DEG) {
      klog("<1>", "EMERGENCY: SoC=%d.%dC Ext=%dC -> Fan 100%%",
           soc_deg, (soc_mdeg % 1000) / 100, ext_deg);
      if (fan_pwm_set_duty(100) == 0)
        current_duty = 100;
      else
        klog("<3>", "ERR: Failed to set emergency fan duty (errno=%d)", errno);
      sleep(5);
      continue;
    }

    if (soc_mdeg >= THERMAL_CAUTION_SOC_MDEG || ext_deg >= (THERMAL_EMERGENCY_EXT_DEG - 1)) {
      if (current_duty != 100) {
        klog("<4>", "CAUTION: SoC=%d.%dC Ext=%dC -> Fan 100%%",
             soc_deg, (soc_mdeg % 1000) / 100, ext_deg);
        if (fan_pwm_set_duty(100) == 0)
          current_duty = 100;
        else
          klog("<3>", "ERR: Failed to set caution fan duty (errno=%d)", errno);
      }
    } else {
      int lookup_temp = soc_deg > ext_deg ? soc_deg : ext_deg;
      int new_duty = fan_lookup_duty(current_duty, lookup_temp);
      if (new_duty != current_duty) {
        klog("<7>", "Temp state change: %dC -> Fan %d%% (was %d%%)",
             lookup_temp, new_duty, current_duty);
        if (fan_pwm_set_duty(new_duty) == 0)
          current_duty = new_duty;
        else
          klog("<3>", "ERR: Failed to update fan duty (errno=%d)", errno);
      }
    }

    /* Wait 2 seconds for commands, then loop */
    if (poll(&pfd, 1, 2000) > 0 && (pfd.revents & POLLIN)) {
      char cmd[64] = {0};
      ssize_t n = read(cmd_fd, cmd, sizeof(cmd)-1);
      if (n > 0 && strstr(cmd, "status")) {
        klog("<6>", "Status: SoC=%d.%dC Ext=%dC Fan=%d%%",
             soc_deg, (soc_mdeg % 1000) / 100, ext_deg, current_duty);
      }
    }
  }

  if (cmd_fd >= 0) close(cmd_fd);
  unlink(FAN_CMD_FIFO);
}

int fan_ctrl_init(void) {
  int flags = O_RDWR | O_CREAT | O_CLOEXEC;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  int lock_fd = open(FAN_LOCK_PATH, flags, 0600);
  if (lock_fd < 0) return -1;
  struct stat file;
  if (fstat(lock_fd, &file) < 0 || !S_ISREG(file.st_mode) || file.st_nlink != 1) {
    close(lock_fd);
    errno = EINVAL;
    return -1;
  }

  if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
    int result = -1;
    if (errno == EWOULDBLOCK || errno == EAGAIN) result = fan_existing_ready(lock_fd);
    int saved_errno = errno;
    close(lock_fd);
    errno = saved_errno;
    return result;
  }
  if (ftruncate(lock_fd, 0) < 0) { close(lock_fd); return -1; }

  int ready[2];
  if (pipe(ready) < 0) { close(lock_fd); return -1; }
  /* The inherited flock reserves startup; the child publishes its own PID
   * with a POSIX record lock, which is not inherited from the parent. */
  pid_t pid = fork();
  if (pid < 0) {
    close(ready[0]);
    close(ready[1]);
    close(lock_fd);
    return -1;
  }

  if (pid > 0) {
    close(ready[1]);
    struct pollfd startup = {.fd = ready[0], .events = POLLIN};
    char owned = 0;
    int result = -1;
    if (poll(&startup, 1, 1000) > 0 && read(ready[0], &owned, 1) == 1 && owned == 1) {
      LOG("Fan control daemon spawned in background (PID: %d)", pid);
      result = 0;
    } else {
      LOG("Fan control daemon did not initialize cooling");
      /* This is our unreaped child, so its PID cannot have been reused. */
      kill(pid, SIGKILL);
      /* Do not block on a child stuck in a kernel operation. Teardown still
       * requires its lock release before touching PWM. */
      waitpid(pid, NULL, WNOHANG);
      errno = EIO;
    }
    close(ready[0]);
    close(lock_fd); /* The child retains the shared flock. */
    return result;
  }

  close(ready[0]);
  /* Install the fan handler before publishing ownership: an immediate stop
   * must not reach the inherited application stop handler. */
  prctl(PR_SET_NAME, "open_vrhmd_fan", 0, 0, 0);
  struct sigaction sa = {0};
  sa.sa_handler = fan_signal_stop;
  sigemptyset(&sa.sa_mask);
  fan_stop = 0;
  struct flock ownership = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
  if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0 ||
      fcntl(lock_fd, F_SETLK, &ownership) < 0) _exit(1);
  setsid();
  /* Keep cooling after display exit, without retaining display descriptors. */
  close_devices();

  /* Close standard FDs to fully detach */
  close(0);
  close(1);
  close(2);
  open("/dev/null", O_RDONLY);
  open("/dev/null", O_WRONLY);
  open("/dev/null", O_WRONLY);

  /* Run the loop forever */
  if (!fan_stop) fan_thermal_loop(lock_fd, ready[1]);
  else close(ready[1]);

  /* Release the singleton lock after shutdown or startup failure. */
  close(lock_fd);
  _exit(0);
}

static int fan_owner_identity(pid_t pid, struct owner_identity *identity) {
  if (read_owner_identity(pid, identity) < 0) return -1;
  char path[128], comm[32];
  snprintf(path, sizeof(path), OPEN_VRHMD_PROC_ROOT "/%ld/comm", (long)pid);
  FILE *file = fopen(path, "r");
  if (!file) return -1;
  int ok = fgets(comm, sizeof(comm), file) != NULL;
  fclose(file);
  if (!ok || strcmp(comm, "open_vrhmd_fan\n")) { errno = EBUSY; return -1; }
  return 0;
}

int fan_ctrl_teardown(void) {
  int flags = O_RDWR | O_CREAT | O_CLOEXEC;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  int lock_fd = open(FAN_LOCK_PATH, flags, 0600);
  if (lock_fd < 0) { ERR("open fan ownership"); return -1; }
  struct stat lock_file;
  if (fstat(lock_fd, &lock_file) < 0 || !S_ISREG(lock_file.st_mode) ||
      lock_file.st_nlink != 1) { errno = EINVAL; goto failed; }
  int64_t start = monotonic_milliseconds();
  if (start < 0) goto failed;
  pid_t signaled = 0;
  struct owner_identity signaled_identity = {0};
  for (;;) {
    pid_t owner;
    if (lock_owner(lock_fd, &owner) < 0) goto failed;
    if (!owner) {
      if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0) break;
      if (errno != EWOULDBLOCK && errno != EAGAIN) goto failed;
      /* A child may still be installing handlers before publishing its PID. */
    } else {
      struct owner_identity identity;
      if (fan_owner_identity(owner, &identity) < 0) {
        if (errno != ENOENT && errno != ESRCH) goto failed;
      } else if (!signaled) {
        struct owner_identity self, confirmed;
        pid_t confirmed_owner;
        if (read_owner_identity(getpid(), &self) < 0) goto failed;
        if (identity.executable_device != self.executable_device ||
            identity.executable_inode != self.executable_inode) {
          errno = EBUSY;
          LOG("Refusing fan lock owner PID %ld: executable differs", (long)owner);
          goto failed;
        }
        if (lock_owner(lock_fd, &confirmed_owner) < 0) goto failed;
        if (confirmed_owner == owner && fan_owner_identity(owner, &confirmed) == 0 &&
            same_owner(&identity, &confirmed)) {
          LOG("Stopping fan daemon PID %ld...", (long)owner);
          if (kill(owner, SIGTERM) < 0 && errno != ESRCH) goto failed;
          signaled = owner;
          signaled_identity = identity;
        }
      } else if (owner != signaled || !same_owner(&identity, &signaled_identity)) {
        errno = EBUSY;
        LOG("Fan ownership changed while waiting for stop");
        goto failed;
      }
    }
    int64_t now = monotonic_milliseconds();
    if (now < 0) goto failed;
    if (now - start >= FAN_STOP_WAIT_MS) { errno = ETIMEDOUT; goto failed; }
    usleep(100000);
  }

  /* Wait a bit for the daemon to truly exit and release sysfs handles.
   * Increased to 250ms because Sony's motor.c is extremely sensitive. */
  usleep(250000);

  /* Keep this inode: unlinking it could let a second daemon bypass ownership. */

  /* Explicitly disable the PWM signal first */
  fan_sysfs_write(FAN_PWM_ENABLE, "0");

  /* CRITICAL: Unexport the PWM channel so vrhmd_main.elf doesn't crash
   * in motor.c when attempting to re-export it. */
  fan_sysfs_write(FAN_PWM_UNEXPORT, "20");

  /* Sony Safety Sweep: vrhmd_main.elf uses GPIO 13 (PDCON) and GPIO 19 (Motor).
   * MTK sysfs offset is 0x140 (320), so 13->333 and 19->339. */
  fan_sysfs_write("/sys/class/gpio/unexport", "333");
  fan_sysfs_write("/sys/class/gpio/unexport", "339");

  /* Final settle time for kernel to process unexports */
  usleep(50000);
  close(lock_fd);
  return 0;

failed: {
    int saved_errno = errno;
    close(lock_fd);
    errno = saved_errno;
    ERR("fan daemon did not release ownership; PWM cleanup skipped");
    return -1;
  }
}
