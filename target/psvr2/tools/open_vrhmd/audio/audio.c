/*
 * audio.c — PSVR2 Audio WAV/MP3 Playback (Userspace)
 *
 * Plays PCM WAV files and decoded MP3 through WM1801 at controlled volume.
 *
 * Since the stock retail sieaudio.ko does NOT have playback ioctls compiled in
 * (CONFIG_SIE_DEVELOP_BUILD was not set), we leverage the TRANSFER_START path
 * to start the DMA pipeline. This path sets up:
 *   - I2S output enabled
 *   - DL12 DMA reading from AFE SRAM
 *   - Connection mux: O0=I0, O1=I1 (DL12 → I2S out → WM1801 codec)
 *
 * Strategy:
 *   1. Open /dev/audio, call PREPARE (available in retail) to ensure AFE is up
 *   2. Map AFE SRAM via /dev/mem and overwrite the DL12 buffer with PCM
 *   3. Refill the half-buffer opposite the hardware DMA cursor
 *
 * Hardware details (from DT and kernel source):
 *   AFE registers: 0x10c00000, size 0x1000
 *   AFE SRAM:      0x10c01000, size 0x2400 (9216 bytes total)
 *   DL12 SRAM:     First 4608 bytes (kernel: "DL Use SRAM: size:4608")
 *   DL12 format:   S32_LE, 2ch, 48kHz, period=128 samples
 *
 * CRITICAL: AFE SRAM is mapped as Device memory on ARM64. Only 32-bit aligned
 * stores are safe. NEON/SIMD (memset, memcpy) will cause alignment faults!
 *
 * Volume safety:
 *   All samples are scaled by (volume_pct / 100) before writing.
 *   Default = 50% to protect the WM1801 headphone amplifier.
 */

#include "audio.h"
#include "codec_path.h"
#include "../open_vrhmd.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "../codec/mp3.h"


/* ═══════════════════════════════════════════════════════════════
 *  MT3612 AFE Register / Memory Map (from device tree + source)
 * ═══════════════════════════════════════════════════════════════ */
#define AFE_REG_BASE 0x10c00000ULL
#define AFE_REG_SIZE 0x1000
#define AFE_SRAM_BASE 0x10c01000ULL
#define AFE_SRAM_SIZE 0x2400 /* 9216 bytes total */

/* DL12 gets the first half of SRAM (kernel log: "DL Use SRAM: size:4608") */
#define DL12_SRAM_SIZE 4608
#define DL12_FRAME_BYTES 8 /* 2ch × 4 bytes (S32_LE) */
#define DL12_TOTAL_FRAMES (DL12_SRAM_SIZE / DL12_FRAME_BYTES) /* 576 frames */

/* AFE registers (offsets from AFE_REG_BASE) */
#define AFE_DAC_CON0 0x0010
#define AFE_DL12_BASE_R 0x0340
#define AFE_DL12_CUR_R 0x0344
#define AFE_DL12_END_R 0x0348

/* Connection mux — for routing DL12 to I2S output */
#define AFE_CONN_MUX_CFG 0x0af8

/* Sine generator (hardware tone test — bypasses SRAM entirely) */
#define AFE_SGEN_CON0 0x01f0
#define AFE_SGEN_CON2 0x01dc
#define SGEN_CON2_EN 0x23
#define SGEN_CON2_EN_MASK 0x3F
#define SGEN_CON0_EN_CUST 0x04A61A61 /* 48kHz sine, both channels */

/* ═══════════════════════════════════════════════════════════════
 *  WM1801 Codec (Wolfson) — I2C Interface
 *
 *  Reversed from vrhmd_main.elf (sub_13B4C, sub_134E8, sub_13724).
 *  I2C bus: /dev/i2c-0, slave address: 0x4A
 *  Protocol: 8-bit register + 16-bit data (big-endian)
 * ═══════════════════════════════════════════════════════════════ */
#define WM1801_I2C_BUS 0
#define WM1801_I2C_ADDR 0x4A

/* Exact 06.00 vrhmd_main.elf codec helpers: headphone gain uses 2/3,
 * DAC gain uses 10/11. Registers 57/58 select ADC sidetone sources. */
#define WM1801_DAC_CONTROL 5
#define WM1801_POWER_2 26
#define WM1801_HEADPHONE_CONTROL 69
#define WM1801_DAC_VOLUME_LEFT 10
#define WM1801_DAC_VOLUME_RIGHT 11
#define WM1801_SIDETONE_LEFT 57
#define WM1801_SIDETONE_RIGHT 58
#define WM1801_VOLUME_UPDATE 0x100
#define WM1801_DAC_0DB 0xC0

static int fd_i2c = -1;
static uint16_t headphone_volume;

static int clamp_volume(int volume_pct) {
  if (volume_pct < 0)
    return 0;
  return volume_pct > 100 ? 100 : volume_pct;
}

