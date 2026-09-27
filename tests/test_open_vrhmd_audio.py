"""Host regression checks for PCM parsing, buffering, and fan hysteresis.

The actual source is compiled with minimal Linux header shims. Tests use normal
memory in place of MMIO and never initialize or access headset hardware.
"""
from __future__ import annotations

from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "target/psvr2/tools/open_vrhmd"
PCM_FORMAT = struct.pack("<HHIIHH", 1, 2, 48000, 192000, 4, 16)


def make_wav(chunks: list[tuple[bytes, bytes]]) -> bytes:
    payload = b"WAVE" + b"".join(
        tag + struct.pack("<I", len(data)) + data + (b"\0" if len(data) % 2 else b"")
        for tag, data in chunks
    )
    return b"RIFF" + struct.pack("<I", len(payload)) + payload


class AudioTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("host C compiler unavailable")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        (cls.directory / "linux").mkdir()
        (cls.directory / "sys").mkdir()
        (cls.directory / "linux/i2c.h").write_text("""#pragma once
#include <stdint.h>
#define I2C_M_RD 1
struct i2c_msg { uint16_t addr, flags, len; uint8_t *buf; };
""")
        (cls.directory / "linux/i2c-dev.h").write_text("""#pragma once
#include "i2c.h"
#define I2C_SLAVE 0x0703
#define I2C_RDWR 0x0707
struct i2c_rdwr_ioctl_data { struct i2c_msg *msgs; uint32_t nmsgs; };
""")
        (cls.directory / "sys/prctl.h").write_text("""#pragma once
#define PR_SET_NAME 15
static inline int prctl(int option, ...) { (void)option; return 0; }
""")
        harness = cls.directory / "audio_checks.c"
        harness.write_text(r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
static int test_open(const char *path, int flags, ...);
static int test_close(int fd);
static int test_ioctl(int fd, unsigned long request, ...);
static ssize_t test_write(int fd, const void *data, size_t size);
static int test_usleep(useconds_t duration);
static int test_clock_gettime(clockid_t clock, struct timespec *time);
#define clock_gettime test_clock_gettime
#define open test_open
#define close test_close
#define ioctl test_ioctl
#define write test_write
#define usleep test_usleep
''' + '#include "' + str(SOURCE / "audio/audio.c") + '"\n'
                           '#include "' + str(SOURCE / "core/fan_ctrl.c") + '"\n' + r'''
#undef clock_gettime
#undef open
#undef close
#undef ioctl
#undef write
#undef usleep
struct Psvr2Mp3Decoder { int unused; };
Psvr2Mp3Decoder *psvr2_mp3_open(const char *path, char *error, size_t capacity) {
  (void)path; if (capacity) snprintf(error,capacity,"test decoder unavailable");
  errno = EINVAL; return NULL;
}
unsigned psvr2_mp3_sample_rate(const Psvr2Mp3Decoder *d) { (void)d;return 48000; }
unsigned psvr2_mp3_channels(const Psvr2Mp3Decoder *d) { (void)d;return 2; }
int64_t psvr2_mp3_read_frames(Psvr2Mp3Decoder *d,int16_t *pcm,size_t frames) {
  (void)d;(void)pcm;(void)frames;errno=EINVAL;return -1;
}
void psvr2_mp3_close(Psvr2Mp3Decoder *d) { (void)d; }
#define MOCK_I2C_FD 1300
static int mock_codec;
static int mock_dma, mock_stalled_dma, mock_mute_after_start;
static uint64_t mock_clock_us;
static uint16_t codec_registers[256];
static struct { uint8_t reg; uint16_t value; } codec_writes[256];
static unsigned codec_write_count;
static int test_open(const char *path, int flags, ...) {
  if (mock_codec && !strcmp(path, "/dev/i2c-0")) return MOCK_I2C_FD;
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  return open(path, flags, mode);
}
static int test_close(int fd) {
  return fd == MOCK_I2C_FD ? 0 : close(fd);
}
static int test_ioctl(int fd, unsigned long request, ...) {
  assert(mock_codec && fd == MOCK_I2C_FD);
  if (request == I2C_SLAVE) return 0;
  assert(request == I2C_RDWR);
  va_list ap;
  va_start(ap, request);
  struct i2c_rdwr_ioctl_data *rdwr = va_arg(ap, void *);
  va_end(ap);
  assert(rdwr->nmsgs == 2);
  assert(rdwr->msgs[0].addr == WM1801_I2C_ADDR);
  assert(rdwr->msgs[0].len == 1 && rdwr->msgs[0].flags == 0);
  assert(rdwr->msgs[1].addr == WM1801_I2C_ADDR);
  assert(rdwr->msgs[1].len == 2 && rdwr->msgs[1].flags == I2C_M_RD);
  uint16_t value = codec_registers[rdwr->msgs[0].buf[0]];
  rdwr->msgs[1].buf[0] = value >> 8;
  rdwr->msgs[1].buf[1] = value & 255;
  return 2;
}
static ssize_t test_write(int fd, const void *data, size_t size) {
  if (fd != MOCK_I2C_FD) return write(fd, data, size);
  assert(mock_codec && size == 3 && codec_write_count < 256);
  const uint8_t *bytes = data;
  uint16_t value = (uint16_t)((bytes[1] << 8) | bytes[2]);
  codec_registers[bytes[0]] = value;
  codec_writes[codec_write_count].reg = bytes[0];
  codec_writes[codec_write_count++].value = value;
  return 3;
}
static int test_usleep(useconds_t duration) {
  if (mock_dma) {
    mock_clock_us += duration;
    if (!mock_stalled_dma) {
      uint64_t frame = (mock_clock_us * AUDIO_DL12_RATE / UINT64_C(1000000))
                       % DL12_TOTAL_FRAMES;
      afe_regs[AFE_DL12_CUR_R / 4] = (uint32_t)AFE_SRAM_BASE +
                                    (uint32_t)frame * DL12_FRAME_BYTES;
    }
    if (mock_mute_after_start && mock_clock_us >= UINT64_C(5000000))
      codec_registers[WM1801_DAC_CONTROL] |= 0x8;
    return 0;
  }
  return mock_codec ? 0 : usleep(duration);
}
static int test_clock_gettime(clockid_t clock, struct timespec *time) {
  if (!mock_dma) return clock_gettime(clock, time);
  assert(clock == CLOCK_MONOTONIC);
  time->tv_sec = (time_t)(mock_clock_us / UINT64_C(1000000));
  time->tv_nsec = (long)((mock_clock_us % UINT64_C(1000000)) * 1000);
  return 0;
}
static unsigned find_codec_write(uint8_t reg, uint16_t value) {
  for (unsigned i = 0; i < codec_write_count; ++i)
    if (codec_writes[i].reg == reg && codec_writes[i].value == value) return i;
  return codec_write_count;
}
enum { SYNTHETIC_FRAMES = 12 * AUDIO_DL12_RATE };
typedef struct SyntheticPcm {
  uint64_t frame;
} SyntheticPcm;
static int read_synthetic_frames(void *context, void *buffer, int max_frames) {
  SyntheticPcm *source = context;
  uint64_t remaining = SYNTHETIC_FRAMES - source->frame;
  int frames = remaining < (uint64_t)max_frames ? (int)remaining : max_frames;
  int16_t *samples = buffer;
  for (int frame = 0; frame < frames; ++frame) {
    samples[frame * 2] = 8192;
    samples[frame * 2 + 1] = -16384;
  }
  source->frame += (uint64_t)frames;
  return frames;
}
void close_devices(void) {}
volatile sig_atomic_t application_stop_requested;
static void previous_signal(int signal) { (void)signal; }
int main(int argc, char **argv) {
  assert(argc >= 2);
  if (!strcmp(argv[1], "codec")) {
    mock_codec = 1;
    codec_registers[1] = 0x049f;
    assert(wm1801_init(50) == 0);
    assert(codec_registers[10] == 0x00c0);
    assert(codec_registers[11] == 0x01c0);
    assert(codec_registers[57] == 0 && codec_registers[58] == 0);
    assert(codec_registers[2] == 0x00e7 && codec_registers[3] == 0x01e7);
    assert(codec_registers[5] == 0);
    unsigned left = find_codec_write(10, 0x00c0);
    unsigned right = find_codec_write(11, 0x01c0);
    unsigned headphone_power = find_codec_write(90, 0x0080);
    assert(left < right && right < headphone_power);
    assert(find_codec_write(2, 0x00e7) < headphone_power);
    assert(find_codec_write(3, 0x01e7) < headphone_power);
    assert(find_codec_write(0, 0x01b2) == codec_write_count);
    assert(find_codec_write(57, 0x00c4) == codec_write_count);
    assert(find_codec_write(58, 0x01c4) == codec_write_count);
    assert(wm1801_init(100) == 0);
    assert(codec_registers[2] == 0x00f9 && codec_registers[3] == 0x01f9);
    unsigned before = codec_write_count;
    assert(wm1801_init(-1) < 0 && errno == ERANGE);
    assert(wm1801_init(101) < 0 && errno == ERANGE);
    assert(codec_write_count == before);
    return 0;
  }
  if (!strcmp(argv[1], "stream") || !strcmp(argv[1], "stream-muted") ||
      !strcmp(argv[1], "stream-stalled")) {
    assert(argc == 2);
    uint32_t registers[AFE_REG_SIZE / sizeof(uint32_t)] = {0};
    uint32_t memory[DL12_SRAM_SIZE / sizeof(uint32_t) + 2];
    for (unsigned i = 0; i < sizeof(memory) / sizeof(memory[0]); ++i)
      memory[i] = 0x55555555;
    afe_regs = registers;
    afe_sram = memory + 1;
    registers[AFE_DL12_CUR_R / 4] = (uint32_t)AFE_SRAM_BASE;
    mock_codec = mock_dma = 1;
    mock_stalled_dma = !strcmp(argv[1], "stream-stalled");
    mock_mute_after_start = !strcmp(argv[1], "stream-muted");
    fd_i2c = MOCK_I2C_FD;
    codec_registers[WM1801_POWER_2] = 0x01e0;
    codec_registers[WM1801_HEADPHONE_CONTROL] = 0x00ff;
    codec_registers[WM1801_DAC_CONTROL] = 0;
    codec_registers[WM1801_DAC_VOLUME_LEFT] = 0x00c0;
    codec_registers[WM1801_DAC_VOLUME_RIGHT] = 0x01c0;
    SyntheticPcm source = {0};
    int ret = stream_pcm(&source, read_synthetic_frames, 2, 16, 20);
    if (mock_stalled_dma) {
      assert(ret == -1 && errno == ETIMEDOUT);
      assert(mock_clock_us >= UINT64_C(1000000) &&
             mock_clock_us < UINT64_C(2000000));
    } else if (mock_mute_after_start) {
      assert(ret == -1 && errno == EIO);
      assert(mock_clock_us >= UINT64_C(5000000) &&
             mock_clock_us < UINT64_C(6000000));
    } else {
      assert(ret == 0 && source.frame == SYNTHETIC_FRAMES);
      assert(mock_clock_us >= UINT64_C(11900000) &&
             mock_clock_us < UINT64_C(12100000));
    }
    assert(memory[0] == 0x55555555 &&
           memory[DL12_SRAM_SIZE / sizeof(uint32_t) + 1] == 0x55555555);
    for (unsigned i = 0; i < DL12_SRAM_SIZE / sizeof(uint32_t); ++i)
      assert(afe_sram[i] == 0);
    return 0;
  }
  if (!strcmp(argv[1], "wav")) {
    assert(argc == 3);
    FILE *fp = fopen(argv[2], "rb");
    assert(fp);
    struct wav_format fmt;
    uint32_t data_size;
    int ret = parse_wav_header(fp, &fmt, &data_size);
    if (!ret)
      printf("%u %u %u %u %u\n", fmt.channels, fmt.bits_per_sample,
             fmt.sample_rate, fmt.block_align, data_size);
    fclose(fp);
    return ret < 0;
  }
  if (!strcmp(argv[1], "fan")) {
    assert(argc == 4);
    printf("%d\n", fan_lookup_duty(atoi(argv[2]), atoi(argv[3])));
    return 0;
  }
  if (!strcmp(argv[1], "signals")) {
    struct sigaction previous = {0}, actual;
    previous.sa_handler = previous_signal;
    sigemptyset(&previous.sa_mask);
    assert(sigaction(SIGINT, &previous, NULL) == 0);
    assert(sigaction(SIGTERM, &previous, NULL) == 0);
    assert(install_signal_handlers() == 0);
    restore_signal_handlers();
    assert(sigaction(SIGINT, NULL, &actual) == 0);
    assert(actual.sa_handler == previous_signal);
    assert(sigaction(SIGTERM, NULL, &actual) == 0);
    assert(actual.sa_handler == previous_signal);
    audio_sigint(SIGTERM);
    assert(g_audio_stop && application_stop_requested);
    return 0;
  }
  if (!strcmp(argv[1], "dma")) {
    uint32_t registers[AFE_REG_SIZE / sizeof(uint32_t)] = {0};
    afe_regs = registers;
    registers[AFE_DL12_CUR_R / 4] = (uint32_t)AFE_SRAM_BASE;
    assert(wait_for_dma_half(0) == 1);
    g_audio_stop = 1;
    assert(wait_for_dma_half(1) == 0);
    g_audio_stop = 0;
    application_stop_requested = 1;
    assert(wait_for_dma_half(1) == 0);
    application_stop_requested = 0;
    assert(wait_for_dma_half(1) == -1);
    assert(errno == ETIMEDOUT);
    return 0;
  }
  assert(!strcmp(argv[1], "pcm"));
  uint8_t pcm16[] = {0x00,0x80,0xff,0x7f,0xff,0xff,0x00,0x00};
  uint8_t pcm32[] = {0x00,0x00,0x00,0x80,0xff,0xff,0xff,0x7f};
  assert(pcm_sample(pcm16,16) == INT32_MIN);
  assert(pcm_sample(pcm16+2,16) == INT32_C(2147418112));
  assert(pcm_sample(pcm16+4,16) == -65536);
  assert(pcm_sample(pcm32,32) == INT32_MIN);
  assert(pcm_sample(pcm32+4,32) == INT32_MAX);
  assert(clamp_volume(-1) == 0 && clamp_volume(50) == 50 && clamp_volume(101) == 100);
  uint32_t memory[DL12_HALF_WORDS * 2 + 2];
  memset(memory,0x55,sizeof(memory));
  afe_sram = memory + 1;
  write_pcm_half(1,pcm16,2,2,16,50);
  assert(memory[0] == 0x55555555 && memory[DL12_HALF_WORDS * 2 + 1] == 0x55555555);
  assert(memory[1] == 0x55555555 && memory[DL12_HALF_WORDS] == 0x55555555);
  assert(memory[DL12_HALF_WORDS + 1] == (uint32_t)-1073741824);
  assert(memory[DL12_HALF_WORDS + 2] == 1073709056);
  assert(memory[DL12_HALF_WORDS + 3] == (uint32_t)-32768);
  assert(memory[DL12_HALF_WORDS + 4] == 0 && memory[DL12_HALF_WORDS * 2] == 0);
  write_pcm_half(0,pcm16,1,1,16,100);
  assert(memory[1] == (uint32_t)INT32_MIN && memory[2] == (uint32_t)INT32_MIN);
  assert(memory[3] == 0 && memory[DL12_HALF_WORDS] == 0);
  write_pcm_half(0,pcm32,1,2,32,100);
  assert(memory[1] == (uint32_t)INT32_MIN && memory[2] == INT32_MAX);
  return 0;
}
''')
        cls.binary = cls.directory / "audio_checks"
        subprocess.run(
            [compiler, "-std=gnu11", "-DPSVR2_SOURCE_FAMILY_0600=1",
             "-I" + str(cls.directory),
             "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
             str(harness), "-lm", "-o", str(cls.binary)],
            check=True, capture_output=True, text=True,
        )
        cls.codec_binary = cls.directory / "codec-path-checks"
        subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Wconversion", "-Werror",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             str(Path(__file__).resolve().parents[1] /
                 "tests/fixtures/open_vrhmd_codec_harness.c"),
             "-o", str(cls.codec_binary)],
            check=True, capture_output=True, text=True,
        )

    def run_check(self, *arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run([str(self.binary), *arguments], input="",
                              capture_output=True, text=True, timeout=5)

    def parse_wav(self, payload: bytes) -> subprocess.CompletedProcess:
        path = self.directory / "sample.wav"
        path.write_bytes(payload)
        return self.run_check("wav", str(path))

    def test_wav_accepts_extra_chunks_and_odd_padding(self) -> None:
        for chunks in [[(b"fmt ", PCM_FORMAT), (b"data", b"\0" * 8)],
                       [(b"JUNK", b"123"), (b"fmt ", PCM_FORMAT + b"123"),
                        (b"LIST", b"abc"), (b"data", b"\0" * 8)]]:
            with self.subTest(chunks=chunks):
                result = self.parse_wav(make_wav(chunks))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), "2 16 48000 4 8")

    def test_wav_rejects_invalid_format_and_partial_frames(self) -> None:
        for fmt in [PCM_FORMAT[:14], PCM_FORMAT[:12] + b"\0\0" + PCM_FORMAT[14:],
                    PCM_FORMAT[:2] + b"\0\0" + PCM_FORMAT[4:],
                    PCM_FORMAT[:14] + b"\x08\0"]:
            with self.subTest(format=fmt):
                result = self.parse_wav(make_wav([(b"fmt ", fmt), (b"data", b"\0" * 8)]))
                self.assertEqual(result.returncode, 1)
        result = self.parse_wav(make_wav([(b"fmt ", PCM_FORMAT), (b"data", b"\0" * 7)]))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.parse_wav(make_wav([(b"data", b"\0" * 8)])).returncode, 1)

    def test_wav_rejects_truncated_and_overflowing_chunks(self) -> None:
        valid = make_wav([(b"fmt ", PCM_FORMAT), (b"data", b"\0" * 8)])
        oversized = bytearray(valid)
        oversized[16:20] = struct.pack("<I", 0xFFFFFFFF)
        for payload in [valid[:-1], bytes(oversized), b"NOPE" + valid[4:]]:
            with self.subTest(payload=payload):
                self.assertEqual(self.parse_wav(payload).returncode, 1)

    def test_codec_uses_bounded_gain_and_playback_only_route(self) -> None:
        result = self.run_check("codec")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_codec_route_failure_and_headphone_gain_bounds(self) -> None:
        result = subprocess.run([str(self.codec_binary)], capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_synthetic_pcm_stream_completes_and_reports_progress(self) -> None:
        result = self.run_check("stream")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Playback complete: 576000 frames", result.stderr)
        self.assertIn("Audio progress: 5.0s", result.stderr)
        self.assertIn("Audio progress: 10.0s", result.stderr)

    def test_pcm_stream_fails_if_codec_mutes_while_dma_keeps_running(self) -> None:
        result = self.run_check("stream-muted")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Audio output was disabled or muted", result.stderr)
        self.assertIn("Playback failed:", result.stderr)

    def test_pcm_stream_fails_and_silences_sram_when_dma_stalls(self) -> None:
        result = self.run_check("stream-stalled")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DL12 DMA cursor stalled", result.stderr)
        self.assertIn("Playback failed:", result.stderr)

    def test_pcm_signed_boundaries_volume_and_half_buffer_bounds(self) -> None:
        result = self.run_check("pcm")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_playback_restores_existing_signal_handlers(self) -> None:
        result = self.run_check("signals")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_dma_wait_stops_or_times_out(self) -> None:
        result = self.run_check("dma")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_fan_hysteresis_and_emergency_recovery(self) -> None:
        for duty, temperature, expected in [(30, 42, 38), (38, 32, 30),
                                            (38, 44, 46), (46, 59, 46),
                                            (46, 60, 80), (80, 38, 46),
                                            (100, 50, 80)]:
            with self.subTest(duty=duty, temperature=temperature):
                result = self.run_check("fan", str(duty), str(temperature))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(int(result.stdout), expected)


if __name__ == "__main__":
    unittest.main()
