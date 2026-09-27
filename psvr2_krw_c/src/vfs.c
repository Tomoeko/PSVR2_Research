#include "psvr2/vfs.h"
#include "kernel_list_internal.h"
#include "vfs_internal.h"

#include <stdlib.h>
#include <string.h>

void psvr2_vfs_init(psvr2_vfs *vfs, psvr2_krw *krw) {
    vfs->krw = krw;
}

static const psvr2_firmware_profile *vfs_profile(psvr2_vfs *vfs) {
    return vfs && vfs->krw && vfs->krw->ex &&
                   vfs->krw->ex->constants
               ? &vfs->krw->ex->constants->fw : NULL;
}

static uint64_t page_to_virt(
    const psvr2_firmware_profile *profile, uint64_t page) {
    if (!profile || page < profile->vmemmap_base) return 0;
    uint64_t pfn =
        (page - profile->vmemmap_base) / PSVR2_STRUCT_PAGE_SIZE;
    return pfn * PSVR2_PAGE_SIZE + profile->page_offset_base;
}

bool psvr2_vfs_dentry_name(psvr2_vfs *vfs, uint64_t dentry,
                           char *out, size_t max_len) {
    const psvr2_firmware_profile *profile = vfs_profile(vfs);
    if (!profile) return false;
    uint64_t ptr = psvr2_krw_read_ptr(
        vfs->krw, dentry + profile->dentry_name_offset);
    if (!ptr || !psvr2_krw_read_string(vfs->krw, ptr, out, max_len)) {
        if (max_len) snprintf(out, max_len, "?");
        return false;
    }
    return true;
}

static bool inode_info_ptr(psvr2_vfs *vfs, uint64_t inode,
                           psvr2_vfs_type *type, uint64_t *size) {
    uint8_t data[0x40];
    if (!inode || !psvr2_krw_read(vfs->krw, inode, data, sizeof(data))) {
        if (type) *type = PSVR2_VFS_ERROR;
        if (size) *size = 0;
        return false;
    }
    const psvr2_firmware_profile *profile = vfs_profile(vfs);
    if (!profile || profile->inode_size_offset + 8 > sizeof(data))
        return false;
    uint16_t mode =
        psvr2_load_le16(data + profile->inode_mode_offset);
    if (size)
        *size = psvr2_load_le64(
            data + profile->inode_size_offset);
    if (type) {
        switch (mode & 0xf000U) {
        case 0x4000: *type = PSVR2_VFS_DIRECTORY; break;
        case 0x8000: *type = PSVR2_VFS_FILE; break;
        case 0xa000: *type = PSVR2_VFS_SYMLINK; break;
        case 0x2000: *type = PSVR2_VFS_CHAR; break;
        case 0x6000: *type = PSVR2_VFS_BLOCK; break;
        default: *type = PSVR2_VFS_UNKNOWN; break;
        }
    }
    return true;
}

bool psvr2_vfs_inode_info(psvr2_vfs *vfs, uint64_t dentry,
                          psvr2_vfs_type *type, uint64_t *size) {
    const psvr2_firmware_profile *profile = vfs_profile(vfs);
    return profile && inode_info_ptr(
        vfs, psvr2_krw_read_ptr(
                 vfs->krw, dentry + profile->dentry_inode_offset),
        type, size);
}

uint64_t psvr2_vfs_root(psvr2_vfs *vfs) {
    const psvr2_firmware_profile *profile = vfs_profile(vfs);
    if (!profile) return 0;
    uint64_t fs = psvr2_krw_read_ptr(
        vfs->krw, profile->init_task + profile->task_fs_offset);
    return fs ? psvr2_krw_read_ptr(
                    vfs->krw, fs + profile->fs_root_offset) : 0;
}

typedef struct {
    psvr2_vfs *vfs;
    const psvr2_firmware_profile *profile;
    const char *wanted;
    uint64_t found;
} resolve_child_context;

static bool resolve_child_node(void *opaque, uint64_t node) {
    resolve_child_context *context = opaque;
    uint64_t child =
        node - context->profile->dentry_child_offset;
    char child_name[256];
    if (psvr2_vfs_dentry_name(
            context->vfs, child, child_name, sizeof(child_name)) &&
        strcmp(child_name, context->wanted) == 0) {
        context->found = child;
        return false;
    }
    return true;
}

