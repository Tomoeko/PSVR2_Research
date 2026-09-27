#include "mpeg.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MPEG1 syntax and reconstruction follow ISO/IEC 11172-2. Common VLC
 * assignments are documented in ITU-T H.262 Annex B. This decoder has no
 * hardware dependencies; compressed input and reconstructed references are
 * bounded independently, and malformed syntax is never concealed. */
typedef struct {
  unsigned code, length;
  int value;
} Vlc;
#include "mpeg_tables.h"
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define INTRA 1
#define PATTERN 2
#define BACKWARD 4
#define FORWARD 8
#define QUANT 16
static const Vlc intra_type[] = {{1, 1, INTRA}, {1, 2, INTRA | QUANT}};
static const Vlc predicted_type[] = {{1, 1, FORWARD | PATTERN},
                                     {1, 2, PATTERN},
                                     {1, 3, FORWARD},
                                     {3, 5, INTRA},
                                     {2, 5, QUANT | FORWARD | PATTERN},
                                     {1, 5, QUANT | PATTERN},
                                     {1, 6, QUANT | INTRA}};
static const Vlc bidirectional_type[] = {
    {2, 2, FORWARD | BACKWARD},
    {3, 2, FORWARD | BACKWARD | PATTERN},
    {2, 3, BACKWARD},
    {3, 3, BACKWARD | PATTERN},
    {2, 4, FORWARD},
    {3, 4, FORWARD | PATTERN},
    {3, 5, INTRA},
    {2, 5, QUANT | FORWARD | BACKWARD | PATTERN},
    {3, 6, QUANT | FORWARD | PATTERN},
    {2, 6, QUANT | BACKWARD | PATTERN},
    {1, 6, QUANT | INTRA}};
static const Vlc motion[] = {
    {1, 1, 0},    {1, 2, 1},    {1, 3, 2},    {1, 4, 3},    {3, 6, 4},
    {5, 7, 5},    {4, 7, 6},    {3, 7, 7},    {11, 9, 8},   {10, 9, 9},
    {9, 9, 10},   {17, 10, 11}, {16, 10, 12}, {15, 10, 13}, {14, 10, 14},
    {13, 10, 15}, {12, 10, 16}};
static const Vlc dc_y[] = {{4, 3, 0},  {0, 2, 1},  {1, 2, 2},
                           {5, 3, 3},  {6, 3, 4},  {14, 4, 5},
                           {30, 5, 6}, {62, 6, 7}, {126, 7, 8}};
static const Vlc dc_c[] = {{0, 2, 0},  {1, 2, 1},   {2, 2, 2},
                           {6, 3, 3},  {14, 4, 4},  {30, 5, 5},
                           {62, 6, 6}, {126, 7, 7}, {254, 8, 8}};
