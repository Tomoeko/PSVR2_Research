/* Real POSIX locks/signals, synthetic Linux /proc identity on any host OS. */
#define open_devices target_open_devices
#define close_devices target_close_devices
#include "../../target/psvr2/tools/open_vrhmd/core/device.c"
#undef open_devices
#undef close_devices

__attribute__((constructor)) static void prepare_process_identity(void) {
  char directory[4096], path[4096];
  int length = snprintf(directory, sizeof(directory), OPEN_VRHMD_PROC_ROOT "/%ld",
                        (long)getpid());
  if (length < 0 || (size_t)length >= sizeof(directory)) abort();
  if (mkdir(directory, 0700) < 0 && errno != EEXIST) abort();
  length = snprintf(path, sizeof(path), "%s/exe", directory);
  if (length < 0 || (size_t)length >= sizeof(path)) abort();
  const char *executable = getenv("TEST_EXE_MISMATCH") ? "/bin/sh" : getenv("TEST_BINARY");
  if (!executable || symlink(executable, path) < 0) abort();
  length = snprintf(path, sizeof(path), "%s/stat", directory);
  if (length < 0 || (size_t)length >= sizeof(path)) abort();
  FILE *file = fopen(path, "w");
  if (!file) abort();
  fprintf(file, "%ld (test (comm)) S", (long)getpid());
  for (int field = 4; field <= 22; ++field)
    fprintf(file, " %d", field == 22 ? 12345 : 0);
  fputc('\n', file);
  fclose(file);
}
