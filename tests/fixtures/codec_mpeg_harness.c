#include "../../target/psvr2/tools/open_vrhmd/codec/mpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  char error[160];
  Psvr2MpegDecoder *d = psvr2_mpeg_open(argv[1], error, sizeof(error));
  if (!d) {
    fprintf(stderr, "%s\n", error);
    return 1;
  }
  FILE *out = fopen(argv[2], "wb");
  if (!out)
    return 2;
  Psvr2MpegFrame f;
  int r;
  unsigned frames = 0;
  while ((r = psvr2_mpeg_read_frame(d, &f)) == 1) {
    if (f.y.stride < f.width || f.cb.stride < (f.width + 1) / 2 ||
        f.cr.stride != (size_t)f.cb.stride)
      return 3;
    for (unsigned p = 0; p < 3; p++) {
      const Psvr2MpegPlane *plane = p == 0 ? &f.y : p == 1 ? &f.cb : &f.cr;
      unsigned w = p ? (f.width + 1) / 2 : f.width,
               h = p ? (f.height + 1) / 2 : f.height;
      for (unsigned y = 0; y < h; y++)
        if (fwrite(plane->data + y * plane->stride, 1, w, out) != w)
          return 2;
    }
    if (f.time != (double)frames / psvr2_mpeg_frame_rate(d))
      return 3;
    frames++;
  }
  fclose(out);
  if (r < 0) {
    fprintf(stderr, "%s after %u frames\n", psvr2_mpeg_error(d), frames);
    psvr2_mpeg_close(d);
    return 1;
  }
  printf("%u %u %u %.9f\n", frames, psvr2_mpeg_width(d), psvr2_mpeg_height(d),
         psvr2_mpeg_frame_rate(d));
  if (argc > 3) {
    double t = strtod(argv[3], NULL);
    r = psvr2_mpeg_seek(d, t, &f);
    if (r < 0 || (r == 1 && f.time < t))
      return 3;
    if (psvr2_mpeg_rewind(d) < 0 || psvr2_mpeg_read_frame(d, &f) != 1 ||
        f.time != 0)
      return 3;
  }
  psvr2_mpeg_close(d);
  return 0;
}
