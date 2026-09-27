#ifndef PSVR2_CODEC_MP3_H
#define PSVR2_CODEC_MP3_H
#include <stddef.h>
#include <stdint.h>

typedef struct Psvr2Mp3Decoder Psvr2Mp3Decoder;

/* MPEG-1 Layer III, 32/44.1/48 kHz. Open decodes and validates the first frame.
 * The decoder owns its file and bounded reservoir/overlap state. */
Psvr2Mp3Decoder *psvr2_mp3_open(const char *path, char *error, size_t error_cap);
unsigned psvr2_mp3_sample_rate(const Psvr2Mp3Decoder *decoder);
unsigned psvr2_mp3_channels(const Psvr2Mp3Decoder *decoder);
/* Interleaved signed 16-bit PCM, capacity/count in complete channel frames.
 * Returns 0 at EOF, -1 with errno for malformed/unsupported input or I/O.
 * An error is sticky; close and reopen to restart. No concealment/silent frames. */
int64_t psvr2_mp3_read_frames(Psvr2Mp3Decoder *decoder, int16_t *pcm,
                            size_t max_frames);
void psvr2_mp3_close(Psvr2Mp3Decoder *decoder);
#endif
