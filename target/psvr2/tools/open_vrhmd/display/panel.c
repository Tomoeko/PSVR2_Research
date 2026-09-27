#include "../open_vrhmd.h"
#include "panel.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define CMD(c, n, ...) { .cmd = (c), .count = (n), .para_list = { __VA_ARGS__ } }
#define UNLOCK CMD(0xF0, 2, 0x5A, 0x5A)
#define LOCK CMD(0xF0, 2, 0xA5, 0xA5)
/* VM unlock/lock payloads include the command byte, as in the captured sequence. */
#define VM_UNLOCK CMD(0xF0, 3, 0xF0, 0x5A, 0x5A)
#define VM_LOCK CMD(0xF0, 3, 0xF0, 0xA5, 0xA5)

static const struct lcm_setting_entry init_reset[] = {
    UNLOCK, CMD(0x01, 0, 0), CMD(REGFLAG_DELAY, 10, 0), CMD(0x9D, 1, 0), LOCK};
static const struct lcm_setting_entry init_f6[] = {
    UNLOCK, CMD(0xB0, 1, 0x2E), CMD(0xF6, 1, 0x03), LOCK};
static const struct lcm_setting_entry init_dsc[] = {UNLOCK, CMD(0x9D, 1, 1)};
static const struct lcm_setting_entry init_pps[] = {
    CMD(0x9E, 88,
        0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x07, 0xF8, 0x03, 0xE8, 0x00,
        0x66, 0x03, 0xE8, 0x03, 0xE8, 0x02, 0x00, 0x03, 0x73, 0x00, 0x20,
        0x0B, 0x2F, 0x00, 0x0D, 0x00, 0x0F, 0x01, 0x31, 0x00, 0x8A, 0x18,
        0x00, 0x10, 0xF0, 0x03, 0x0C, 0x20, 0x00, 0x06, 0x0B, 0x0B, 0x33,
        0x0E, 0x1C, 0x2A, 0x38, 0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79,
        0x7B, 0x7D, 0x7E, 0x01, 0x02, 0x01, 0x00, 0x09, 0x40, 0x09, 0xBE,
        0x19, 0xFC, 0x19, 0xFA, 0x19, 0xF8, 0x1A, 0x38, 0x1A, 0x78, 0x1A,
        0xB6, 0x2A, 0xB6, 0x2A, 0xF4, 0x2A, 0xF4, 0x4B, 0x34, 0x63, 0x74)};
static const struct lcm_setting_entry init_lock[] = {LOCK};
static const struct lcm_setting_entry init_timing[] = {
    UNLOCK, CMD(0xC4, 1, 0x02), LOCK};
static const struct lcm_setting_entry nop[] = {CMD(0, 0, 0)};
static const struct lcm_setting_entry vm_unmute[] = {
    VM_UNLOCK, CMD(0xB0, 1, 0x50), CMD(0xC3, 1, 0), CMD(0xF7, 1, 0x02), VM_LOCK};
static const struct lcm_setting_entry vm_sleep_out[] = {
    CMD(0x11, 0, 0), CMD(REGFLAG_DELAY, 25, 0), VM_UNLOCK,
    CMD(0xE8, 5, 0xE8, 0x64, 0x88, 0x00, 0x0C), VM_LOCK};
static const struct lcm_setting_entry vm_brightness[] = {
    CMD(0x53, 1, 0x28), CMD(0x51, 3, 0x51, 0xFF, 0x03), CMD(0x80, 1, 0)};
static const struct lcm_setting_entry vm_eb[] = {
    VM_UNLOCK, CMD(0xB0, 1, 0x2E), CMD(0xEB, 1, 0x05), VM_LOCK};
static const struct lcm_setting_entry vm_f2[] = {
    VM_UNLOCK, CMD(0xB0, 1, 0x08), CMD(0xF2, 1, 0), VM_LOCK};

struct panel_batch {
  const char *name;
  const struct lcm_setting_entry *table;
  int count;
};
#define BATCH(name, table) {name, table, ARRAY_COUNT(table)}
static const struct panel_batch init_batches[] = {
    BATCH("unlock+reset+DSCoff", init_reset), BATCH("F6[2E]=03", init_f6),
    BATCH("DSC enable", init_dsc), BATCH("PPS 0x9E", init_pps),
    BATCH("lock", init_lock), BATCH("C4=02", init_timing)};
static const struct panel_batch after_read_batches[] = {BATCH("NOP", nop)};
static const struct panel_batch after_start_batches[] = {
    BATCH("video unmute", vm_unmute), BATCH("sleep out+E8", vm_sleep_out)};
static const struct panel_batch after_mutex_batches[] = {
    BATCH("brightness", vm_brightness), BATCH("EB[2E]=05", vm_eb),
    BATCH("F2[08]=00", vm_f2), BATCH("NOP", nop)};

int panel_send_commands(enum panel_command_stage stage) {
  const struct panel_batch *batches;
  size_t count;
  int vm_mode = 0;
  switch (stage) {
  case PANEL_COMMAND_INIT:
    batches = init_batches;
    count = ARRAY_COUNT(init_batches);
    break;
  case PANEL_COMMAND_AFTER_READ:
    batches = after_read_batches;
    count = ARRAY_COUNT(after_read_batches);
    break;
  case PANEL_COMMAND_AFTER_START:
    batches = after_start_batches;
    count = ARRAY_COUNT(after_start_batches);
    vm_mode = 1;
    break;
  case PANEL_COMMAND_AFTER_MUTEX:
    batches = after_mutex_batches;
    count = ARRAY_COUNT(after_mutex_batches);
    vm_mode = 1;
    break;
  default:
    errno = EINVAL;
    return -1;
  }

  for (size_t i = 0; i < count; ++i) {
    struct lcm_param_ioctl args = {batches[i].table, batches[i].count, vm_mode};
    int ret = ioctl(fd_dsi, DSI_SET_LCM_PARAM, &args);
    LOG("  Panel %s: ret=%d", batches[i].name, ret);
    if (ret < 0)
      return -1;
  }
  return 0;
}
