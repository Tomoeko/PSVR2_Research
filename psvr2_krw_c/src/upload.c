#include "psvr2/shellcode.h"

#include <stdlib.h>
#include <string.h>

enum {
    PSVR2_UPLOAD_PAGE_LIMIT = 16384
};

typedef struct {
    uint64_t pages[PSVR2_UPLOAD_PAGE_LIMIT];
} upload_pages;

static bool collect_upload_page(
    void *opaque, uint64_t index, uint64_t virt) {
    upload_pages *pages = opaque;
    if (index >= PSVR2_ARRAY_LEN(pages->pages)) return false;
    pages->pages[index] = virt;
    return true;
}

static bool upload_pages_ready(
    const upload_pages *pages, size_t needed) {
    if (needed > PSVR2_ARRAY_LEN(pages->pages))
        return false;
    for (size_t index = 0; index < needed; ++index)
        if (!pages->pages[index])
            return false;
    return true;
}

static bool wait_for_upload_pages(
    psvr2_runtime *runtime, const char *remote_name, size_t needed,
    psvr2_vfs *vfs, uint64_t *inode, upload_pages *pages) {
    const psvr2_firmware_profile *profile =
        &runtime->krw->ex->constants->fw;
    uint64_t tmp = 0;
    double deadline = psvr2_now() + 5.0;
    do {
        if (!tmp)
            tmp = psvr2_vfs_find_mount(vfs, "tmp");
        uint64_t dentry =
            tmp
                ? psvr2_vfs_resolve_child(vfs, tmp, remote_name)
                : 0;
        uint64_t candidate =
            dentry
                ? psvr2_krw_read_ptr(
                      runtime->krw,
                      dentry + profile->dentry_inode_offset)
                : 0;
        upload_pages observed = {0};
        if (candidate &&
            psvr2_vfs_file_pages(
                vfs, candidate, collect_upload_page, &observed) &&
            upload_pages_ready(&observed, needed)) {
            *inode = candidate;
            *pages = observed;
            return true;
        }
    } while (psvr2_now() < deadline);
    return false;
}

bool psvr2_upload_file(psvr2_runtime *runtime, const char *local_path,
                       const char *remote_name) {
    if (!runtime || !local_path) return false;
    const psvr2_firmware_profile *profile =
        &runtime->krw->ex->constants->fw;
    psvr2_buffer data = {0};
    if (!psvr2_read_file(local_path, &data) || !data.len) {
        fprintf(stderr, "[-] Could not read %s\n", local_path);
        psvr2_buffer_free(&data);
        return false;
    }
    if (!remote_name || !*remote_name)
        remote_name = psvr2_basename(local_path);

    char remote[512], previous[560], command[768];
    snprintf(remote, sizeof(remote), "/tmp/%s", remote_name);
    /*
     * Renaming forces creation of a fresh inode/page-cache mapping. Reuse one
     * backup name so iterative uploads do not accumulate trash_NNNN files.
     */
    snprintf(previous, sizeof(previous), "%s.previous", remote);
    snprintf(command, sizeof(command), "mv '%s' '%s'", remote, previous);
    int64_t result;
    (void)psvr2_exec(runtime, command, false, &result, NULL);

    size_t iterations = (data.len + 50) / 51;
    char padding[51];
    memset(padding, 'A', sizeof(padding) - 1);
    padding[sizeof(padding) - 1] = '\0';
    snprintf(command, sizeof(command),
             "x=0;>'%s';while [ $x -lt %zu ];do echo '%s'>>'%s';"
             "x=$((x+1));done",
             remote, iterations, padding, remote);
    printf("[*] Uploading %s (%zu bytes) -> %s\n",
           remote_name, data.len, remote);
    if (!psvr2_exec(runtime, command, false, &result, NULL) || result != 0) {
        psvr2_buffer_free(&data);
        return false;
    }

    size_t needed =
        (data.len + PSVR2_PAGE_SIZE - 1) / PSVR2_PAGE_SIZE;
    psvr2_vfs vfs;
    psvr2_vfs_init(&vfs, runtime->krw);
    uint64_t inode = 0;
    upload_pages pages = {0};
    if (!wait_for_upload_pages(
            runtime, remote_name, needed, &vfs, &inode, &pages)) {
        psvr2_buffer_free(&data);
        return false;
    }

    double start = psvr2_now();
    size_t writes = 0;
    for (size_t offset = 0; offset < data.len; offset += 8) {
        uint8_t word[8] = {0};
        size_t length = psvr2_min_size(8, data.len - offset);
        memcpy(word, data.data + offset, length);
        size_t page = offset / PSVR2_PAGE_SIZE;
        size_t in_page = offset % PSVR2_PAGE_SIZE;
        if (!psvr2_krw_write_u64_fast(
                runtime->krw, pages.pages[page] + in_page,
                psvr2_load_le64(word))) {
            psvr2_buffer_free(&data);
            return false;
        }
        if (++writes % 512 == 0)
            psvr2_device_check_keep_alive(runtime->krw->ex->usb);
    }

    if (!psvr2_krw_write_u64_fast(
            runtime->krw,
            inode + profile->inode_size_offset, data.len)) {
        psvr2_buffer_free(&data);
        return false;
    }
    uint8_t inode_head[8];
    if (psvr2_krw_read(
            runtime->krw, inode, inode_head, sizeof(inode_head))) {
        uint64_t value =
            (psvr2_load_le64(inode_head) & ~UINT64_C(0xffff)) | 0100755U;
        (void)psvr2_krw_write_u64_fast(runtime->krw, inode, value);
    }
    printf("[+] Upload complete: %.0f bytes/sec\n",
           data.len / psvr2_max_double(0.001, psvr2_now() - start));
    psvr2_buffer_free(&data);
    return true;
}
