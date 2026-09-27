#ifndef PSVR2_ARM64_H
#define PSVR2_ARM64_H

#include "psvr2/common.h"

bool psvr2_arm64_branch(uint64_t from, uint64_t to, bool link,
                        uint32_t *instruction);
bool psvr2_arm64_ldr_literal(unsigned reg, uint64_t from,
                             uint64_t literal, uint32_t *instruction);

#endif
