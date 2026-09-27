#include "../../target/psvr2/tools/open_vrhmd/codec/mpeg.c"
#include <assert.h>

static void set_bits(Psvr2MpegDecoder *d, unsigned value, unsigned length) {
  memset(d, 0, sizeof(*d));
  d->bits = value;
  d->bit_count = length;
}
static void check_table(const Vlc *table, size_t count) {
  for (size_t i = 0; i < count; i++) {
    Psvr2MpegDecoder d;
    set_bits(&d, table[i].code, table[i].length);
    assert(vlc(&d, table, count) == table[i].value && !d.failed &&
           !d.bit_count);
    for (size_t j = 0; j < i; j++) {
      assert(table[j].length != table[i].length ||
             table[j].code != table[i].code);
      unsigned shortest =
          table[j].length < table[i].length ? table[j].length : table[i].length;
      assert((table[j].code >> (table[j].length - shortest)) !=
             (table[i].code >> (table[i].length - shortest)));
    }
  }
}
int main(void) {
  check_table(address, COUNT(address));
  check_table(pattern, COUNT(pattern));
  check_table(coefficients, COUNT(coefficients));
  check_table(motion, COUNT(motion));
  check_table(intra_type, COUNT(intra_type));
  check_table(predicted_type, COUNT(predicted_type));
  check_table(bidirectional_type, COUNT(bidirectional_type));
  check_table(dc_y, COUNT(dc_y));
  check_table(dc_c, COUNT(dc_c));
  Psvr2MpegDecoder d;
  set_bits(&d, 3, 3);
  d.fcode[0] = 1; /* 01 sign=1 => negative one */
  assert(vector_component(&d, 0, 0) == -1);
  set_bits(&d, 2, 3);
  d.fcode[0] = 1;
  assert(vector_component(&d, 0, 15) == -16);
  set_bits(&d, 3, 3);
  d.fcode[0] = 1;
  assert(vector_component(&d, 0, -16) == 15);
  set_bits(&d, 7, 4);
  d.fcode[0] = 2; /* 01 sign=1 residual=1 => -2 */
  assert(vector_component(&d, 0, 0) == -2);
  uint8_t source[64], destination[64];
  for (unsigned y = 0; y < 8; y++)
    for (unsigned x = 0; x < 8; x++)
      source[y * 8 + x] = (uint8_t)(x * 10 + y * 2);
  predict_plane(destination, source, 8, 8, 0, 0, 8, -1, -1, 0);
  assert(destination[0] == 0 && destination[9] == 6 && destination[63] == 78);
  assert(psvr2_mpeg_seek(NULL, 0, NULL) == -1);
  return 0;
}
