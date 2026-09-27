#include "../open_vrhmd.h"

#ifdef GPU_RENDER
#include <strings.h>

bool media_path_is_mpeg(const char *path) {
  const char *extension = path ? strrchr(path, '.') : NULL;
  return extension && (!strcasecmp(extension, ".mpg") ||
                       !strcasecmp(extension, ".mpeg") ||
                       !strcasecmp(extension, ".m1v") ||
                       !strcasecmp(extension, ".mpv"));
}

bool media_flip_png_rows(Psvr2PngImage *image) {
  if (!image || !image->pixels || !image->width || !image->height ||
      image->width > 8192 || image->height > 8192)
    return false;
  const size_t row_bytes = (size_t)image->width * 4;
  for (uint32_t row = 0; row < image->height / 2; row++) {
    uint8_t *top = image->pixels + row * row_bytes;
    uint8_t *bottom = image->pixels + (image->height - row - 1) * row_bytes;
    for (size_t byte = 0; byte < row_bytes; byte++) {
      uint8_t saved = top[byte];
      top[byte] = bottom[byte];
      bottom[byte] = saved;
    }
  }
  return true;
}
#endif
