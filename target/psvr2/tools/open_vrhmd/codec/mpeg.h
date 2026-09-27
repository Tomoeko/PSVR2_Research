#ifndef PSVR2_CODEC_MPEG_H
#define PSVR2_CODEC_MPEG_H

#include <stddef.h>
#include <stdint.h>

typedef struct Psvr2MpegDecoder Psvr2MpegDecoder;

typedef struct Psvr2MpegPlane {
  unsigned width;
  unsigned height;
  size_t stride;
  const uint8_t *data;
} Psvr2MpegPlane;

typedef struct Psvr2MpegFrame {
  double time;
  unsigned width;
  unsigned height;
  Psvr2MpegPlane y;
  Psvr2MpegPlane cb;
  Psvr2MpegPlane cr;
} Psvr2MpegFrame;

/* Accepts MPEG1 video elementary streams and MPEG program streams. Frames
 * arrive in display order. Planes are planar YUV420; width/height describe
 * padded storage, stride is its byte row pitch, and frame dimensions specify
 * the visible crop. Pixel storage belongs to the decoder and remains valid
 * until the next read, rewind, seek or close. */
Psvr2MpegDecoder *psvr2_mpeg_open(const char *path, char *error,
                                  size_t capacity);
unsigned psvr2_mpeg_width(const Psvr2MpegDecoder *decoder);
unsigned psvr2_mpeg_height(const Psvr2MpegDecoder *decoder);
double psvr2_mpeg_frame_rate(const Psvr2MpegDecoder *decoder);
int psvr2_mpeg_read_frame(Psvr2MpegDecoder *decoder, Psvr2MpegFrame *frame);
int psvr2_mpeg_rewind(Psvr2MpegDecoder *decoder);
int psvr2_mpeg_seek(Psvr2MpegDecoder *decoder, double seconds,
                    Psvr2MpegFrame *frame);
const char *psvr2_mpeg_error(const Psvr2MpegDecoder *decoder);
void psvr2_mpeg_close(Psvr2MpegDecoder *decoder);

/* read_frame/seek return 1 for a frame, 0 at a clean end and -1 for an error.
 * The caller must distinguish malformed input from a clean end of playback. */

#endif
