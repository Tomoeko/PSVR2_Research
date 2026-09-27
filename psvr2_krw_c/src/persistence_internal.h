#ifndef PSVR2_PERSISTENCE_INTERNAL_H
#define PSVR2_PERSISTENCE_INTERNAL_H

#include "psvr2/common.h"

bool psvr2_persist_name_valid(const char *name);
bool psvr2_persist_parse_sha256(
    const char *text, char digest[65]);
bool psvr2_persist_mount_matches(
    const char *mounts, bool read_only);
bool psvr2_persist_firmware_supported(
    uint32_t version, bool firmware_forced);

#endif
