/* Read synthetic inode/radix/page data through the actual VFS implementation. */
#include "psvr2/vfs.h"
#include "kernel_list_internal.h"

#include <assert.h>
#include <string.h>

#define TEST_DENTRY UINT64_C(0x1000)
#define TEST_INODE UINT64_C(0x2000)
#define TEST_MAPPING UINT64_C(0x3000)
#define TEST_RADIX UINT64_C(0x18000000)
#define TEST_PAGE_BASE UINT64_C(0x10000000)
#define TEST_VMEMMAP UINT64_C(0x90000000)
#define TEST_PAGES 4U

static uint64_t file_size;
static size_t read_count, fail_read;
static struct { uint64_t address; size_t length; } reads[TEST_PAGES * 2U];

static uint8_t fixture_byte(uint64_t offset) {
    return (uint8_t)((offset * 29U + (offset >> 5U) + 17U) & 255U);
}

uint64_t psvr2_krw_read_ptr(psvr2_krw *krw, uint64_t address) {
    (void)krw;
    if (address == TEST_DENTRY + 0x30U) return TEST_INODE;
    if (address == TEST_INODE + 0x40U) return TEST_MAPPING;
    assert(address == TEST_MAPPING + 0x10U);
    return TEST_RADIX;
}

uint32_t psvr2_krw_read_u32(psvr2_krw *krw, uint64_t address) {
    (void)krw;
    assert(address == TEST_MAPPING + 0x08U);
    return 1U;
}

bool psvr2_krw_read(psvr2_krw *krw, uint64_t address, void *out,
                    size_t length) {
    (void)krw;
    if (address == TEST_INODE) {
        assert(length == 0x40U);
        memset(out, 0, length);
        psvr2_store_le16(out, 0x8000U);
        psvr2_store_le64((uint8_t *)out + 0x10U, file_size);
        return true;
    }
    if (address == TEST_RADIX + 0x40U) {
        assert(length == 512U);
        memset(out, 0, length);
        uint64_t pages = (file_size + PSVR2_PAGE_SIZE - 1U) / PSVR2_PAGE_SIZE;
        assert(pages <= TEST_PAGES);
        for (uint64_t index = 0; index < pages; ++index)
            psvr2_store_le64((uint8_t *)out + (size_t)index * 8U,
                TEST_VMEMMAP + index * PSVR2_STRUCT_PAGE_SIZE);
        return true;
    }
    assert(address >= TEST_PAGE_BASE);
    assert(address < TEST_PAGE_BASE + TEST_PAGES * PSVR2_PAGE_SIZE);
    uint64_t offset = address - TEST_PAGE_BASE;
    /* Never cross a physical file-page boundary or the requested file end. */
    assert(offset % PSVR2_PAGE_SIZE + length <= PSVR2_PAGE_SIZE);
    assert(offset <= file_size && length <= file_size - offset);
    assert(read_count < PSVR2_ARRAY_LEN(reads));
    reads[read_count].address = address;
    reads[read_count].length = length;
    ++read_count;
    if (fail_read && read_count == fail_read) return false;
    for (size_t index = 0; index < length; ++index)
        ((uint8_t *)out)[index] = fixture_byte(offset + index);
    return true;
}

bool psvr2_krw_read_string(psvr2_krw *krw, uint64_t address,
                           char *out, size_t length) {
    (void)krw; (void)address; (void)out; (void)length;
    assert(0 && "cached-file reads must not traverse names");
    return false;
}

psvr2_walk_result psvr2_kernel_list_walk(psvr2_krw *krw, uint64_t first,
    uint64_t stop, bool visit_stop, size_t limit,
    psvr2_kernel_list_node_fn fn, void *opaque) {
    (void)krw; (void)first; (void)stop; (void)visit_stop;
    (void)limit; (void)fn; (void)opaque;
    assert(0 && "cached-file reads must not traverse linked lists");
    return PSVR2_WALK_INVALID;
}

static void check_file(psvr2_vfs *vfs, uint64_t size) {
    file_size = size;
    read_count = fail_read = 0;
    psvr2_buffer output = {0};
    /* Existing output is preserved while exact file bytes are appended. */
    assert(psvr2_buffer_append(&output, "HEAD", 4U));
    assert(psvr2_vfs_read_file(vfs, TEST_DENTRY, &output, (size_t)size));
    assert(output.len == size + 4U && !memcmp(output.data, "HEAD", 4U));
    for (uint64_t index = 0; index < size; ++index)
        assert(output.data[4U + (size_t)index] == fixture_byte(index));
    size_t full_pages = (size_t)(size / PSVR2_PAGE_SIZE);
    size_t partial = (size_t)(size % PSVR2_PAGE_SIZE);
    assert(read_count == full_pages * 2U + (partial ? 1U : 0U));
    const size_t prefix = PSVR2_PAGE_SIZE / PSVR2_DIRECT_READ_BLOCK_SIZE *
                          PSVR2_DIRECT_READ_BLOCK_SIZE;
    for (size_t page = 0; page < full_pages; ++page) {
        assert(reads[page * 2U].address == TEST_PAGE_BASE + page * PSVR2_PAGE_SIZE);
        assert(reads[page * 2U].length == prefix);
        assert(reads[page * 2U + 1U].address ==
            TEST_PAGE_BASE + (page + 1U) * PSVR2_PAGE_SIZE - PSVR2_DIRECT_READ_BLOCK_SIZE);
        assert(reads[page * 2U + 1U].length == PSVR2_DIRECT_READ_BLOCK_SIZE);
    }
    if (partial) {
        assert(reads[read_count - 1U].address ==
            TEST_PAGE_BASE + full_pages * PSVR2_PAGE_SIZE);
        assert(reads[read_count - 1U].length == partial);
    }
    psvr2_buffer_free(&output);
}

int main(void) {
    psvr2_constants constants = {.fw = {
        .dentry_inode_offset = 0x30U, .inode_mode_offset = 0U,
        .inode_size_offset = 0x10U, .inode_mapping_offset = 0x40U,
        .radix_slots_offset = 0x40U, .page_offset_base = TEST_PAGE_BASE,
        .vmemmap_base = TEST_VMEMMAP, .kernel_va_limit = UINT64_C(0x20000000)
    }};
    psvr2_exploit exploit = {.constants = &constants};
    psvr2_krw krw = {.ex = &exploit};
    psvr2_vfs vfs = {.krw = &krw};
    check_file(&vfs, PSVR2_PAGE_SIZE);
    check_file(&vfs, PSVR2_PAGE_SIZE * 3U);
    check_file(&vfs, PSVR2_PAGE_SIZE + 137U);
    check_file(&vfs, 137U);
    for (size_t failure = 1U; failure <= 2U; ++failure) {
        file_size = PSVR2_PAGE_SIZE;
        read_count = 0;
        fail_read = failure;
        psvr2_buffer output = {0};
        assert(psvr2_buffer_append(&output, "HEAD", 4U));
        assert(!psvr2_vfs_read_file(&vfs, TEST_DENTRY, &output, PSVR2_PAGE_SIZE));
        assert(read_count == failure && output.len == 4U);
        assert(!memcmp(output.data, "HEAD", 4U));
        psvr2_buffer_free(&output);
    }
    puts("same-page cached-file read regressions passed");
    return 0;
}