static const uint8_t scan[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
static const uint8_t default_intra[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};
static const double rates[9] = {0,  24000.0 / 1001, 24, 25, 30000.0 / 1001, 30,
                                50, 60000.0 / 1001, 60};

struct Psvr2MpegDecoder {
  FILE *file;
  int program, video_id, packet_left, source_end, failed;
  uint64_t bits;
  unsigned bit_count;
  int pending_code;
  unsigned width, height, pw, ph, mw, mh;
  double rate, basis[8][8];
  uint8_t intra[64], inter[64];
  struct {
    int16_t value;
    uint8_t length;
  } coefficient_prefix[1024];
  uint8_t *frames[3], *covered;
  size_t plane_bytes, frame_bytes;
  int past, future, work, finished, final_emitted;
  unsigned picture_type, quant, full[2], fcode[2];
  int mv[2][2], dc[3], last_mode;
  uint64_t display_index;
  char error[160];
};
static int fail(Psvr2MpegDecoder *d, const char *message) {
  if (!d->failed)
    snprintf(d->error, sizeof(d->error), "%s", message);
  d->failed = 1;
  errno = EINVAL;
  return -1;
}
static int raw(Psvr2MpegDecoder *d) {
  int c = fgetc(d->file);
  if (c == EOF && ferror(d->file))
    fail(d, "MPEG input read failed");
  return c;
}
static int skip_raw(Psvr2MpegDecoder *d, unsigned count) {
  while (count--)
    if (raw(d) == EOF)
      return fail(d, "Truncated program stream packet");
  return 0;
}
static int packet_byte(Psvr2MpegDecoder *d) {
  if (!d->program)
    return raw(d);
  while (!d->packet_left && !d->source_end && !d->failed) {
    unsigned prefix = 0xffffff;
    int c;
    do {
      c = raw(d);
      if (c == EOF) {
        d->source_end = 1;
        return EOF;
      }
      prefix = ((prefix << 8) | (unsigned)c) & 0xffffff;
    } while (prefix != 1);
    int id = raw(d);
    if (id == EOF)
      return fail(d, "Truncated program stream start code");
    if (id == 0xb9) {
      d->source_end = 1;
      return EOF;
    }
    if (id == 0xba) {
      c = raw(d);
      if (c == EOF)
        return fail(d, "Truncated pack header");
      if ((c & 0xf0) == 0x20) {
        if (skip_raw(d, 7) < 0)
          return EOF;
      } else if ((c & 0xc0) == 0x40) {
        if (skip_raw(d, 8) < 0)
          return EOF;
        c = raw(d);
        if (c == EOF || skip_raw(d, (unsigned)c & 7) < 0)
          return EOF;
      } else {
        fail(d, "Unsupported program stream pack header");
        return EOF;
      }
      continue;
    }
    int hi = raw(d), lo = raw(d);
    if (hi == EOF || lo == EOF) {
      fail(d, "Truncated packet length");
      return EOF;
    }
    int length = (hi << 8) | lo;
    if (id < 0xe0 || id > 0xef || (d->video_id >= 0 && id != d->video_id)) {
      if (skip_raw(d, (unsigned)length) < 0)
        return EOF;
      continue;
    }
    if (!length) {
      fail(d, "Unbounded video PES packet is unsupported");
      return EOF;
    }
    d->video_id = id;
    c = raw(d);
    length--;
    while (c == 0xff && length > 0) {
      c = raw(d);
      length--;
    }
    if (c == EOF) {
      fail(d, "Truncated video PES header");
      return EOF;
    }
    if ((c & 0xc0) == 0x80) {
      int flags = raw(d), n = raw(d);
      (void)flags;
      length -= 2;
      if (flags == EOF || n == EOF || n > length ||
          skip_raw(d, (unsigned)n) < 0) {
        fail(d, "Invalid MPEG2 PES header");
        return EOF;
      }
      length -= n;
    } else {
      if ((c & 0xc0) == 0x40) {
        if (length < 2 || raw(d) == EOF || (c = raw(d)) == EOF) {
          fail(d, "Truncated PES STD field");
          return EOF;
        }
        length -= 2;
      }
      unsigned n = (c & 0xf0) == 0x20   ? 4
                   : (c & 0xf0) == 0x30 ? 9
                   : c == 0x0f          ? 0
                                        : 99;
      if (n > (unsigned)length || skip_raw(d, n) < 0) {
        fail(d, "Invalid MPEG1 PES timestamp header");
        return EOF;
      }
      length -= (int)n;
    }
    if (length < 0) {
      fail(d, "Invalid video PES length");
      return EOF;
    }
    d->packet_left = length;
  }
  if (!d->packet_left)
    return EOF;
  int c = raw(d);
  if (c == EOF) {
    fail(d, "Truncated video PES payload");
    return EOF;
  }
  d->packet_left--;
  return c;
}
static int ensure(Psvr2MpegDecoder *d, unsigned n) {
  while (d->bit_count < n) {
    int c = packet_byte(d);
    if (c == EOF || d->failed)
      return 0;
    d->bits = (d->bits << 8) | (unsigned)c;
    d->bit_count += 8;
  }
  return 1;
}
static unsigned peek(Psvr2MpegDecoder *d, unsigned n) {
  if (!ensure(d, n))
    return 0;
  return (unsigned)((d->bits >> (d->bit_count - n)) & ((UINT64_C(1) << n) - 1));
}
static unsigned get(Psvr2MpegDecoder *d, unsigned n) {
  unsigned v = peek(d, n);
  if (d->bit_count < n) {
    fail(d, "Truncated MPEG syntax");
    return 0;
  }
  d->bit_count -= n;
  return v;
}
static int code(Psvr2MpegDecoder *d) {
  if (d->pending_code >= 0) {
    int c = d->pending_code;
    d->pending_code = -1;
    return c;
  }
  d->bit_count -= d->bit_count % 8;
  unsigned p = 0xffffff;
  while (ensure(d, 8)) {
    p = ((p << 8) | get(d, 8)) & 0xffffff;
    if (p == 1) {
      if (!ensure(d, 8)) {
        fail(d, "Truncated elementary start code");
        return -1;
      }
      return (int)get(d, 8);
    }
  }
  return -1;
}
static int vlc(Psvr2MpegDecoder *d, const Vlc *table, size_t count) {
  unsigned v = 0;
  for (unsigned n = 1; n <= 16; n++) {
    v = (v << 1) | get(d, 1);
    if (d->failed)
      return 0;
    for (size_t i = 0; i < count; i++)
      if (table[i].length == n && table[i].code == v)
        return table[i].value;
  }
  fail(d, "Invalid MPEG variable length code");
  return 0;
}
static int coefficient_symbol(Psvr2MpegDecoder *d) {
  /* The common short coefficient codes are decoded with a bounded prefix
   * lookup. Rare longer codes and final short tails retain the checked path. */
  if (ensure(d, 10)) {
    unsigned prefix = peek(d, 10);
    unsigned length = d->coefficient_prefix[prefix].length;
    if (length) {
      get(d, length);
      return d->coefficient_prefix[prefix].value;
    }
  }
  return vlc(d, coefficients, COUNT(coefficients));
}
static int sequence(Psvr2MpegDecoder *d) {
  unsigned w = get(d, 12), h = get(d, 12);
  get(d, 4);
  unsigned r = get(d, 4);
  get(d, 18);
  if (get(d, 1) != 1)
    return fail(d, "Missing MPEG sequence marker");
  get(d, 10);
  get(d, 1);
  if (!w || !h || w > 2048 || h > 2048 || r < 1 || r > 8)
    return fail(d, "Unsupported MPEG dimensions or frame rate");
  if (d->frames[0] && (w != d->width || h != d->height))
    return fail(d, "Changing MPEG dimensions requires a new decoder");
  d->width = w;
  d->height = h;
  d->rate = rates[r];
  d->mw = (w + 15) / 16;
  d->mh = (h + 15) / 16;
  d->pw = d->mw * 16;
  d->ph = d->mh * 16;
  memcpy(d->intra, default_intra, 64);
  memset(d->inter, 16, 64);
  if (get(d, 1))
    for (unsigned i = 0; i < 64; i++) {
      unsigned v = get(d, 8);
      if (!v)
        return fail(d, "Zero intra quantization matrix entry");
      d->intra[scan[i]] = (uint8_t)v;
    }
  if (get(d, 1))
    for (unsigned i = 0; i < 64; i++) {
      unsigned v = get(d, 8);
      if (!v)
        return fail(d, "Zero non-intra quantization matrix entry");
      d->inter[scan[i]] = (uint8_t)v;
    }
  if (d->failed)
    return -1;
  if (!d->frames[0]) {
    d->plane_bytes = (size_t)d->pw * d->ph;
    d->frame_bytes = d->plane_bytes * 3 / 2;
    for (unsigned i = 0; i < 3; i++) {
      d->frames[i] = calloc(1, d->frame_bytes);
      if (!d->frames[i])
        return fail(d, "MPEG frame allocation failed");
    }
    d->covered = calloc((size_t)d->mw * d->mh, 1);
    if (!d->covered)
      return fail(d, "MPEG coverage allocation failed");
  }
  return 0;
}
static int clamp(int v, int max) { return v < 0 ? 0 : v > max ? max : v; }
static uint8_t sample(const uint8_t *p, unsigned width, unsigned height, int x,
                      int y) {
  return p[(size_t)clamp(y, (int)height - 1) * width +
           (unsigned)clamp(x, (int)width - 1)];
}
static void predict_plane(uint8_t *dst, const uint8_t *src, unsigned width,
                          unsigned height, unsigned x, unsigned y,
                          unsigned size, int vx, int vy, int average) {
  /* Floor division, including negative odd half-pixel motion. */
  int ix = vx >= 0 ? vx / 2 : -((-vx + 1) / 2),
      iy = vy >= 0 ? vy / 2 : -((-vy + 1) / 2);
  unsigned fx = (unsigned)(vx - 2 * ix), fy = (unsigned)(vy - 2 * iy);
  for (unsigned j = 0; j < size; j++)
    for (unsigned i = 0; i < size; i++) {
      int sx = (int)(x + i) + ix, sy = (int)(y + j) + iy;
      unsigned a = sample(src, width, height, sx, sy), value = a;
      if (fx && fy)
        value = (a + sample(src, width, height, sx + 1, sy) +
                 sample(src, width, height, sx, sy + 1) +
                 sample(src, width, height, sx + 1, sy + 1) + 2) /
                4;
      else if (fx)
        value = (a + sample(src, width, height, sx + 1, sy) + 1) / 2;
      else if (fy)
        value = (a + sample(src, width, height, sx, sy + 1) + 1) / 2;
      uint8_t *p = dst + (size_t)(y + j) * width + x + i;
      *p = (uint8_t)(average ? ((unsigned)*p + value + 1) / 2 : value);
    }
}
static int prediction(Psvr2MpegDecoder *d, unsigned address_value, int mode) {
  unsigned x = (address_value % d->mw) * 16, y = (address_value / d->mw) * 16;
  int filled = 0;
  for (unsigned direction = 0; direction < 2; direction++)
    if (mode & (direction ? BACKWARD : FORWARD)) {
      int ref = d->picture_type == 2 ? d->future
                : direction          ? d->future
                                     : d->past;
      if (ref < 0)
        return fail(d, "MPEG prediction references are unavailable");
      int vx = d->mv[direction][0] * (d->full[direction] ? 2 : 1),
          vy = d->mv[direction][1] * (d->full[direction] ? 2 : 1);
      uint8_t *dst = d->frames[d->work], *src = d->frames[ref];
      predict_plane(dst, src, d->pw, d->ph, x, y, 16, vx, vy, filled);
      for (unsigned p = 0; p < 2; p++)
        predict_plane(dst + d->plane_bytes + p * d->plane_bytes / 4,
                      src + d->plane_bytes + p * d->plane_bytes / 4, d->pw / 2,
                      d->ph / 2, x / 2, y / 2, 8, vx / 2, vy / 2, filled);
      filled = 1;
    }
  if (!filled)
    return fail(d, "MPEG inter macroblock has no prediction");
  return 0;
}
static int vector_component(Psvr2MpegDecoder *d, unsigned direction,
                            int predictor) {
  int m = vlc(d, motion, COUNT(motion));
  if (!m)
    return predictor;
  int sign = get(d, 1) ? -1 : 1;
  unsigned residual_bits = d->fcode[direction] - 1;
  int delta = ((m - 1) << residual_bits) + (int)get(d, residual_bits) + 1;
  int value = predictor + sign * delta, limit = 16 << residual_bits;
  if (value < -limit)
    value += 2 * limit;
  else if (value >= limit)
    value -= 2 * limit;
  return value;
}
static void inverse(Psvr2MpegDecoder *d, const int coeff[64], uint8_t *dst,
                    unsigned stride, int add) {
  int dc_only = 1;
  for (unsigned i = 1; i < 64; i++)
    if (coeff[i]) {
      dc_only = 0;
      break;
    }
  if (dc_only) {
    int residual = (int)floor(coeff[0] / 8.0 + 0.5);
    for (unsigned y = 0; y < 8; y++)
      for (unsigned x = 0; x < 8; x++) {
        size_t offset = (size_t)y * stride + x;
        dst[offset] = (uint8_t)clamp(residual + (add ? dst[offset] : 0), 255);
      }
    return;
  }
  double rows[64];
  for (unsigned v = 0; v < 8; v++)
    for (unsigned x = 0; x < 8; x++) {
      double sum = 0;
      for (unsigned u = 0; u < 8; u++)
        sum += coeff[v * 8 + u] * d->basis[u][x];
      rows[v * 8 + x] = sum;
    }
  for (unsigned y = 0; y < 8; y++)
    for (unsigned x = 0; x < 8; x++) {
      double sum = 0;
      for (unsigned v = 0; v < 8; v++)
        sum += rows[v * 8 + x] * d->basis[v][y];
      int value = (int)floor(sum * 0.25 + 0.5) +
                  (add ? dst[(size_t)y * stride + x] : 0);
      dst[(size_t)y * stride + x] = (uint8_t)clamp(value, 255);
    }
}
static int block(Psvr2MpegDecoder *d, unsigned mb, unsigned index, int intra) {
  int coeff[64] = {0};
  int position = -1;
  if (intra) {
    unsigned component = index < 4 ? 0 : index - 3;
    int size =
        vlc(d, component ? dc_c : dc_y, component ? COUNT(dc_c) : COUNT(dc_y));
    int diff = (int)get(d, (unsigned)size);
    if (size && diff < (1 << (size - 1)))
      diff -= (1 << size) - 1;
    d->dc[component] += diff;
    coeff[0] = d->dc[component] * 8;
    position = 0;
  }
  int first = !intra;
  for (unsigned iterations = 0; iterations < 65 && !d->failed; iterations++) {
    int run, level;
    if (first && peek(d, 1) == 1) {
      get(d, 1);
      run = 0;
      level = get(d, 1) ? -1 : 1;
    } else {
      int symbol = coefficient_symbol(d);
      if (symbol == -1) {
        if (first)
          return fail(d, "Empty MPEG non-intra block");
        break;
      }
      if (symbol == -2) {
        run = (int)get(d, 6);
        int v = (int)get(d, 8);
        if (v == 0)
          level = (int)get(d, 8);
        else if (v == 128)
          level = (int)get(d, 8) - 256;
        else
          level = v > 128 ? v - 256 : v;
        if (!level)
          return fail(d, "Zero MPEG escape coefficient");
      } else {
        run = symbol / 64;
        level = symbol % 64;
        if (get(d, 1))
          level = -level;
      }
    }
    first = 0;
    position += run + 1;
    if (position >= 64)
      return fail(d, "MPEG coefficient run exceeds block");
    unsigned offset = scan[position];
    int magnitude = level < 0 ? -level : level;
    int value =
        intra ? magnitude * (int)d->quant * d->intra[offset] / 8
              : (2 * magnitude + 1) * (int)d->quant * d->inter[offset] / 16;
    if (value)
      value = (value - 1) | 1;
    value = clamp(value, 2047);
    coeff[offset] = level < 0 ? -value : value;
    if (iterations == 64)
      return fail(d, "MPEG block has no end marker");
  }
  if (d->failed)
    return -1;
  unsigned x = (mb % d->mw) * 16, y = (mb / d->mw) * 16;
  uint8_t *dest = d->frames[d->work];
  unsigned stride = d->pw;
  if (index < 4) {
    x += (index & 1) * 8;
    y += (index >> 1) * 8;
  } else {
    dest += d->plane_bytes + (index - 4) * d->plane_bytes / 4;
    stride /= 2;
    x /= 2;
    y /= 2;
  }
  inverse(d, coeff, dest + (size_t)y * stride + x, stride, !intra);
  return 0;
}
static int skipped(Psvr2MpegDecoder *d, unsigned mb) {
  if (d->picture_type == 1)
    return fail(d, "Skipped MPEG intra macroblock");
  d->dc[0] = d->dc[1] = d->dc[2] = 128;
  if (d->picture_type == 2) {
    d->mv[0][0] = d->mv[0][1] = 0;
    d->last_mode = FORWARD;
  }
  if (prediction(d, mb, d->last_mode) < 0)
    return -1;
  d->covered[mb] = 1;
  return 0;
}
static int slice(Psvr2MpegDecoder *d, unsigned row) {
  if (row < 1 || row > d->mh)
    return fail(d, "MPEG slice row exceeds picture");
  d->quant = get(d, 5);
  if (!d->quant)
    return fail(d, "Zero MPEG slice quantizer");
  while (get(d, 1))
    get(d, 8);
  d->dc[0] = d->dc[1] = d->dc[2] = 128;
  memset(d->mv, 0, sizeof(d->mv));
  d->last_mode = 0;
  int previous = (int)((row - 1) * d->mw) - 1;
  while (!d->failed) {
    int enough = ensure(d, 23);
    if (enough
            ? peek(d, 23) == 0
            : !d->bit_count || !(d->bits & ((UINT64_C(1) << d->bit_count) - 1)))
      break;
    unsigned increment = 0;
    int value;
    do {
      value = vlc(d, address, COUNT(address));
      if (value == 34)
        increment += 33;
    } while (!d->failed && (value == 34 || value == 35));
    if (d->failed)
      return -1;
    increment += (unsigned)value;
    if (!increment || increment > d->mw * d->mh)
      return fail(d, "Invalid MPEG macroblock increment");
    int mb = previous + (int)increment;
    if (mb < 0 || (unsigned)mb >= d->mw * d->mh)
      return fail(d, "MPEG macroblock outside picture");
    for (int j = previous + 1; j < mb; j++)
      if (skipped(d, (unsigned)j) < 0)
        return -1;
    const Vlc *types = d->picture_type == 1   ? intra_type
                       : d->picture_type == 2 ? predicted_type
                                              : bidirectional_type;
    size_t n = d->picture_type == 1   ? COUNT(intra_type)
               : d->picture_type == 2 ? COUNT(predicted_type)
                                      : COUNT(bidirectional_type);
    int type = vlc(d, types, n);
    if (type & QUANT) {
      d->quant = get(d, 5);
      if (!d->quant)
        return fail(d, "Zero MPEG macroblock quantizer");
    }
    if (type & INTRA) {
      memset(d->mv, 0, sizeof(d->mv));
      d->last_mode = 0;
    } else {
      d->dc[0] = d->dc[1] = d->dc[2] = 128;
      for (unsigned direction = 0; direction < 2; direction++)
        if (type & (direction ? BACKWARD : FORWARD))
          for (unsigned axis = 0; axis < 2; axis++)
            d->mv[direction][axis] =
                vector_component(d, direction, d->mv[direction][axis]);
      if (d->picture_type == 2 && !(type & FORWARD)) {
        d->mv[0][0] = d->mv[0][1] = 0;
        type |= FORWARD;
      }
      if (prediction(d, (unsigned)mb, type) < 0)
        return -1;
      d->last_mode = type & (FORWARD | BACKWARD);
    }
    int cbp = type & INTRA     ? 63
              : type & PATTERN ? vlc(d, pattern, COUNT(pattern))
                               : 0;
    if ((type & PATTERN) && !cbp)
      return fail(d, "Invalid zero MPEG coded block pattern");
    for (unsigned i = 0; i < 6; i++)
      if (cbp & (1 << (5 - i)))
        if (block(d, (unsigned)mb, i, type & INTRA) < 0)
          return -1;
    if (d->covered[mb])
      return fail(d, "Duplicate MPEG macroblock");
    d->covered[mb] = 1;
    previous = mb;
  }
  if (d->failed)
    return -1;
  return 0;
}
static int picture(Psvr2MpegDecoder *d) {
  if (!d->frames[0])
    return fail(d, "MPEG picture precedes sequence header");
  get(d, 10);
  d->picture_type = get(d, 3);
  get(d, 16);
  if (d->picture_type < 1 || d->picture_type > 3)
    return fail(d, "Unsupported MPEG picture type");
  memset(d->full, 0, sizeof(d->full));
  memset(d->fcode, 0, sizeof(d->fcode));
  for (unsigned direction = 0; direction < (d->picture_type == 3   ? 2
                                            : d->picture_type == 2 ? 1
                                                                   : 0);
       direction++) {
    d->full[direction] = get(d, 1);
    d->fcode[direction] = get(d, 3);
    if (!d->fcode[direction])
      return fail(d, "Zero MPEG motion f_code");
  }
  while (get(d, 1))
    get(d, 8);
  for (d->work = 0; d->work == d->past || d->work == d->future; d->work++)
    ;
  if (d->work >= 3)
    return fail(d, "MPEG reference allocation failure");
  memset(d->covered, 0, (size_t)d->mw * d->mh);
  unsigned slices = 0;
  int c;
  while ((c = code(d)) >= 0) {
    if (c >= 1 && c <= 0xaf) {
      slices++;
      if (slice(d, (unsigned)c) < 0)
        return -1;
    } else if (c == 0xb2)
      continue;
    else {
      d->pending_code = c;
      break;
    }
  }
  if (d->failed)
    return -1;
  if (!slices)
    return fail(d, "MPEG picture contains no slices");
  for (unsigned i = 0; i < d->mw * d->mh; i++)
    if (!d->covered[i])
      return fail(d, "MPEG picture has missing macroblocks");
  return 0;
}
static void frame(Psvr2MpegDecoder *d, int index, Psvr2MpegFrame *out) {
  out->time = (double)d->display_index++ / d->rate;
  out->width = d->width;
  out->height = d->height;
  out->y = (Psvr2MpegPlane){d->pw, d->ph, d->pw, d->frames[index]};
  out->cb = (Psvr2MpegPlane){d->pw / 2, d->ph / 2, d->pw / 2,
                             d->frames[index] + d->plane_bytes};
  out->cr = (Psvr2MpegPlane){d->pw / 2, d->ph / 2, d->pw / 2,
                             d->frames[index] + d->plane_bytes * 5 / 4};
}
int psvr2_mpeg_read_frame(Psvr2MpegDecoder *d, Psvr2MpegFrame *out) {
  if (!d || !out) {
    errno = EINVAL;
    return -1;
  }
  if (d->failed)
    return -1;
  while (!d->finished) {
    int c = code(d);
    if (c < 0 || c == 0xb7) {
      d->finished = 1;
      break;
    }
    if (c == 0xb3) {
      if (sequence(d) < 0)
        return -1;
    } else if (c == 0xb5)
      return fail(d, "MPEG2 video extensions are unsupported");
    else if (c == 0) {
      if (picture(d) < 0)
        return -1;
      if (d->picture_type == 3) {
        frame(d, d->work, out);
        return 1;
      }
      int previous = d->future;
      d->past = previous;
      d->future = d->work;
      if (previous >= 0) {
        frame(d, previous, out);
        return 1;
      }
    }
  }
  if (d->failed)
    return -1;
  if (d->future >= 0 && !d->final_emitted) {
    d->final_emitted = 1;
    frame(d, d->future, out);
    return 1;
  }
  return 0;
}
int psvr2_mpeg_rewind(Psvr2MpegDecoder *d) {
  if (!d) {
    errno = EINVAL;
    return -1;
  }
  if (fseek(d->file, 0, SEEK_SET) < 0)
    return fail(d, "MPEG rewind failed");
  clearerr(d->file);
  d->packet_left = d->source_end = d->failed = 0;
  d->video_id = -1;
  d->bit_count = 0;
  d->bits = 0;
  d->pending_code = -1;
  d->past = d->future = -1;
  d->finished = d->final_emitted = 0;
  d->display_index = 0;
  d->error[0] = 0;
  int c;
  while ((c = code(d)) >= 0 && c != 0xb3)
    ;
  if (c != 0xb3)
    return fail(d, "MPEG sequence header not found");
  return sequence(d);
}
Psvr2MpegDecoder *psvr2_mpeg_open(const char *path, char *error,
                                  size_t capacity) {
  Psvr2MpegDecoder *d = calloc(1, sizeof(*d));
  if (!d) {
    if (error && capacity)
      snprintf(error, capacity, "MPEG decoder allocation failed");
    return NULL;
  }
  d->file = path ? fopen(path, "rb") : NULL;
  if (!d->file) {
    snprintf(d->error, sizeof(d->error), "Cannot open MPEG input");
    goto bad;
  }
  unsigned p = 0xffffff;
  int c, id = -1;
  for (unsigned i = 0; i < 1048576 && (c = raw(d)) != EOF; i++) {
    p = ((p << 8) | (unsigned)c) & 0xffffff;
    if (p == 1) {
      id = raw(d);
      break;
    }
  }
  if (id < 0) {
    snprintf(d->error, sizeof(d->error), "MPEG start code not found");
    goto bad;
  }
  d->program = id == 0xba;
  if (!d->program && id != 0xb3) {
    snprintf(d->error, sizeof(d->error),
             "MPEG sequence or pack header expected");
    goto bad;
  }
  for (unsigned u = 0; u < 8; u++)
    for (unsigned x = 0; x < 8; x++)
      d->basis[u][x] = (u ? 1 : 1 / sqrt(2.0)) *
                       cos((2 * x + 1) * u * 3.14159265358979323846 / 16);
  for (size_t i = 0; i < COUNT(coefficients); i++) {
    if (coefficients[i].length > 10)
      continue;
    unsigned suffix = 10 - coefficients[i].length;
    unsigned start = coefficients[i].code << suffix;
    for (unsigned j = 0; j < (1u << suffix); j++) {
      d->coefficient_prefix[start + j].value = (int16_t)coefficients[i].value;
      d->coefficient_prefix[start + j].length = (uint8_t)coefficients[i].length;
    }
  }
  if (psvr2_mpeg_rewind(d) < 0)
    goto bad;
  if (error && capacity)
    error[0] = 0;
  return d;
bad:
  if (error && capacity)
    snprintf(error, capacity, "%s", d->error);
  psvr2_mpeg_close(d);
  return NULL;
}
unsigned psvr2_mpeg_width(const Psvr2MpegDecoder *d) {
  return d ? d->width : 0;
}
unsigned psvr2_mpeg_height(const Psvr2MpegDecoder *d) {
  return d ? d->height : 0;
}
double psvr2_mpeg_frame_rate(const Psvr2MpegDecoder *d) {
  return d ? d->rate : 0;
}
const char *psvr2_mpeg_error(const Psvr2MpegDecoder *d) {
  return d ? d->error : "Invalid MPEG decoder";
}
int psvr2_mpeg_seek(Psvr2MpegDecoder *d, double seconds, Psvr2MpegFrame *out) {
  if (!d || !out || !isfinite(seconds) || seconds < 0) {
    errno = EINVAL;
    return -1;
  }
  if (psvr2_mpeg_rewind(d) < 0)
    return -1;
  int r;
  do {
    r = psvr2_mpeg_read_frame(d, out);
  } while (r == 1 && out->time < seconds);
  return r;
}
void psvr2_mpeg_close(Psvr2MpegDecoder *d) {
  if (!d)
    return;
  if (d->file)
    fclose(d->file);
  for (unsigned i = 0; i < 3; i++)
    free(d->frames[i]);
  free(d->covered);
  free(d);
}
