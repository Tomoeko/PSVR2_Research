#include "../open_vrhmd.h"

static void *map_register_page(int fd_mem, uint64_t address, int protection) {
  if (address & (sizeof(uint32_t) - 1)) {
    errno = EINVAL;
    ERR("unaligned register address 0x%llx", (unsigned long long)address);
    return MAP_FAILED;
  }
  uint64_t page = address & ~((uint64_t)PAGE_SIZE_4K - 1);
  void *mapped = mmap(NULL, PAGE_SIZE_4K, protection, MAP_SHARED, fd_mem, (off_t)page);
  if (mapped == MAP_FAILED) ERR("mmap register 0x%llx", (unsigned long long)address);
  return mapped;
}

uint32_t devmem_read32(int fd_mem, uint64_t address) {
  void *page = map_register_page(fd_mem, address, PROT_READ);
  if (page == MAP_FAILED) return 0xDEAD;
  size_t offset = address & (PAGE_SIZE_4K - 1);
  uint32_t value = *(volatile uint32_t *)((uint8_t *)page + offset);
  munmap(page, PAGE_SIZE_4K);
  return value;
}

void devmem_write32(int fd_mem, uint64_t address, uint32_t value) {
  void *page = map_register_page(fd_mem, address, PROT_READ | PROT_WRITE);
  if (page == MAP_FAILED) return;
  size_t offset = address & (PAGE_SIZE_4K - 1);
  *(volatile uint32_t *)((uint8_t *)page + offset) = value;
  munmap(page, PAGE_SIZE_4K);
}
