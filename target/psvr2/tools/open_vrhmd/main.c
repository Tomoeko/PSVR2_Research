#include "open_vrhmd.h"

#ifdef GPU_RENDER
GLuint g_pbos[PBO_COUNT];
int g_pbo_write_idx;
int g_pbo_read_idx;
sem_t g_sem_free;
sem_t g_sem_full;
pthread_t g_mirror_thread;
_Atomic bool g_mirror_running;
const char *g_image_path;
const char *g_video_path;
int g_stream;
int g_stream_fd = -1;
uint8_t *g_vhdr_packets[PBO_COUNT];
volatile sig_atomic_t g_bench_stop;
#endif

enum command_kind { DISPLAY, AUDIO, TONE, STOP, INFO };
struct command {
  const char *name;
  const char *arguments;
  const char *description;
  enum command_kind kind;
  int pattern;
  int min_args;
  int max_args;
};

static const struct command commands[] = {
  {"go", "[R G B] [--stream]", "Full display pipeline (RGB 0-255)", DISPLAY, 0, 0, 3},
  {"gradient", "[--stream]", "Rainbow gradient test pattern", DISPLAY, 1, 0, 0},
#ifdef GPU_RENDER
  {"gpu", "[--stream]", "GPU-rendered gradient", DISPLAY, 2, 0, 0},
  {"bench", "[--stream]", "Rotating cubes and FPS counter", DISPLAY, 3, 0, 0},
  {"image", "<path> [--stream]", "PNG/JPEG image viewer", DISPLAY, 4, 1, 1},
  {"video", "<path> [--stream]", "MPEG or packed monochrome video", DISPLAY, 5, 1, 1},
#endif
  {"audio", "<path> [volume]", "WAV/MP3 playback (0-100%, default 50%)", AUDIO, 0, 1, 2},
  {"tone", "[seconds]", "Hardware sine generator (1-30s, default 5s)", TONE, 0, 0, 1},
  {"stop", "", "Display and audio teardown", STOP, 0, 0, 0},
  {"info", "", "Hardware and OpenGL diagnostics", INFO, 0, 0, 0},
};

static void usage(const char *program) {
  fprintf(stderr, "Usage: %s <command>\n", program);
  for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
    fprintf(stderr, "  %-9s %-20s %s\n", commands[i].name,
            commands[i].arguments, commands[i].description);
}

static int parse_number(const char *text, int minimum, int maximum, int *value) {
  char *end;
  errno = 0;
  long number = strtol(text, &end, 10);
  if (errno || end == text || *end || number < minimum || number > maximum) {
    LOG("Invalid number '%s': expected %d-%d", text, minimum, maximum);
    return -1;
  }
  *value = (int)number;
  return 0;
}

/* Prefer CWD, then the executable directory for bundled sample files. */
static int resolve_audio_path(const char *program, const char *argument,
                              char *path, size_t size) {
  if (argument[0] == '/' || access(argument, F_OK) == 0) {
    if (snprintf(path, size, "%s", argument) < (int)size) return 0;
  } else {
    char executable[4096];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length >= 0) executable[length] = '\0';
    else if (snprintf(executable, sizeof(executable), "%s", program) >= (int)sizeof(executable))
      return -1;
    char *slash = strrchr(executable, '/');
    if (slash) {
      slash[1] = '\0';
      if (snprintf(path, size, "%s%s", executable, argument) < (int)size) return 0;
    } else if (snprintf(path, size, "%s", argument) < (int)size) return 0;
  }
  LOG("Audio path is too long");
  return -1;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }
  if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
    usage(argv[0]);
    return 0;
  }
  const struct command *command = NULL;
  for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
    if (strcmp(argv[1], commands[i].name) == 0) {
      command = &commands[i];
      break;
    }
  }
  if (!command) {
    LOG("Unknown command: %s", argv[1]);
    usage(argv[0]);
    return 1;
  }

#ifdef GPU_RENDER
  int positional = 2;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--stream") == 0) g_stream = 1;
    else argv[positional++] = argv[i];
  }
  argc = positional;
  if (g_stream && command->kind != DISPLAY) {
    LOG("--stream requires a display command");
    return 1;
  }
#endif
  int arguments = argc - 2;
  if (arguments < command->min_args || arguments > command->max_args ||
      (command->kind == DISPLAY && command->pattern == 0 && arguments != 0 && arguments != 3)) {
    LOG("Usage: %s %s %s", argv[0], command->name, command->arguments);
    return 1;
  }
  int rgb[3] = {255, 0, 0};
  int volume = 50, duration = 5;
  char audio_path[4096];
  if (command->kind == DISPLAY && command->pattern == 0 && arguments == 3) {
    for (int i = 0; i < 3; i++)
      if (parse_number(argv[i + 2], 0, 255, &rgb[i]) < 0) return 1;
  }
  if (command->kind == AUDIO) {
    if (arguments == 2 && parse_number(argv[3], 0, 100, &volume) < 0) return 1;
    if (resolve_audio_path(argv[0], argv[2], audio_path, sizeof(audio_path)) < 0) return 1;
  }
  if (command->kind == TONE && arguments == 1 &&
      parse_number(argv[2], 1, 30, &duration) < 0) return 1;
#ifdef GPU_RENDER
  if (command->kind == DISPLAY && command->pattern == 4) g_image_path = argv[2];
  if (command->kind == DISPLAY && command->pattern == 5) g_video_path = argv[2];
#endif
  LOG("PSVR2 display/audio pipeline — firmware %s", OPEN_VRHMD_FIRMWARE);
  if (command->kind == INFO) return cmd_info() == 0 ? 0 : 1;
  if (application_lock_acquire(command->kind == STOP) < 0) {
    ERR("acquire application ownership");
    return 1;
  }
  if (command->kind != STOP) perform_tuning();
  if (open_devices() < 0) {
    application_lock_release();
    return 1;
  }

  int ret = -1;
  if (application_stop_requested) {
    ret = cmd_stop();
    goto done;
  }
  switch (command->kind) {
  case DISPLAY:
    ret = fan_ctrl_init();
    if (ret < 0) {
      ERR("initialize fan cooling");
      cmd_stop();
      break;
    }
    ret = cmd_go((uint8_t)rgb[0], (uint8_t)rgb[1], (uint8_t)rgb[2], command->pattern);
    break;
  case AUDIO:
    LOG("Audio mode: file=%s volume=%d%%", audio_path, volume);
    ret = audio_init(volume);
    if (ret == 0 && !application_stop_requested)
      ret = audio_play_file(audio_path, volume);
    audio_cleanup();
    break;
  case TONE:
    ret = audio_init(50);
    if (ret == 0 && !application_stop_requested) ret = audio_play_tone(duration);
    audio_cleanup();
    break;
  case STOP:
    ret = cmd_stop();
    break;
  case INFO:
    break;
  }
done:
  close_devices();
  application_lock_release();
  return ret == 0 ? 0 : 1;
}
