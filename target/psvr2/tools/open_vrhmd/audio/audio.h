/*
 * audio.h — PSVR2 Audio Playback Definitions (Userspace)
 *
 * Ioctl definitions matching sieaudio.ko's interface.
 * The stock retail sieaudio.ko only supports PREPARE, TRANSFER_START/STOP,
 * and CAPTURE_START/STOP. Playback ioctls (7-11) require a develop build.
 *
 * For retail firmware, we use the PREPARE + TRANSFER_START path and write
 * PCM data directly to the AFE DL12 DMA buffer via /dev/mem.
 */

#ifndef OPEN_VRHMD_AUDIO_H
#define OPEN_VRHMD_AUDIO_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════
 *  SIE Audio ioctl definitions (from sieaudio.h)
 * ═══════════════════════════════════════════════════════════════ */
#define SIE_AUDIO_IOC_TYPE 'A'

/* Standard ioctls (always available in retail sieaudio.ko) */
#define SIE_AUDIO_IO_TRANSFER_START  0x40084101  /* _IOW('A', 1, 8 bytes) */
#define SIE_AUDIO_IO_TRANSFER_STOP   0x00004102  /* _IO('A', 2)          */
#define SIE_AUDIO_IO_PREPARE         0x00004103  /* _IO('A', 3)          */
#define SIE_AUDIO_IO_CAPTURE_START   0x40044104  /* _IOW('A', 4, 4 bytes)*/
#define SIE_AUDIO_IO_CAPTURE_STOP    0x00004105  /* _IO('A', 5)          */

/* Develop-build ioctls (NOT present in stock retail sieaudio.ko) */
#define SIE_AUDIO_IO_PLAYBACK_START  0x00004107  /* _IO('A', 7)          */
#define SIE_AUDIO_IO_PLAYBACK_STOP   0x00004108  /* _IO('A', 8)          */
#define SIE_AUDIO_IO_PLAYBACK_WRITE  0x40084109  /* _IOW('A', 9, 8 bytes)*/
#define SIE_AUDIO_IO_START_TONEGEN   0x0000410A  /* _IO('A', 10)         */
#define SIE_AUDIO_IO_STOP_TONEGEN    0x0000410B  /* _IO('A', 11)         */

/* Transfer start request (matches kernel struct) */
struct audio_io_transfer_start_req {
    uint32_t port_type;  /* 1 = SIE_AUDIO_INPUT_PORT_DP */
    uint32_t rate;       /* 48000 */
};

/* Playback write request (develop build only) */
struct audio_io_playback_write_req {
    void    *buf;
    uint32_t size;
};

/* DL12 DAI runtime defaults (from mt-afe-pcm.c) */
#define AUDIO_DL12_RATE       48000
#define AUDIO_DL12_CHANNELS   2
#define AUDIO_DL12_BPS        32   /* S32_LE */
#define AUDIO_DL12_PERIOD     128  /* samples per period */

/* Period size in bytes: period_samples * channels * bytes_per_sample */
#define AUDIO_PERIOD_BYTES    (AUDIO_DL12_PERIOD * AUDIO_DL12_CHANNELS * 4)

/* API */
int  audio_init(int volume_pct);
int  audio_play_wav(const char *path, int volume_pct);
int  audio_play_mp3(const char *path, int volume_pct);
int  audio_play_file(const char *path, int volume_pct);
int  audio_play_tone(int duration_sec);
int  audio_stop_hardware(void);
void audio_cleanup(void);

#endif /* OPEN_VRHMD_AUDIO_H */
