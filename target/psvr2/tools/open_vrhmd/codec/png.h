#ifndef PSVR2_CODEC_PNG_H
#define PSVR2_CODEC_PNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Psvr2PngImage {
    uint32_t width;
    uint32_t height;
    uint8_t *pixels;
} Psvr2PngImage;

/* Straight RGBA8, top row first. Output starts empty and owns its pixels only
 * after success. Release an existing image before passing it as output again. */
bool psvr2_png_decode(const uint8_t *data, size_t size, Psvr2PngImage *image);
bool psvr2_png_load(const char *path, Psvr2PngImage *image);
void psvr2_png_free(Psvr2PngImage *image);

#endif
