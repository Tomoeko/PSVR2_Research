#include "../open_vrhmd.h"

/* Integer HSV to RGB: hue wraps at 360; saturation/value are 0..255. */
void hsv2rgb(int h, int s, int v, uint8_t *ro, uint8_t *go, uint8_t *bo) {
  h %= 360;
  if (h < 0)
    h += 360;
  if (s == 0) {
    *ro = *go = *bo = (uint8_t)v;
    return;
  }
  int region = h / 60;
  int remainder = (h - (region * 60)) * 255 / 60;
  int p = (v * (255 - s)) >> 8;
  int q = (v * (255 - ((s * remainder) >> 8))) >> 8;
  int t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
  switch (region) {
  case 0:
    *ro = v;
    *go = t;
    *bo = p;
    break;
  case 1:
    *ro = q;
    *go = v;
    *bo = p;
    break;
  case 2:
    *ro = p;
    *go = v;
    *bo = t;
    break;
  case 3:
    *ro = p;
    *go = q;
    *bo = v;
    break;
  case 4:
    *ro = t;
    *go = p;
    *bo = v;
    break;
  default:
    *ro = v;
    *go = p;
    *bo = q;
    break;
  }
}
