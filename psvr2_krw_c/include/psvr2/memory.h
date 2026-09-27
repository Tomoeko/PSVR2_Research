#ifndef PSVR2_MEMORY_H
#define PSVR2_MEMORY_H

#include "psvr2/runtime.h"
#include "psvr2/tasks.h"

typedef enum {
    PSVR2_PTE_NONE,
    PSVR2_PTE_L1_BLOCK,
    PSVR2_PTE_L2_BLOCK,
    PSVR2_PTE_L3_PAGE
} psvr2_pte_level;

typedef struct {
    uint64_t address;
    uint64_t value;
    psvr2_pte_level level;
} psvr2_pte;

typedef struct {
    uint64_t base;
    uint32_t size;
    uint32_t text_size;
    uint32_t ro_size;
} psvr2_module_layout;

typedef bool (*psvr2_module_fn)(
    void *opaque, uint64_t module, const char *name);

bool psvr2_get_pte(psvr2_krw *krw, uint64_t va, psvr2_pte *out);
bool psvr2_get_pte_ex(psvr2_krw *krw, uint64_t va, psvr2_pte *out,
                      bool reinit, bool fatal);
uint64_t psvr2_user_ptwalk(psvr2_krw *krw, uint64_t pgd, uint64_t va);
bool psvr2_user_read(psvr2_krw *krw, uint64_t pgd, uint64_t va,
                     void *out, size_t length);
void psvr2_decode_pte(uint64_t pte, psvr2_pte_level level,
                      char permissions[4], char *detail, size_t detail_len);
uint64_t psvr2_find_module(psvr2_krw *krw, const char *name);
psvr2_walk_result psvr2_modules_foreach_ex(
    psvr2_krw *krw, psvr2_module_fn fn, void *opaque);
bool psvr2_module_core_layout(
    psvr2_krw *krw, uint64_t module,
    psvr2_module_layout *layout);

#endif