psvr2_walk_result psvr2_vfs_resolve_child_ex(
    psvr2_vfs *vfs, uint64_t parent, const char *name,
    uint64_t *child) {
    const psvr2_firmware_profile *profile = vfs_profile(vfs);
    if (child) *child = 0;
    if (!profile || !parent || !name || !child)
        return PSVR2_WALK_INVALID;
    uint64_t head =
        parent + profile->dentry_subdirs_offset;
    resolve_child_context context = {
        .vfs = vfs, .profile = profile, .wanted = name
    };
    psvr2_walk_result result = psvr2_kernel_list_walk(
        vfs->krw, psvr2_krw_read_ptr(vfs->krw, head),
        head, false, 500, resolve_child_node, &context);
    *child = context.found;
    return result;
}

uint64_t psvr2_vfs_resolve_child(psvr2_vfs *vfs, uint64_t parent,
                                 const char *name) {
    uint64_t child = 0;
    (void)psvr2_vfs_resolve_child_ex(vfs, parent, name, &child);
    return child;
}

typedef struct {
    psvr2_vfs *vfs;
    const psvr2_firmware_profile *profile;
    psvr2_vfs_mount_fn fn;
    void *opaque;
} mount_walk_context;

static bool visit_mount_node(void *opaque, uint64_t node) {
    mount_walk_context *context = opaque;
    uint64_t base = node - context->profile->mount_list_offset;
    uint64_t mp = psvr2_krw_read_ptr(
        context->vfs->krw,
        base + context->profile->mount_point_offset);
    uint64_t root = psvr2_krw_read_ptr(
        context->vfs->krw,
        base + context->profile->mount_root_offset);
    char name[256] = "/";
    if (mp != root)
        (void)psvr2_vfs_dentry_name(
            context->vfs, mp, name, sizeof(name));
    return !context->fn ||
           context->fn(context->opaque, name, root, base);
}

psvr2_walk_result psvr2_vfs_mounts_ex(
    psvr2_vfs *vfs, psvr2_vfs_mount_fn fn, void *opaque) {
    const psvr2_firmware_profile *profile = vfs_profile(vfs);
    if (!profile) return PSVR2_WALK_INVALID;
    uint64_t nsp = psvr2_krw_read_ptr(
        vfs->krw,
        profile->init_task + profile->task_nsproxy_offset);
    if (!nsp) return PSVR2_WALK_READ_ERROR;
    uint64_t ns = psvr2_krw_read_ptr(
        vfs->krw, nsp + profile->ns_mnt_ns_offset);
    if (!ns) return PSVR2_WALK_READ_ERROR;
    uint64_t head =
        ns + profile->mount_namespace_list_offset;
    mount_walk_context context = {
        .vfs = vfs, .profile = profile, .fn = fn, .opaque = opaque
    };
    return psvr2_kernel_list_walk(
        vfs->krw, psvr2_krw_read_ptr(vfs->krw, head),
        head, false, 100, visit_mount_node, &context);
}

bool psvr2_vfs_mounts(
    psvr2_vfs *vfs, psvr2_vfs_mount_fn fn, void *opaque) {
    psvr2_walk_result result =
        psvr2_vfs_mounts_ex(vfs, fn, opaque);
    return result == PSVR2_WALK_COMPLETE ||
           result == PSVR2_WALK_CALLBACK_STOPPED;
}

typedef struct {
    const char *wanted;
    uint64_t root;
} find_mount_ctx;

static bool find_mount_cb(void *opaque, const char *name, uint64_t root,
                          uint64_t base) {
    (void)base;
    find_mount_ctx *ctx = opaque;
    if (strcmp(name, ctx->wanted) == 0) {
        ctx->root = root;
        return false;
    }
    return true;
}

uint64_t psvr2_vfs_find_mount(psvr2_vfs *vfs, const char *name) {
    find_mount_ctx ctx = {name, 0};
    (void)psvr2_vfs_mounts(vfs, find_mount_cb, &ctx);
    return ctx.root;
}

