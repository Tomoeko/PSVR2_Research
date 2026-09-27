#ifndef PSVR2_KERNEL_IMAGES_INTERNAL_H
#define PSVR2_KERNEL_IMAGES_INTERNAL_H

#include "psvr2/constants.h"

typedef struct {
    const char *name;
    uint64_t address;
    const uint8_t *bytes;
    size_t length;
} psvr2_kernel_anchor;

#define PSVR2_KERNEL_ANCHOR_COUNT 5U

void psvr2_kernel_execution_anchors(
    const psvr2_constants *constants,
    psvr2_kernel_anchor out[PSVR2_KERNEL_ANCHOR_COUNT]);

#endif
