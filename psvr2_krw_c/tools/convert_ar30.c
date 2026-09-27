#include "psvr2/common.h"

#include <stdlib.h>
#include <string.h>

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s <input.raw> [output.ppm] [--frame N]\n"
        "          [--width W --height H --pitch BYTES]\n"
        "Defaults: 1920x1080, pitch 7840.\n", p);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const char *output = "frame.ppm";
    uint64_t width = 1920, height = 1080, pitch = 7840, frame = 0;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--frame") && i + 1 < argc) {
            if (!psvr2_parse_u64(argv[++i], &frame)) return 1;
        } else if (!strcmp(argv[i], "--width") && i + 1 < argc) {
            if (!psvr2_parse_u64(argv[++i], &width)) return 1;
        } else if (!strcmp(argv[i], "--height") && i + 1 < argc) {
            if (!psvr2_parse_u64(argv[++i], &height)) return 1;
        } else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) {
            if (!psvr2_parse_u64(argv[++i], &pitch)) return 1;
        } else if (argv[i][0] != '-') {
            output = argv[i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (!width || !height || width > UINT32_MAX || height > UINT32_MAX ||
        pitch < width * 4 || pitch > UINT32_MAX ||
        height > SIZE_MAX / pitch) return 1;
    uint32_t w = (uint32_t)width, h = (uint32_t)height;
    uint32_t row_pitch = (uint32_t)pitch;
    size_t frame_size = (size_t)row_pitch * h;
    if (frame > SIZE_MAX / frame_size) return 1;
    size_t frame_offset = (size_t)frame * frame_size;
    psvr2_buffer raw = {0};
    if (!psvr2_read_file(argv[1], &raw) ||
        frame_offset > raw.len || raw.len - frame_offset < frame_size) {
        fprintf(stderr, "Frame %llu is not present in the input.\n",
                (unsigned long long)frame);
        psvr2_buffer_free(&raw);
        return 1;
    }
    FILE *f = fopen(output, "wb");
    if (!f) { psvr2_buffer_free(&raw); return 1; }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t p = psvr2_load_le32(raw.data + frame_offset +
                                         (size_t)y * row_pitch + x * 4U);
            uint8_t rgb[3] = {
                (uint8_t)((p >> 22) & 0xffU),
                (uint8_t)((p >> 12) & 0xffU),
                (uint8_t)((p >> 2) & 0xffU)
            };
            if (fwrite(rgb, 1, sizeof(rgb), f) != sizeof(rgb)) {
                fclose(f); psvr2_buffer_free(&raw); return 1;
            }
        }
    }
    fclose(f);
    psvr2_buffer_free(&raw);
    return 0;
}