typedef struct {
    psvr2_vfs *vfs;
    uint64_t input;
    uint64_t output;
    bool reverse;
} mount_lookup_ctx;

static bool mount_lookup_cb(void *opaque, const char *name, uint64_t root,
                            uint64_t base) {
    (void)name;
    mount_lookup_ctx *ctx = opaque;
    const psvr2_firmware_profile *profile =
        vfs_profile(ctx->vfs);
    if (!profile) return false;
    uint64_t mp = psvr2_krw_read_ptr(
        ctx->vfs->krw, base + profile->mount_point_offset);
    if ((!ctx->reverse && ctx->input == mp && mp != root) ||
        (ctx->reverse && ctx->input == root && mp != root)) {
        ctx->output = ctx->reverse ? mp : root;
        return false;
    }
    return true;
}

uint64_t psvr2_vfs_follow_mount(psvr2_vfs *vfs, uint64_t dentry) {
    mount_lookup_ctx ctx = {vfs, dentry, 0, false};
    (void)psvr2_vfs_mounts(vfs, mount_lookup_cb, &ctx);
    return ctx.output ? ctx.output : dentry;
}

uint64_t psvr2_vfs_mount_point(psvr2_vfs *vfs, uint64_t root) {
    mount_lookup_ctx ctx = {vfs, root, 0, true};
    (void)psvr2_vfs_mounts(vfs, mount_lookup_cb, &ctx);
    return ctx.output;
}

uint64_t psvr2_vfs_resolve_path(psvr2_vfs *vfs, uint64_t start,
                                const char *path) {
    const psvr2_firmware_profile *profile = vfs_profile(vfs);
    if (!profile) return 0;
    if (!path || !*path) return start;
    if (*path == '/') start = psvr2_vfs_root(vfs);
    char *copy = psvr2_strdup(path);
    if (!copy) return 0;
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part;
         part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) {
            uint64_t mp = psvr2_vfs_mount_point(vfs, start);
            if (mp) start = mp;
            uint64_t parent = psvr2_krw_read_ptr(
                vfs->krw, start + profile->dentry_parent_offset);
            if (parent) start = parent;
            continue;
        }
        start = psvr2_vfs_follow_mount(vfs, start);
        start = psvr2_vfs_resolve_child(vfs, start, part);
        if (!start) break;
    }
    free(copy);
    return start ? psvr2_vfs_follow_mount(vfs, start) : 0;
}

typedef struct {
    psvr2_vfs *vfs;
    const psvr2_firmware_profile *profile;
    psvr2_vfs_entry_fn fn;
    void *opaque;
} directory_walk_context;

static bool visit_directory_node(void *opaque, uint64_t node) {
    directory_walk_context *context = opaque;
    uint64_t child =
        node - context->profile->dentry_child_offset;
    uint8_t meta[16];
    char name[256] = "?";
    psvr2_vfs_type type = PSVR2_VFS_ERROR;
    uint64_t size = 0;
    if (psvr2_krw_read(
            context->vfs->krw,
            child + context->profile->dentry_name_offset,
            meta, sizeof(meta))) {
        uint64_t name_ptr = psvr2_load_le64(meta);
        uint64_t inode_ptr = psvr2_load_le64(meta + 8);
        if (name_ptr)
            (void)psvr2_krw_read_string(
                context->vfs->krw, name_ptr,
                name, sizeof(name));
        (void)inode_info_ptr(
            context->vfs, inode_ptr, &type, &size);
    }
    return !context->fn ||
           context->fn(
               context->opaque, child, name, type, size);
}

psvr2_walk_result psvr2_vfs_list_ex(
    psvr2_vfs *vfs, uint64_t parent,
    psvr2_vfs_entry_fn fn, void *opaque) {
    const psvr2_firmware_profile *profile = vfs_profile(vfs);
    if (!profile || !parent) return PSVR2_WALK_INVALID;
    uint64_t head =
        parent + profile->dentry_subdirs_offset;
    directory_walk_context context = {
        .vfs = vfs, .profile = profile, .fn = fn, .opaque = opaque
    };
    return psvr2_kernel_list_walk(
        vfs->krw, psvr2_krw_read_ptr(vfs->krw, head),
        head, false, 200, visit_directory_node, &context);
}