/* Write a 16-bit value to a WM1801 register.
 * Wire format: [reg_addr] [data_hi] [data_lo] */
static int wm1801_write(uint8_t reg, uint16_t val) {
  uint8_t buf[3] = {reg, (val >> 8) & 0xFF, val & 0xFF};
  if (write(fd_i2c, buf, 3) != 3) {
    ERR("WM1801: write reg %d = 0x%04x failed (errno=%d)", reg, val, errno);
    return -1;
  }
  return 0;
}

/* Read a 16-bit value from a WM1801 register. */
static int wm1801_read(uint8_t reg, uint16_t *val) {
  struct i2c_msg msgs[2];
  struct i2c_rdwr_ioctl_data rdwr;

  uint8_t reg_buf = reg;
  uint8_t data_buf[2] = {0};

  msgs[0].addr = WM1801_I2C_ADDR;
  msgs[0].flags = 0; /* write */
  msgs[0].len = 1;
  msgs[0].buf = &reg_buf;

  msgs[1].addr = WM1801_I2C_ADDR;
  msgs[1].flags = I2C_M_RD; /* read */
  msgs[1].len = 2;
  msgs[1].buf = data_buf;

  rdwr.msgs = msgs;
  rdwr.nmsgs = 2;

  if (ioctl(fd_i2c, I2C_RDWR, &rdwr) < 0) {
    ERR("WM1801: read reg %d failed (errno=%d)", reg, errno);
    return -1;
  }
  *val = (data_buf[0] << 8) | data_buf[1];
  return 0;
}

/* Read-modify-write a WM1801 register: reg = (reg & ~mask) | (mask & value) */
static int wm1801_rmw(uint8_t reg, uint16_t mask, uint16_t value) {
  uint16_t cur;
  if (wm1801_read(reg, &cur) < 0)
    return -1;
  uint16_t newval = (cur & ~mask) | (mask & value);
  return wm1801_write(reg, newval);
}

static int wm1801_update(void *context, uint8_t reg, uint16_t mask,
                         uint16_t value) {
  (void)context;
  return wm1801_rmw(reg, mask, value);
}

/* Initialize the WM1801 codec — replicated from vrhmd_main.elf sub_13B4C.
 * This powers on the DAC, headphone amp, sets I2S format, and unmutes. */
static int wm1801_init(int volume_pct) {
  uint16_t vol_val;
  if (audio_codec_headphone_code(volume_pct, &vol_val) < 0)
    return -1;
  char i2c_path[32];
  snprintf(i2c_path, sizeof(i2c_path), "/dev/i2c-%d", WM1801_I2C_BUS);

  fd_i2c = open(i2c_path, O_RDWR);
  if (fd_i2c < 0) {
    ERR("Cannot open %s (errno=%d)", i2c_path, errno);
    return -1;
  }

  if (ioctl(fd_i2c, I2C_SLAVE, WM1801_I2C_ADDR) < 0) {
    ERR("Cannot set I2C slave addr 0x%02x (errno=%d)", WM1801_I2C_ADDR, errno);
    close(fd_i2c);
    fd_i2c = -1;
    return -1;
  }

  /* Verify WM1801 presence — read Device ID register (reg 1) */
  uint16_t dev_id = 0;
  if (wm1801_read(1, &dev_id) < 0) {
    ERR("WM1801 not responding on I2C");
    close(fd_i2c);
    fd_i2c = -1;
    return -1;
  }
  LOG("WM1801 Device ID: 0x%04x (rev %d)", dev_id, (dev_id >> 9) & 7);
  fflush(stdout);

  /* Stock headphone power sequence, without ambient microphone monitoring. */

  /* Step 1: Software reset */
  if (wm1801_write(15, 0) < 0)
    goto fail;
  LOG("WM1801: software reset sent");
  fflush(stdout);
  usleep(10000); /* 10ms */

  /* Step 2: Power management */
  if (wm1801_write(25, 234) < 0) /* 0xEA — power mgmt 1 */
    goto fail;
  if (wm1801_write(28, 24) < 0) /* 0x18 — anti-pop */
    goto fail;
  LOG("WM1801: power mgmt registers written");
  fflush(stdout);
  usleep(50000); /* 50ms for power-up */

  /* Step 3: Configure basic output routing */
  if (wm1801_write(82, 0) < 0) /* Reg 0x52 */
    goto fail;
  if (wm1801_write(WM1801_SIDETONE_LEFT, 0) < 0 ||
      wm1801_write(WM1801_SIDETONE_RIGHT, 0) < 0)
    goto fail;
  if (wm1801_write(71, 435) < 0) /* Reg 0x47 = 0x01B3 */
    goto fail;
  if (wm1801_write(32, 16) < 0) /* Reg 0x20 = 0x10 (ADC input path) */
    goto fail;
  if (wm1801_rmw(48, 1, 0) < 0) /* Reg 0x30: clear bit 0 */
    goto fail;

  /* Set both gains before powering/unmuting the outputs, matching Sony's
   * initialization order. Retain the player's attenuated headphone ceiling. */
  headphone_volume = vol_val;
  if (wm1801_write(2, vol_val | 0x80) < 0 ||
      wm1801_write(3, vol_val | 0x80 | WM1801_VOLUME_UPDATE) < 0)
    goto fail;
  if (wm1801_write(WM1801_DAC_VOLUME_LEFT, WM1801_DAC_0DB) < 0 ||
      wm1801_write(WM1801_DAC_VOLUME_RIGHT,
                   WM1801_DAC_0DB | WM1801_VOLUME_UPDATE) < 0)
    goto fail;

  /* Step 4: Headphone power sequencer */
  if (wm1801_write(87, 48) < 0) /* Reg 0x57 = 0x30 */
    goto fail;
  if (wm1801_write(88, 256) < 0) /* Reg 0x58 = 0x100 */
    goto fail;
  if (wm1801_write(90, 128) < 0) /* Reg 0x5A = 0x80 */
    goto fail;

  /* Step 6: Wait for VMID charge (CRITICAL) */
  LOG("WM1801: waiting 400ms for VMID charge...");
  fflush(stdout);
  usleep(400000);

  /* The sequencer may overwrite routing and gain. Reassert playback without
   * modifying the microphone PGA, and clear the actual DAC mute last. */
  if (audio_codec_start_playback(NULL, wm1801_update, vol_val) < 0)
    goto fail;

  LOG("WM1801: init complete, volume=%d%% (val=0x%02x)", volume_pct, vol_val);
  fflush(stdout);

  return 0;

fail:
  {
    int saved_errno = errno;
    audio_codec_mute_playback(NULL, wm1801_update);
    close(fd_i2c);
    errno = saved_errno;
  }
  fd_i2c = -1;
  return -1;
}

