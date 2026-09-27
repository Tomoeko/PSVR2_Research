#include "png.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3 || !(argc & 1)) return 2;
    for (int index = 1; index < argc; index += 2) {
        FILE *file = fopen(argv[index + 1], "rb");
        if (!file || fseek(file, 0, SEEK_END)) return 2;
        long length = ftell(file);
        if (length < 0 || fseek(file, 0, SEEK_SET)) return 2;
        unsigned char *bytes = malloc((size_t)length + 1);
        if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length)
            return 2;
        if (fclose(file)) return 2;
        Psvr2PngImage image = {0};
        bool decoded = psvr2_png_decode(bytes, (size_t)length, &image);
        bool expected = argv[index][0] == '1';
        if (decoded != expected) return 1;
        if (decoded) {
            volatile unsigned checksum = 0;
            for (size_t pixel = 0; pixel < (size_t)image.width * image.height * 4; ++pixel)
                checksum += image.pixels[pixel];
            (void)checksum;
        } else if (image.pixels || image.width || image.height) return 1;
        psvr2_png_free(&image);
        psvr2_png_free(&image);
        free(bytes);
    }
    return 0;
}