bool psvr2_vfs_list(psvr2_vfs *vfs, uint64_t parent,
                    psvr2_vfs_entry_fn fn, void *opaque) {
    psvr2_walk_result result =
        psvr2_vfs_list_ex(vfs, parent, fn, opaque);
    return result == PSVR2_WALK_COMPLETE ||
           result == PSVR2_WALK_CALLBACK_STOPPED;
}

bool psvr2_vfs_page_map_put(
    psvr2_vfs_page_map *map, uint64_t index, uint64_t address) {
    if (!map || !address) return false;
    size_t low = 0;
    size_t high = map->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (map->entries[middle].index < index)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < map->count && map->entries[low].index == index) {
        map->entries[low].address = address;
        return true;
    }
    if (map->count == map->capacity) {
        size_t capacity = map->capacity ? map->capacity * 2 : 64;
        if (capacity < map->capacity ||
            capacity > SIZE_MAX / sizeof(*map->entries))
            return false;
        void *entries = realloc(
            map->entries, capacity * sizeof(*map->entries));
        if (!entries) return false;
        map->entries = entries;
        map->capacity = capacity;
    }
    if (low < map->count)
        memmove(
            map->entries + low + 1, map->entries + low,
            (map->count - low) * sizeof(*map->entries));
    map->entries[low] = (psvr2_vfs_page){
        .index = index,
        .address = address
    };
    ++map->count;
    return true;
}

uint64_t psvr2_vfs_page_map_get(
    const psvr2_vfs_page_map *map, uint64_t index) {
    if (!map) return 0;
    size_t low = 0;
    size_t high = map->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        uint64_t candidate = map->entries[middle].index;
        if (candidate < index)
            low = middle + 1;
        else if (candidate > index)
            high = middle;
        else
            return map->entries[middle].address;
    }
    return 0;
}

void psvr2_vfs_page_map_free(psvr2_vfs_page_map *map) {
    if (!map) return;
    free(map->entries);
    memset(map, 0, sizeof(*map));
}

static bool walk_radix(psvr2_vfs *vfs, uint64_t node, unsigned height,
                       uint64_t base_index, uint64_t max_index,
                       psvr2_vfs_page_map *pages);

static bool collect_inode_pages(
    psvr2_vfs *vfs, uint64_t inode, uint64_t max_index,
    psvr2_vfs_page_map *pages) {
    const psvr2_firmware_profile *profile = vfs_profile(vfs);
    if (!profile) return false;
    uint64_t mapping = psvr2_krw_read_ptr(vfs->krw,
        inode + profile->inode_mapping_offset);
    if (!mapping) return false;
    uint32_t height = psvr2_krw_read_u32(vfs->krw, mapping + 0x08);
    uint64_t root = psvr2_krw_read_ptr(vfs->krw, mapping + 0x10);
    if (!root || height > 10) return false;
    return walk_radix(vfs, root, height, 0, max_index, pages);
}

bool psvr2_vfs_file_pages(psvr2_vfs *vfs, uint64_t inode,
                          psvr2_vfs_page_fn fn, void *opaque) {
    psvr2_vfs_page_map pages = {0};
    bool ok = collect_inode_pages(vfs, inode, UINT64_MAX, &pages);
    for (size_t i = 0; ok && i < pages.count; ++i)
        if (fn && !fn(
                opaque, pages.entries[i].index,
                pages.entries[i].address))
            break;
    psvr2_vfs_page_map_free(&pages);
    return ok;
}

