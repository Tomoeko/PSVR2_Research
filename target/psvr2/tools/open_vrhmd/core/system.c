#include "../open_vrhmd.h"
#include <sys/statvfs.h>

#define GPU_DEVFREQ "/sys/devices/platform/soc/13000000.rgx/devfreq/13000000.rgx/"

static int read_text(const char *path, char *value, size_t size) {
  FILE *file = fopen(path, "r");
  if (!file) return -1;
  int ret = fgets(value, size, file) ? 0 : -1;
  fclose(file);
  if (ret == 0) value[strcspn(value, "\r\n")] = '\0';
  return ret;
}

static int set_sysfs(const char *path, const char *value) {
  char current[32] = {0};
  if (read_text(path, current, sizeof(current)) == 0 && strcmp(current, value) == 0)
    return 0;
  FILE *file = fopen(path, "w");
  if (!file) return -1;
  int ret = fputs(value, file) < 0 ? -1 : 0;
  if (fclose(file) != 0) ret = -1;
  return ret;
}

void perform_tuning(void) {
  int policies = 0;
  for (int i = 0; i < 4; i++) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor", i);
    if (set_sysfs(path, "performance") == 0) policies++;
  }
  LOG("CPU governor: performance applied to %d policies", policies);
  int max_ret = set_sysfs(GPU_DEVFREQ "max_freq", "870000000");
  int min_ret = set_sysfs(GPU_DEVFREQ "min_freq", "870000000");
  LOG("GPU frequency: 870MHz requested (min=%s max=%s)",
      min_ret == 0 ? "ok" : "unavailable", max_ret == 0 ? "ok" : "unavailable");
}

#ifdef GPU_RENDER
static void report_gl_info(void) {
  void *he = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
  void *hg = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
  if (!he || !hg) {
    LOG("OpenGL ES        : Unavailable (missing libraries)");
    goto cleanup;
  }
  ED (*get_display)(void *) = dlsym(he, "eglGetDisplay");
  EB (*initialize)(ED, EGLint *, EGLint *) = dlsym(he, "eglInitialize");
  EB (*choose_config)(ED, const EGLint *, EC *, EGLint, EGLint *) = dlsym(he, "eglChooseConfig");
  EX (*create_context)(ED, EC, EX, const EGLint *) = dlsym(he, "eglCreateContext");
  EB (*make_current)(ED, void *, void *, EX) = dlsym(he, "eglMakeCurrent");
  EB (*destroy_context)(ED, EX) = dlsym(he, "eglDestroyContext");
  EB (*terminate)(ED) = dlsym(he, "eglTerminate");
  const unsigned char *(*get_string)(GLenum) = dlsym(hg, "glGetString");
  if (!get_display || !initialize || !choose_config || !create_context ||
      !make_current || !destroy_context || !terminate || !get_string) {
    LOG("OpenGL ES        : Unavailable (missing EGL/GLES entry points)");
    goto cleanup;
  }
  ED display = get_display(NULL);
  EGLint major, minor;
  if (!display || !initialize(display, &major, &minor)) goto cleanup;
  const EGLint config_attributes[] = {0x3040, 4, 0x3033, 0, 0x3038};
  const EGLint context_attributes[] = {0x3098, 2, 0x3038};
  EC config;
  EGLint count = 0;
  if (choose_config(display, config_attributes, &config, 1, &count) && count > 0) {
    EX context = create_context(display, config, NULL, context_attributes);
    if (context) {
      if (make_current(display, NULL, NULL, context)) {
        const struct { const char *label; GLenum name; } strings[] = {
          {"OpenGL ES Version", 0x1F02}, {"GL Renderer", 0x1F01}, {"GL Vendor", 0x1F00}
        };
        for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
          const unsigned char *value = get_string(strings[i].name);
          LOG("%-17s: %s", strings[i].label, value ? (const char *)value : "Unknown");
        }
        make_current(display, NULL, NULL, NULL);
      }
      destroy_context(display, context);
    }
  }
  terminate(display);
cleanup:
  if (hg) dlclose(hg);
  if (he) dlclose(he);
}
#endif

int cmd_info(void) {
  LOG("=== PSVR2 Hardware & System Information ===");
  LOG("Build firmware   : %s", OPEN_VRHMD_FIRMWARE);
  char model[128] = "SIE VR HMD";
  read_text("/sys/firmware/devicetree/base/model", model, sizeof(model));
  LOG("Board Model      : %s", model);

  char line[256];
  int cores = 0;
  FILE *file = fopen("/proc/cpuinfo", "r");
  if (file) {
    while (fgets(line, sizeof(line), file))
      if (strncmp(line, "processor", 9) == 0) cores++;
    fclose(file);
  }
  LOG("CPU Type         : MediaTek MT3612 (ARM Cortex-A35, %d Cores)", cores ? cores : 4);
  file = fopen("/proc/meminfo", "r");
  if (file) {
    long total = 0, available = 0;
    while (fgets(line, sizeof(line), file)) {
      if (sscanf(line, "MemTotal: %ld kB", &total) == 1) continue;
      if (sscanf(line, "MemAvailable: %ld kB", &available) == 1) continue;
    }
    fclose(file);
    LOG("RAM Total        : %ld MB", total / 1024);
    LOG("RAM Available    : %ld MB", available / 1024);
  }
  struct statvfs storage;
  if (statvfs("/tmp", &storage) == 0) {
    const double gib = 1024.0 * 1024.0 * 1024.0;
    LOG("/tmp Storage     : %.2f GB Total / %.2f GB Free",
        (double)storage.f_blocks * storage.f_frsize / gib,
        (double)storage.f_bavail * storage.f_frsize / gib);
  }
  LOG("GPU Type         : PowerVR Rogue GE8430");
#ifdef GPU_RENDER
  report_gl_info();
#endif
  char value[32] = "Unknown";
  if (read_text(GPU_DEVFREQ "max_freq", value, sizeof(value)) == 0)
    LOG("GPU Max Freq     : %s Hz", value);
  if (read_text("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq", value, sizeof(value)) == 0)
    LOG("CPU Cur Freq     : %ld MHz", strtol(value, NULL, 10) / 1000);
  if (read_text(GPU_DEVFREQ "cur_freq", value, sizeof(value)) == 0)
    LOG("GPU Cur Freq     : %ld MHz", strtol(value, NULL, 10) / 1000000);

  int fd = open("/dev/mtk_wrapper_thermal", O_RDWR | O_CLOEXEC);
  if (fd >= 0) {
    int max_temp = -999999;
    for (int sensor = 0; sensor < 8; sensor++) {
      struct thermal_get_from_id args = {.in_bank_id = 0, .in_sensor_id = sensor};
      if (ioctl(fd, THERMAL_GET_FROM_ID_CMD, &args) == 0 && args.out_temperature > max_temp)
        max_temp = args.out_temperature;
    }
    close(fd);
    if (max_temp != -999999) LOG("SOC Temperature  : %.1f C", max_temp / 1000.0);
  }
  return 0;
}