static int fd_audio = -1;
static int fd_mem = -1;
static volatile sig_atomic_t g_audio_stop = 0;
static struct sigaction saved_sigint;
static struct sigaction saved_sigterm;
static int signals_installed = 0;

static void audio_sigint(int sig) {
  (void)sig;
  g_audio_stop = 1;
  application_stop_requested = 1;
}

static int audio_stop_requested(void) {
  return g_audio_stop || application_stop_requested;
}

static int install_signal_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = audio_sigint;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGINT, &sa, &saved_sigint) < 0)
    return -1;
  if (sigaction(SIGTERM, &sa, &saved_sigterm) < 0) {
    sigaction(SIGINT, &saved_sigint, NULL);
    return -1;
  }
  signals_installed = 1;
  return 0;
}

static void restore_signal_handlers(void) {
  if (signals_installed) {
    sigaction(SIGINT, &saved_sigint, NULL);
    sigaction(SIGTERM, &saved_sigterm, NULL);
    signals_installed = 0;
  }
}

static void check_stdin_for_stop(void) {
  struct timeval tv = {0, 0};
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);

  if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
    if (FD_ISSET(STDIN_FILENO, &fds)) {
      char buf[64];
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n > 0) {
        g_audio_stop = 1;
      }
    }
  }
}

static volatile uint32_t *afe_regs = NULL;
static volatile uint32_t *afe_sram = NULL;

/* ═══════════════════════════════════════════════════════════════
 *  Device-memory safe operations (NO NEON / NO memset / NO memcpy)
 *
 *  ARM64 Device memory only supports single-element aligned loads/stores.
 *  Using memset/memcpy triggers NEON/SIMD → alignment fault.
 * ═══════════════════════════════════════════════════════════════ */