static bool walk_radix(psvr2_vfs *vfs, uint64_t node, unsigned height,
                       uint64_t base_index, uint64_t max_index,
                       psvr2_vfs_page_map *pages) {
    const psvr2_firmware_profile *profile = vfs_profile(vfs);
    if (!profile || base_index > max_index) return profile != NULL;
    uint64_t real = node & ~UINT64_C(3);
    if (height == 0) {
        if (real >= profile->vmemmap_base &&
            real < profile->vmemmap_base + UINT64_C(0x100000000)) {
            uint64_t virt = page_to_virt(profile, real);
            if (virt)
                return psvr2_vfs_page_map_put(
                    pages, base_index, virt);
        }
        return true;
    }
    if (real < profile->page_offset_base ||
        real >= profile->kernel_va_limit)
        return true;
    uint8_t data[512];
    if (!psvr2_krw_read(
            vfs->krw, real + profile->radix_slots_offset,
                        data, sizeof(data))) return false;
    unsigned shift = (height - 1U) * 6U;
    for (unsigned i = 0; i < 64; ++i) {
        uint64_t child_index =
            base_index + ((uint64_t)i << shift);
        if (child_index > max_index) break;
        uint64_t slot = psvr2_load_le64(data + i * 8U);
        if (slot && !walk_radix(vfs, slot, height - 1U,
                                child_index, max_index, pages))
            return false;
    }
    return true;
}

static bool read_cached_file_page(psvr2_vfs *vfs, uint64_t address,
                                  uint8_t *output, size_t length) {
    const size_t prefix = PSVR2_PAGE_SIZE / PSVR2_DIRECT_READ_BLOCK_SIZE *
                          PSVR2_DIRECT_READ_BLOCK_SIZE;
    const size_t remainder = PSVR2_PAGE_SIZE - prefix;
    if (length != PSVR2_PAGE_SIZE || !prefix || !remainder)
        return psvr2_krw_read(vfs->krw, address, output, length);

    /* A full cached file page can use one overlapping final direct-read
     * block instead of many word-sized tail requests. Both reads remain
     * wholly inside this page; only its remaining bytes enter the output. */
    uint8_t tail[PSVR2_DIRECT_READ_BLOCK_SIZE];
    if (!psvr2_krw_read(vfs->krw, address, output, prefix) ||
        !psvr2_krw_read(vfs->krw,
            address + PSVR2_PAGE_SIZE - PSVR2_DIRECT_READ_BLOCK_SIZE,
            tail, sizeof(tail)))
        return false;
    memcpy(output + prefix, tail + sizeof(tail) - remainder, remainder);
    return true;
}

bool psvr2_vfs_read_file(psvr2_vfs *vfs, uint64_t dentry,
                         psvr2_buffer *out, size_t max_size) {
    const psvr2_firmware_profile *profile = vfs_profile(vfs);
    if (!profile) return false;
    uint64_t inode = psvr2_krw_read_ptr(
        vfs->krw, dentry + profile->dentry_inode_offset);
    psvr2_vfs_type type;
    uint64_t size;
    if (!inode || !psvr2_vfs_inode_info(vfs, dentry, &type, &size) ||
        type != PSVR2_VFS_FILE || size == 0 || size > max_size ||
        size > SIZE_MAX) return false;
    uint64_t count = (size + PSVR2_PAGE_SIZE - 1) / PSVR2_PAGE_SIZE;
    psvr2_vfs_page_map pages = {0};
    bool ok = collect_inode_pages(vfs, inode, count - 1U, &pages);
    for (uint64_t i = 0; ok && i < count; ++i) {
        uint64_t va = psvr2_vfs_page_map_get(&pages, i);
        size_t n = (size_t)psvr2_min_u64(
            (uint64_t)PSVR2_PAGE_SIZE,
            size - i * PSVR2_PAGE_SIZE);
        if (!va ||
            !psvr2_buffer_reserve(out, out->len + n) ||
            !read_cached_file_page(
                vfs, va, out->data + out->len, n)) {
            ok = false;
            break;
        }
        out->len += n;
    }
    psvr2_vfs_page_map_free(&pages);
    return ok;
}

const char *psvr2_vfs_type_name(psvr2_vfs_type type) {
    switch (type) {
    case PSVR2_VFS_DIRECTORY: return "DIR";
    case PSVR2_VFS_FILE: return "FILE";
    case PSVR2_VFS_SYMLINK: return "LNK";
    case PSVR2_VFS_CHAR: return "CHR";
    case PSVR2_VFS_BLOCK: return "BLK";
    case PSVR2_VFS_ERROR: return "ERR";
    default: return "UNK";
    }
}
