#include "psvr2/arm64.h"

bool psvr2_arm64_branch(uint64_t from, uint64_t to, bool link,
                        uint32_t *instruction) {
    if (!instruction || (from & 3U) != 0 || (to & 3U) != 0)
        return false;
    int64_t offset = (int64_t)(to - from);
    if (offset < -INT64_C(0x8000000) ||
        offset > INT64_C(0x7fffffc))
        return false;
    uint32_t immediate =
        (uint32_t)(((uint64_t)(offset >> 2)) &
                   UINT64_C(0x03ffffff));
    *instruction =
        (link ? UINT32_C(0x94000000) : UINT32_C(0x14000000)) |
        immediate;
    return true;
}

bool psvr2_arm64_ldr_literal(unsigned reg, uint64_t from,
                             uint64_t literal, uint32_t *instruction) {
    if (!instruction || reg > 31U ||
        (from & 3U) != 0 || (literal & 3U) != 0)
        return false;
    int64_t offset = (int64_t)(literal - from);
    if (offset < -INT64_C(0x100000) ||
        offset > INT64_C(0xffffc))
        return false;
    *instruction =
        UINT32_C(0x58000000) |
        (((uint32_t)(offset >> 2) & UINT32_C(0x7ffff)) << 5) |
        reg;
    return true;
}