static void sram_zero(volatile uint32_t *dst, int n_words) {
  for (int i = 0; i < n_words; i++)
    dst[i] = 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  Direct AFE register access via /dev/mem
 * ═══════════════════════════════════════════════════════════════ */
static void unmap_afe(void);

static int map_afe(void) {
  fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
  if (fd_mem < 0) {
    ERR("Cannot open /dev/mem for AFE access");
    return -1;
  }

  afe_regs =
      (volatile uint32_t *)mmap(NULL, AFE_REG_SIZE, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd_mem, AFE_REG_BASE);
  if (afe_regs == MAP_FAILED) {
    ERR("mmap AFE regs at 0x%llx", (unsigned long long)AFE_REG_BASE);
    afe_regs = NULL;
    unmap_afe();
    return -1;
  }

  afe_sram =
      (volatile uint32_t *)mmap(NULL, AFE_SRAM_SIZE, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd_mem, AFE_SRAM_BASE);
  if (afe_sram == MAP_FAILED) {
    ERR("mmap AFE SRAM at 0x%llx", (unsigned long long)AFE_SRAM_BASE);
    afe_sram = NULL;
    unmap_afe();
    return -1;
  }

  LOG("AFE mapped: regs=%p sram=%p", afe_regs, afe_sram);
  return 0;
}

static void unmap_afe(void) {
  if (afe_regs) {
    munmap((void *)afe_regs, AFE_REG_SIZE);
    afe_regs = NULL;
  }
  if (afe_sram) {
    munmap((void *)afe_sram, AFE_SRAM_SIZE);
    afe_sram = NULL;
  }
  if (fd_mem >= 0) {
    close(fd_mem);
    fd_mem = -1;
  }
}

static inline uint32_t afe_read(uint32_t offset) {
  return afe_regs[offset / 4];
}

static inline void afe_write(uint32_t offset, uint32_t val) {
  afe_regs[offset / 4] = val;
}

static inline void afe_rmw(uint32_t offset, uint32_t mask, uint32_t val) {
  uint32_t cur = afe_read(offset);
  afe_write(offset, (cur & ~mask) | (val & mask));
}

/* ═══════════════════════════════════════════════════════════════
 *  WAV file parsing
 * ═══════════════════════════════════════════════════════════════ */
static uint16_t read_le16(const uint8_t *p) {
  return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

struct wav_format {
  uint16_t channels;
  uint16_t bits_per_sample;
  uint32_t sample_rate;
  uint16_t block_align;
};

/* Walk RIFF chunks, including their required odd-byte padding. Bounds are
 * checked before seeking so a truncated file cannot masquerade as valid PCM. */
static int parse_wav_header(FILE *fp, struct wav_format *fmt,
                            uint32_t *data_size) {
  uint8_t riff[12];
  if (fread(riff, 1, sizeof(riff), fp) != sizeof(riff) ||
      memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4)) {
    ERR("Not a valid RIFF/WAVE file");
    return -1;
  }

  uint32_t riff_size = read_le32(riff + 4);
  if (riff_size < 4 || fseeko(fp, 0, SEEK_END) < 0)
    return -1;
  off_t file_size = ftello(fp);
  if (file_size < 0 || (uint64_t)riff_size + 8 > (uint64_t)file_size ||
      fseeko(fp, sizeof(riff), SEEK_SET) < 0) {
    ERR("Truncated WAV RIFF container");
    return -1;
  }

  uint64_t remaining = riff_size - 4;
  int have_format = 0;
  while (remaining >= 8) {
    uint8_t chunk[8];
    if (fread(chunk, 1, sizeof(chunk), fp) != sizeof(chunk))
      break;
    remaining -= sizeof(chunk);
    uint32_t chunk_size = read_le32(chunk + 4);
    uint64_t padded_size = (uint64_t)chunk_size + (chunk_size & 1);
    if (padded_size > remaining) {
      ERR("WAV chunk exceeds the RIFF container");
      return -1;
    }

    if (!memcmp(chunk, "fmt ", 4)) {
      uint8_t pcm[16];
      if (chunk_size < sizeof(pcm) ||
          fread(pcm, 1, sizeof(pcm), fp) != sizeof(pcm)) {
        ERR("Truncated WAV format chunk");
        return -1;
      }
      fmt->channels = read_le16(pcm + 2);
      fmt->sample_rate = read_le32(pcm + 4);
      fmt->block_align = read_le16(pcm + 12);
      fmt->bits_per_sample = read_le16(pcm + 14);
      if (read_le16(pcm) != 1 ||
          (fmt->channels != 1 && fmt->channels != 2) ||
          (fmt->bits_per_sample != 16 && fmt->bits_per_sample != 32) ||
          !fmt->sample_rate ||
          fmt->block_align != fmt->channels * (fmt->bits_per_sample / 8)) {
        ERR("WAV requires mono/stereo PCM with 16-bit or 32-bit samples");
        return -1;
      }
      have_format = 1;
      if (fseeko(fp, (off_t)(padded_size - sizeof(pcm)), SEEK_CUR) < 0)
        return -1;
    } else if (!memcmp(chunk, "data", 4)) {
      if (!have_format || chunk_size % fmt->block_align) {
        ERR("WAV data has no valid format or contains an incomplete frame");
        return -1;
      }
      *data_size = chunk_size;
      return 0;
    } else if (fseeko(fp, (off_t)padded_size, SEEK_CUR) < 0) {
      return -1;
    }
    remaining -= padded_size;
  }
  ERR("Failed to find WAV data chunk");
  return -1;
}

static int send_audio_patch(void) {
  FILE *fp = fopen("/proc/stage3", "w");
  if (!fp)
    return -1;
  int write_failed = fputs("audiopatch", fp) == EOF;
  int close_failed = fclose(fp) == EOF;
  if (write_failed || close_failed) {
    ERR("Failed to send stage3 audiopatch");
    return -1;
  }
  return 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  Audio init / cleanup
 *
 *  The retail kernel's transfer_start has TWO gates:
 *    1. audio_io_transfer_start() rejects port_type != DP (EPERM)
 *    2. mt_afe_transfer_start() has "no port change" short path
 *       that skips DMA start when transfer_port == port_type
 *
 *  Since transfer_port persists as 1 (DP) across open/close,
 *  TRANSFER_START(port=1) always hits the short path → DMA stuck.
 *
 *  FIX: Use stage3's audiopatch command (echo audiopatch > /proc/stage3)
 *  to reset afe_data.transfer_port to 0 in kernel memory. Then
 *  TRANSFER_START(port=1) passes the EPERM check AND takes the
 *  full path (0 != 1) including DMA start.
 *
 *  Sequence: PREPARE → stage3 audiopatch → TRANSFER_START(port=1)
 * ═══════════════════════════════════════════════════════════════ */
int audio_init(int volume_pct) {
  /* A retry releases the previous instance before acquiring new descriptors. */
  audio_cleanup();
  volume_pct = clamp_volume(volume_pct);
  fd_audio = open("/dev/audio", O_RDWR);
  if (fd_audio < 0) {
    ERR("Cannot open /dev/audio");
    return -1;
  }

  /* Step 1: Initial reset via stage3.
   * If the driver is already in a prepared state (e.g., from a previous run),
   * this clears the state so we can cleanly start over.
   * If this is the first boot, it will silently fail (which is fine, port is
   * already 0). */
  if (send_audio_patch() == 0) {
    LOG("stage3 audiopatch (initial) sent");
    usleep(10000); /* 10ms for kernel to process */
  }

  /* Step 2: First PREPARE — sets up AFE clocks, DMA buffers, I2S config */
  int ret = ioctl(fd_audio, SIE_AUDIO_IO_PREPARE);
  if (ret < 0) {
    ERR("PREPARE ioctl failed: %d", ret);
    audio_cleanup();
    return -1;
  }
  LOG("Audio PREPARE OK");

  /* Step 3: Initialize WM1801 codec via I2C.
   * Use requested volume. */
  if (wm1801_init(volume_pct) < 0) {
    ERR("WM1801 codec init failed");
    audio_cleanup();
    return -1;
  }

  /* Step 4: TRANSFER_START with port_type=1 (DP).
   * Since transfer_port is guaranteed to be 0 now, the comparison 0!=1 forces
   * the full code path including mt_afe_dais_trigger → DMA start.
   * This WILL break the clock (sets to DP_PLL). */
  struct audio_io_transfer_start_req ts_req;
  ts_req.port_type = 1; /* SIE_AUDIO_INPUT_PORT_DP (required, validated) */
  ts_req.rate = 48000;
  ret = ioctl(fd_audio, SIE_AUDIO_IO_TRANSFER_START, &ts_req);
  if (ret < 0) {
    ERR("TRANSFER_START failed: %d", ret);
    audio_cleanup();
    return -1;
  } else {
    LOG("Audio TRANSFER_START OK (DMA started, but CCF is now on DP_PLL!)");
  }

  /* Step 5: Use stage3 audiopatch to clear afe_data.prepared and transfer_port.
   * Then call PREPARE again! Since prepared==0, the kernel will re-evaluate
   * backend_clk_src and set the CCF I2S MCLK parent BACK to APLL1 (audio PLL),
   * completely fixing the clock starvation without stopping the DMA! */
  if (send_audio_patch() == 0) {
    LOG("stage3 audiopatch (post-start) sent — prepared flag cleared!");
    usleep(10000);

    ret = ioctl(fd_audio, SIE_AUDIO_IO_PREPARE);
    if (ret < 0) {
      ERR("Second PREPARE ioctl failed: %d", ret);
      audio_cleanup();
      return -1;
    } else {
      LOG("Second PREPARE OK — CCF clocks restored to APLL1!");
    }
  } else {
    ERR("Cannot open /proc/stage3 — CCF clocks cannot be restored!");
  }

  /* Map AFE registers for direct SRAM access */
  if (map_afe() < 0) {
    audio_cleanup();
    return -1;
  }

  if (audio_codec_start_playback(NULL, wm1801_update, headphone_volume) < 0) {
    audio_cleanup();
    return -1;
  }

  LOG("AFE initialization complete.");
  return 0;
}

void audio_cleanup(void) {
  restore_signal_handlers();
  if (fd_i2c >= 0)
    audio_codec_mute_playback(NULL, wm1801_update);
  /* Zero the DL12 SRAM (word-by-word, device memory safe) */
  if (afe_sram) {
    sram_zero(afe_sram, DL12_SRAM_SIZE / 4);
  }

  if (fd_audio >= 0) {
    /* Stop transfer to cleanly release the I2S/DMA pipeline */
    ioctl(fd_audio, SIE_AUDIO_IO_TRANSFER_STOP);
    close(fd_audio);
    fd_audio = -1;
  }

  unmap_afe();

  if (fd_i2c >= 0) {
    close(fd_i2c);
    fd_i2c = -1;
  }
}

int audio_stop_hardware(void) {
  LOG("  [*] Stopping runaway audio hardware...");
  int fd = open("/dev/audio", O_RDWR);
  if (fd >= 0) {
    ioctl(fd, SIE_AUDIO_IO_TRANSFER_STOP);
    close(fd);
  }

  send_audio_patch();
  return 0;
}

/* Shared DL12 setup and double-buffered PCM streaming. Only aligned scalar
 * stores reach Device memory; conversion happens in ordinary stack memory. */
#define DL12_HALF_FRAMES (DL12_TOTAL_FRAMES / 2)
#define DL12_HALF_WORDS (DL12_HALF_FRAMES * AUDIO_DL12_CHANNELS)
#define DMA_WAIT_TIMEOUT_MS 1000
#define AUDIO_PROGRESS_INTERVAL_MS 5000

static int audio_is_ready(void) {
  if (afe_regs && afe_sram)
    return 1;
  ERR("Audio must be initialized before playback");
  return 0;
}

static void configure_dl12(void) {
  uint32_t sram_phys = (uint32_t)AFE_SRAM_BASE;
  afe_write(AFE_DL12_BASE_R, sram_phys);
  afe_write(AFE_DL12_END_R, sram_phys + DL12_SRAM_SIZE - 1);
  afe_rmw(AFE_DAC_CON0, 1U << 8, 1U << 8);
  afe_rmw(AFE_CONN_MUX_CFG, 0x0000FFFF, 0x00003210);
  sram_zero(afe_sram, DL12_SRAM_SIZE / sizeof(uint32_t));
}

static int64_t monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    return -1;
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A running DMA cursor does not prove that its output is audible: the retail
 * TRANSFER_STOP path can mute the mux without stopping DMA. Report that case
 * explicitly instead of silently consuming the rest of a file. */
static int report_audio_progress(uint64_t frames, int64_t elapsed_ms) {
  uint16_t power = 0, headphone = 0, dac = 0, left = 0, right = 0;
  uint32_t mux = afe_read(AFE_CONN_MUX_CFG);
  uint32_t cursor = afe_read(AFE_DL12_CUR_R);
  if (wm1801_read(WM1801_POWER_2, &power) < 0 ||
      wm1801_read(WM1801_HEADPHONE_CONTROL, &headphone) < 0 ||
      wm1801_read(WM1801_DAC_CONTROL, &dac) < 0 ||
      wm1801_read(WM1801_DAC_VOLUME_LEFT, &left) < 0 ||
      wm1801_read(WM1801_DAC_VOLUME_RIGHT, &right) < 0)
    return -1;
  LOG("Audio progress: %.1fs, %llu frames; DMA=0x%08x mux=0x%08x "
      "codec power=0x%04x HP=0x%04x DAC=0x%04x gain=0x%04x/0x%04x",
      elapsed_ms / 1000.0, (unsigned long long)frames, cursor, mux,
      power, headphone, dac, left, right);
  if (!(afe_read(AFE_DAC_CON0) & (1U << 8)) ||
      (mux & 0xFFFF) != 0x3210 || (power & 0x01e0) != 0x01e0 ||
      (headphone & 0x00ff) != 0x00ff || (dac & 0x8)) {
    errno = EIO;
    ERR("Audio output was disabled or muted during playback");
    return -1;
  }
  return 0;
}

/* Return 1 once DMA reaches the requested half, 0 on a stop request, or -1
 * if the hardware cursor never advances. Poll stdin during the wait too. */
static int wait_for_dma_half(int half) {
  uint32_t start = (uint32_t)AFE_SRAM_BASE + half * (DL12_SRAM_SIZE / 2);
  uint32_t end = start + DL12_SRAM_SIZE / 2;
  int64_t begin = monotonic_ms();
  if (begin < 0)
    return -1;
  while (!audio_stop_requested()) {
    check_stdin_for_stop();
    if (audio_stop_requested())
      break;
    uint32_t cur = afe_read(AFE_DL12_CUR_R);
    if (cur >= start && cur < end)
      return 1;
    int64_t now = monotonic_ms();
    if (now < 0 || now - begin >= DMA_WAIT_TIMEOUT_MS) {
      if (now >= 0)
        errno = ETIMEDOUT;
      ERR("DL12 DMA cursor stalled at 0x%08x", cur);
      return -1;
    }
    usleep(500);
  }
  return 0;
}

static int32_t pcm_sample(const uint8_t *src, int bits) {
  if (bits == 16) {
    /* Multiplication is defined for negative samples; signed left shift is not. */
    return (int32_t)(int16_t)read_le16(src) * 65536;
  }
  uint32_t word = read_le32(src);
  int32_t sample;
  memcpy(&sample, &word, sizeof(sample));
  return sample;
}

static void write_pcm_half(int half, const uint8_t *src, int frames,
                           int channels, int bits, int volume_pct) {
  int sample_bytes = bits / 8;
  int frame_bytes = channels * sample_bytes;
  volatile uint32_t *dst = &afe_sram[half * DL12_HALF_WORDS];
  for (int i = 0; i < frames; ++i, src += frame_bytes) {
    int32_t left = pcm_sample(src, bits);
    int32_t right = channels == 2 ? pcm_sample(src + sample_bytes, bits) : left;
    dst[i * 2] = (uint32_t)(((int64_t)left * volume_pct) / 100);
    dst[i * 2 + 1] = (uint32_t)(((int64_t)right * volume_pct) / 100);
  }
  sram_zero(dst + frames * 2, DL12_HALF_WORDS - frames * 2);
}

typedef int (*pcm_read_frames_fn)(void *source, void *buffer, int max_frames);

static int stream_pcm(void *source, pcm_read_frames_fn read_frames,
                      int channels, int bits, int volume_pct) {
  /* This union keeps the buffer suitably aligned for the MP3 decoder while
   * accommodating stereo S32 WAV input without heap allocations. */
  union {
    uint8_t bytes[DL12_HALF_FRAMES * DL12_FRAME_BYTES];
    int16_t samples[DL12_HALF_WORDS];
    uint32_t align;
  } input;
  int ret = 0;
  int half = 0;
  int fills = 0;
  int last_half = -1;
  uint64_t played_frames = 0;
  int64_t begin = monotonic_ms();
  int64_t next_progress = begin + AUDIO_PROGRESS_INTERVAL_MS;
  if (begin < 0)
    return -1;

  g_audio_stop = application_stop_requested;
  if (install_signal_handlers() < 0) {
    ERR("Cannot install playback signal handlers");
    return -1;
  }
  configure_dl12();
  ret = report_audio_progress(0, 0);

  while (!ret && !audio_stop_requested()) {
    check_stdin_for_stop();
    if (audio_stop_requested())
      break;
    int frames = read_frames(source, input.bytes, DL12_HALF_FRAMES);
    if (frames <= 0) {
      ret = frames < 0 ? -1 : 0;
      break;
    }
    if (fills >= 2) {
      int ready = wait_for_dma_half(1 - half);
      if (ready <= 0) {
        ret = ready < 0 ? -1 : 0;
        break;
      }
    }
    write_pcm_half(half, input.bytes, frames, channels, bits, volume_pct);
    played_frames += frames;
    last_half = half;
    half = 1 - half;
    if (fills < 2)
      ++fills;
    int64_t now = monotonic_ms();
    if (now < 0) {
      ret = -1;
      break;
    }
    if (now >= next_progress) {
      ret = report_audio_progress(played_frames, now - begin);
      if (ret < 0)
        break;
      next_progress = now + AUDIO_PROGRESS_INTERVAL_MS;
    }
  }

  /* Let the final half reach DMA and finish before silencing SRAM. */
  if (!ret && !audio_stop_requested() && last_half >= 0) {
    int ready = wait_for_dma_half(last_half);
    if (ready > 0)
      ready = wait_for_dma_half(1 - last_half);
    if (ready < 0)
      ret = -1;
  }
  sram_zero(afe_sram, DL12_SRAM_SIZE / sizeof(uint32_t));
  restore_signal_handlers();
  LOG("Playback %s: %llu frames", ret < 0 ? "failed" :
      audio_stop_requested() ? "stopped by user" : "complete",
      (unsigned long long)played_frames);
  return ret;
}

struct wav_source {
  FILE *fp;
  uint32_t remaining;
  int frame_bytes;
};

static int read_wav_frames(void *source, void *buffer, int max_frames) {
  struct wav_source *wav = source;
  uint32_t bytes = (uint32_t)(max_frames * wav->frame_bytes);
  if (bytes > wav->remaining)
    bytes = wav->remaining;
  if (fread(buffer, 1, bytes, wav->fp) != bytes) {
    ERR("WAV data is truncated or unreadable");
    return -1;
  }
  wav->remaining -= bytes;
  return bytes / wav->frame_bytes;
}

int audio_play_wav(const char *path, int volume_pct) {
  if (!path || !audio_is_ready())
    return -1;
  volume_pct = clamp_volume(volume_pct);
  LOG("Playing WAV: %s at %d%% volume", path, volume_pct);

  FILE *fp = fopen(path, "rb");
  if (!fp) {
    ERR("Cannot open WAV file: %s", path);
    return -1;
  }
  struct wav_format fmt;
  uint32_t data_size;
  int ret = parse_wav_header(fp, &fmt, &data_size);
  if (!ret) {
    LOG("WAV: %uHz %uch %ubit, data=%u bytes", fmt.sample_rate, fmt.channels,
        fmt.bits_per_sample, data_size);
    if (fmt.sample_rate != AUDIO_DL12_RATE)
      LOG("WARNING: WAV rate differs from 48000Hz; speed/pitch will be off");
    struct wav_source source = {fp, data_size, fmt.block_align};
    ret = stream_pcm(&source, read_wav_frames, fmt.channels,
                     fmt.bits_per_sample, volume_pct);
  }
  fclose(fp);
  return ret;
}

/* ═══════════════════════════════════════════════════════════════
 *  Hardware tone generator test
 *
 *  Enables the AFE's built-in sine wave generator (SGEN).
 *  This produces a tone ENTIRELY in hardware — no SRAM writes,
 *  no DMA involved. If you can hear this tone, the I2S → codec
 *  → headphone path is working and the problem is with SRAM/DMA.
 *  If you can NOT hear it, the codec (WM1801) is muted or the
 *  I2S physical connection is broken.
 * ═══════════════════════════════════════════════════════════════ */
int audio_play_tone(int duration_sec) {
  if (!audio_is_ready() || duration_sec < 0)
    return -1;
  g_audio_stop = application_stop_requested;
  if (install_signal_handlers() < 0) {
    ERR("Cannot install tone signal handlers");
    return -1;
  }
  LOG("=== HARDWARE TONE TEST ===");
  LOG("Enabling AFE sine generator for %d seconds...", duration_sec);

  /* Enable SGEN: write CON2 first (enable), then CON0 (config) */
  afe_rmw(AFE_SGEN_CON2, SGEN_CON2_EN_MASK, SGEN_CON2_EN);
  afe_write(AFE_SGEN_CON0, SGEN_CON0_EN_CUST);

  /* SGEN outputs are on I32/I33 (0x20/0x21). Route them to I2S_OUT (O2/O3).
   * Mux config: O2 = bits 16-23, O3 = bits 24-31. */
  uint32_t orig_mux = afe_read(AFE_CONN_MUX_CFG);
  afe_write(AFE_CONN_MUX_CFG,
            (orig_mux & ~0xFFFF0000) | (0x20 << 16) | (0x21 << 24));

  LOG("Sine generator active. Mux=0x%08x", afe_read(AFE_CONN_MUX_CFG));

  int elapsed = 0;
  while (elapsed < duration_sec && !audio_stop_requested()) {
    check_stdin_for_stop();
    sleep(1);
    elapsed++;
  }

  restore_signal_handlers();

  if (audio_stop_requested()) {
    LOG("Tone generator stopped by user.");
  } else {
    LOG("Tone test complete.");
  }

  /* Stop */
  afe_write(AFE_SGEN_CON2, 0x00000000);
  afe_write(AFE_CONN_MUX_CFG, orig_mux);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  MP3 playback
 * ═══════════════════════════════════════════════════════════════ */
static int read_mp3_frames(void *source, void *buffer, int max_frames) {
  int64_t count = psvr2_mp3_read_frames(source, buffer, (size_t)max_frames);
  if (count < 0 || count > max_frames) {
    ERR("MP3 decoding failed: %s", strerror(errno));
    return -1;
  }
  return (int)count;
}

int audio_play_mp3(const char *path, int volume_pct) {
  if (!path || !audio_is_ready())
    return -1;
  volume_pct = clamp_volume(volume_pct);
  LOG("Playing MP3: %s at %d%% volume", path, volume_pct);
  char error[160];
  Psvr2Mp3Decoder *dec = psvr2_mp3_open(path, error, sizeof(error));
  if (!dec) {
    ERR("Cannot open MP3 file: %s", error);
    return -1;
  }
  unsigned channels = psvr2_mp3_channels(dec);
  unsigned rate = psvr2_mp3_sample_rate(dec);
  if ((channels != 1 && channels != 2) || !rate) {
    ERR("MP3 has no supported mono/stereo audio stream");
    psvr2_mp3_close(dec);
    return -1;
  }
  LOG("MP3: %uHz %uch", rate, channels);
  if (rate != AUDIO_DL12_RATE)
    LOG("WARNING: MP3 rate differs from 48000Hz; speed/pitch will be off");
  int ret = stream_pcm(dec, read_mp3_frames, (int)channels, 16, volume_pct);
  psvr2_mp3_close(dec);
  return ret;
}

/* ═══════════════════════════════════════════════════════════════
 *  Format Dispatcher
 * ═══════════════════════════════════════════════════════════════ */
int audio_play_file(const char *path, int volume_pct) {
  if (!path)
    return -1;
  const char *ext = strrchr(path, '.');
  if (ext && strcasecmp(ext, ".mp3") == 0) {
    return audio_play_mp3(path, volume_pct);
  }
  return audio_play_wav(path, volume_pct);
}
