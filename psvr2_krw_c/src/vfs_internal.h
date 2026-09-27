#ifndef PSVR2_VFS_INTERNAL_H
#define PSVR2_VFS_INTERNAL_H

#include "psvr2/common.h"

typedef struct {
    uint64_t index;
    uint64_t address;
} psvr2_vfs_page;

typedef struct {
    psvr2_vfs_page *entries;
    size_t count;
    size_t capacity;
} psvr2_vfs_page_map;

bool psvr2_vfs_page_map_put(
    psvr2_vfs_page_map *map, uint64_t index, uint64_t address);
uint64_t psvr2_vfs_page_map_get(
    const psvr2_vfs_page_map *map, uint64_t index);
void psvr2_vfs_page_map_free(psvr2_vfs_page_map *map);

#endif
