#ifndef PSVR2_VFS_H
#define PSVR2_VFS_H

#include "psvr2/rw.h"

typedef enum {
    PSVR2_VFS_UNKNOWN,
    PSVR2_VFS_DIRECTORY,
    PSVR2_VFS_FILE,
    PSVR2_VFS_SYMLINK,
    PSVR2_VFS_CHAR,
    PSVR2_VFS_BLOCK,
    PSVR2_VFS_ERROR
} psvr2_vfs_type;

typedef bool (*psvr2_vfs_entry_fn)(void *opaque, uint64_t dentry,
                                   const char *name, psvr2_vfs_type type,
                                   uint64_t size);
typedef bool (*psvr2_vfs_mount_fn)(void *opaque, const char *name,
                                   uint64_t root, uint64_t mount_base);
typedef bool (*psvr2_vfs_page_fn)(void *opaque, uint64_t index, uint64_t virt);

typedef struct psvr2_vfs {
    psvr2_krw *krw;
} psvr2_vfs;

void psvr2_vfs_init(psvr2_vfs *vfs, psvr2_krw *krw);
uint64_t psvr2_vfs_root(psvr2_vfs *vfs);
bool psvr2_vfs_dentry_name(psvr2_vfs *vfs, uint64_t dentry,
                           char *out, size_t max_len);
bool psvr2_vfs_inode_info(psvr2_vfs *vfs, uint64_t dentry,
                          psvr2_vfs_type *type, uint64_t *size);
uint64_t psvr2_vfs_resolve_child(psvr2_vfs *vfs, uint64_t parent,
                                 const char *name);
psvr2_walk_result psvr2_vfs_resolve_child_ex(
    psvr2_vfs *vfs, uint64_t parent, const char *name,
    uint64_t *child);
uint64_t psvr2_vfs_resolve_path(psvr2_vfs *vfs, uint64_t start,
                                const char *path);
bool psvr2_vfs_list(psvr2_vfs *vfs, uint64_t parent,
                    psvr2_vfs_entry_fn fn, void *opaque);
psvr2_walk_result psvr2_vfs_list_ex(
    psvr2_vfs *vfs, uint64_t parent,
    psvr2_vfs_entry_fn fn, void *opaque);
bool psvr2_vfs_mounts(psvr2_vfs *vfs, psvr2_vfs_mount_fn fn, void *opaque);
psvr2_walk_result psvr2_vfs_mounts_ex(
    psvr2_vfs *vfs, psvr2_vfs_mount_fn fn, void *opaque);
uint64_t psvr2_vfs_find_mount(psvr2_vfs *vfs, const char *name);
uint64_t psvr2_vfs_follow_mount(psvr2_vfs *vfs, uint64_t dentry);
uint64_t psvr2_vfs_mount_point(psvr2_vfs *vfs, uint64_t root);
bool psvr2_vfs_read_file(psvr2_vfs *vfs, uint64_t dentry,
                         psvr2_buffer *out, size_t max_size);
bool psvr2_vfs_file_pages(psvr2_vfs *vfs, uint64_t inode,
                          psvr2_vfs_page_fn fn, void *opaque);
const char *psvr2_vfs_type_name(psvr2_vfs_type type);

#endif
