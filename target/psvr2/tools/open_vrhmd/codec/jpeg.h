#ifndef PSVR2_CODEC_JPEG_H
#define PSVR2_CODEC_JPEG_H

#include "png.h"

/* JPEG uses the same owned, top-first RGBA8 image storage as PNG. */
bool psvr2_jpeg_decode(const uint8_t *data, size_t size, Psvr2PngImage *image);
bool psvr2_jpeg_load(const char *path, Psvr2PngImage *image);

#endif
