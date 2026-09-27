#include "psvr2/memory.h"
#include "kernel_list_internal.h"

#include <string.h>

static const psvr2_firmware_profile *profile_for(psvr2_krw *krw) {
    return krw && krw->ex && krw->ex->constants
               ? &krw->ex->constants->fw : NULL;
}

static bool direct_map_range_valid(
    const psvr2_firmware_profile *profile,
    uint64_t address, uint64_t length) {
    return profile && length &&
           profile->page_offset_base < profile->kernel_va_limit &&
           address >= profile->page_offset_base &&
           address < profile->kernel_va_limit &&
           length <= profile->kernel_va_limit - address;
}

static bool phys_to_virt(
    const psvr2_firmware_profile *profile, uint64_t physical,
    uint64_t *virtual_address) {
    if (!profile || !virtual_address ||
        physical < profile->phys_offset ||
        profile->page_offset_base >= profile->kernel_va_limit)
        return false;
    uint64_t offset = physical - profile->phys_offset;
    uint64_t capacity =
        profile->kernel_va_limit - profile->page_offset_base;
    if (offset >= capacity)
        return false;
    *virtual_address = profile->page_offset_base + offset;
    return true;
}

static bool table_entry_address(
    const psvr2_firmware_profile *profile, uint64_t descriptor,
    uint64_t index, uint64_t *address) {
    uint64_t table = 0;
    if ((descriptor & UINT64_C(3)) != UINT64_C(3) ||
        index >= UINT64_C(512) ||
        !phys_to_virt(
            profile, descriptor & profile->phys_mask, &table) ||
        !direct_map_range_valid(profile, table, PSVR2_PAGE_SIZE))
        return false;
    *address = table + index * sizeof(uint64_t);
    return true;
}

static bool translated_block_address(
    const psvr2_firmware_profile *profile, uint64_t descriptor,
    uint64_t va, uint64_t block_mask, uint64_t *address) {
    uint64_t reserved_output_bits =
        descriptor & profile->phys_mask & block_mask;
    uint64_t physical =
        (descriptor & profile->phys_mask & ~block_mask) |
        (va & block_mask);
    return !reserved_output_bits &&
           phys_to_virt(profile, physical, address) &&
           direct_map_range_valid(profile, *address, 1);
}

static bool user_address_valid(
    const psvr2_firmware_profile *profile, uint64_t va,
    uint64_t length) {
    if (!profile || !length || !profile->user_va_bits ||
        profile->user_va_bits >= 64)
        return false;
    uint64_t task_size = UINT64_C(1) << profile->user_va_bits;
    return va < task_size && length <= task_size - va;
}

static bool read_pte_value(psvr2_krw *krw, uint64_t address,
                           uint64_t *value, bool reinit, bool fatal) {
    uint8_t data[8];
    if (!psvr2_krw_read_ex(
            krw, address, data, sizeof(data), reinit, fatal))
        return false;
    *value = psvr2_load_le64(data);
    return true;
}

bool psvr2_get_pte_ex(psvr2_krw *krw, uint64_t va, psvr2_pte *out,
                      bool reinit, bool fatal) {
    const psvr2_firmware_profile *profile = profile_for(krw);
    if (!profile || !out || !profile->swapper_pg_dir ||
        !profile->phys_mask)
        return false;
    uint64_t pgd_addr =
        profile->swapper_pg_dir +
        ((va >> 30) & UINT64_C(0x1ff)) * 8;
    uint64_t pgd = 0;
    if (!read_pte_value(krw, pgd_addr, &pgd, reinit, fatal))
        return false;
    if (!(pgd & 1)) return false;
    if ((pgd & 3) == 1) {
        *out = (psvr2_pte){pgd_addr, pgd, PSVR2_PTE_L1_BLOCK};
        return true;
    }

    uint64_t pmd_addr = 0;
    if (!table_entry_address(
            profile, pgd, (va >> 21) & UINT64_C(0x1ff),
            &pmd_addr))
        return false;
    uint64_t pmd = 0;
    if (!read_pte_value(krw, pmd_addr, &pmd, reinit, fatal))
        return false;
    if (!(pmd & 1)) return false;
    if ((pmd & 3) == 1) {
        *out = (psvr2_pte){pmd_addr, pmd, PSVR2_PTE_L2_BLOCK};
        return true;
    }

    uint64_t pte_addr = 0;
    if (!table_entry_address(
            profile, pmd, (va >> 12) & UINT64_C(0x1ff),
            &pte_addr))
        return false;
    uint64_t pte = 0;
    if (!read_pte_value(krw, pte_addr, &pte, reinit, fatal))
        return false;
    if ((pte & UINT64_C(3)) != UINT64_C(3)) return false;
    *out = (psvr2_pte){pte_addr, pte, PSVR2_PTE_L3_PAGE};
    return true;
}

bool psvr2_get_pte(psvr2_krw *krw, uint64_t va, psvr2_pte *out) {
    return psvr2_get_pte_ex(krw, va, out, true, true);
}

uint64_t psvr2_user_ptwalk(psvr2_krw *krw, uint64_t pgd_base, uint64_t va) {
    const psvr2_firmware_profile *profile = profile_for(krw);
    if (!profile || !profile->phys_mask ||
        !user_address_valid(profile, va, 1) ||
        (pgd_base & (PSVR2_PAGE_SIZE - 1U)) != 0 ||
        !direct_map_range_valid(profile, pgd_base, PSVR2_PAGE_SIZE))
        return 0;
    uint64_t pgd = psvr2_krw_read_ptr(
        krw, pgd_base + ((va >> 30) & UINT64_C(0x1ff)) * 8);
    if (!(pgd & 1)) return 0;
    uint64_t translated = 0;
    if ((pgd & 3) == 1)
        return translated_block_address(
                   profile, pgd, va, UINT64_C(0x3fffffff),
                   &translated)
                   ? translated : 0;

    uint64_t pmd_address = 0;
    if (!table_entry_address(
            profile, pgd, (va >> 21) & UINT64_C(0x1ff),
            &pmd_address))
        return 0;
    uint64_t pmd = psvr2_krw_read_ptr(krw, pmd_address);
    if (!(pmd & 1)) return 0;
    if ((pmd & 3) == 1)
        return translated_block_address(
                   profile, pmd, va, UINT64_C(0x1fffff),
                   &translated)
                   ? translated : 0;

    uint64_t pte_address = 0;
    if (!table_entry_address(
            profile, pmd, (va >> 12) & UINT64_C(0x1ff),
            &pte_address))
        return 0;
    uint64_t pte = psvr2_krw_read_ptr(krw, pte_address);
    if ((pte & UINT64_C(3)) != UINT64_C(3) ||
        !phys_to_virt(
            profile,
            (pte & profile->phys_mask) | (va & UINT64_C(0xfff)),
            &translated) ||
        !direct_map_range_valid(profile, translated, 1))
        return 0;
    return translated;
}

bool psvr2_user_read(psvr2_krw *krw, uint64_t pgd, uint64_t va,
                     void *out, size_t length) {
    uint8_t *cursor = out;
    const psvr2_firmware_profile *profile = profile_for(krw);
    if (!profile || !pgd || !out || !length ||
        !user_address_valid(profile, va, (uint64_t)length) ||
        (pgd & (PSVR2_PAGE_SIZE - 1U)) != 0 ||
        !direct_map_range_valid(profile, pgd, PSVR2_PAGE_SIZE))
        return false;

    while (length) {
        size_t page_remaining =
            PSVR2_PAGE_SIZE - (size_t)(va & (PSVR2_PAGE_SIZE - 1U));
        size_t amount = psvr2_min_size(page_remaining, length);
        uint64_t kernel_address = psvr2_user_ptwalk(krw, pgd, va);
        if (!kernel_address ||
            !psvr2_krw_read(krw, kernel_address, cursor, amount))
            return false;
        va += amount;
        cursor += amount;
        length -= amount;
    }
    return true;
}

void psvr2_decode_pte(uint64_t pte, psvr2_pte_level level,
                      char permissions[4], char *detail, size_t detail_len) {
    unsigned ap = (unsigned)((pte >> 6) & 3);
    unsigned pxn = (unsigned)((pte >> 53) & 1);
    unsigned uxn = (unsigned)((pte >> 54) & 1);
    unsigned af = (unsigned)((pte >> 10) & 1);
    permissions[0] = 'r';
    permissions[1] = ap < 2 ? 'w' : '-';
    permissions[2] = pxn ? '-' : 'x';
    permissions[3] = '\0';
    const char *name = level == PSVR2_PTE_L1_BLOCK ? "L1_BLOCK" :
                       level == PSVR2_PTE_L2_BLOCK ? "L2_BLOCK" :
                       level == PSVR2_PTE_L3_PAGE ? "L3_PAGE" : "NONE";
    snprintf(detail, detail_len, "AP=%u PXN=%u UXN=%u AF=%u %s",
             ap, pxn, uxn, af, name);
}

typedef struct {
    const char *wanted;
    uint64_t found;
} module_find_context;

typedef struct {
    psvr2_krw *krw;
    const psvr2_firmware_profile *profile;
    psvr2_module_fn fn;
    void *opaque;
} module_walk_context;

static bool visit_module_node(void *opaque, uint64_t node) {
    module_walk_context *context = opaque;
    uint64_t base = node - context->profile->module_list_offset;
    char module[64] = "?";
    (void)psvr2_krw_read_string(
        context->krw,
        base + context->profile->module_name_offset,
        module, sizeof(module));
    return context->fn(context->opaque, base, module);
}

psvr2_walk_result psvr2_modules_foreach_ex(
    psvr2_krw *krw, psvr2_module_fn fn, void *opaque) {
    const psvr2_firmware_profile *profile = profile_for(krw);
    if (!profile || !fn || !profile->module_list_head ||
        !profile->module_list_offset || !profile->module_name_offset)
        return PSVR2_WALK_INVALID;
    module_walk_context context = {
        .krw = krw, .profile = profile, .fn = fn, .opaque = opaque
    };
    return psvr2_kernel_list_walk(
        krw, psvr2_krw_read_ptr(krw, profile->module_list_head),
        profile->module_list_head, false, 256,
        visit_module_node, &context);
}

static bool find_module_cb(
    void *opaque, uint64_t base, const char *module) {
    module_find_context *context = opaque;
    if (strcmp(module, context->wanted) == 0) {
        context->found = base;
        return false;
    }
    return true;
}

uint64_t psvr2_find_module(psvr2_krw *krw, const char *name) {
    const psvr2_firmware_profile *profile = profile_for(krw);
    if (!profile || !name || !profile->module_list_head ||
        !profile->module_list_offset || !profile->module_name_offset)
        return 0;
    module_find_context context = {
        .wanted = name
    };
    (void)psvr2_modules_foreach_ex(
        krw, find_module_cb, &context);
    return context.found;
}

bool psvr2_module_core_layout(
    psvr2_krw *krw, uint64_t module,
    psvr2_module_layout *layout) {
    const psvr2_firmware_profile *profile = profile_for(krw);
    if (!profile || !module || !layout ||
        profile->module_core_ro_size_offset <
            profile->module_core_base_offset)
        return false;
    size_t span =
        profile->module_core_ro_size_offset -
        profile->module_core_base_offset + 4U;
    if (span > 64) return false;

    uint8_t data[64];
    if (!psvr2_krw_read(
            krw, module + profile->module_core_base_offset,
            data, span))
        return false;

    psvr2_module_layout observed = {
        .base = psvr2_load_le64(data),
        .size = psvr2_load_le32(
            data + profile->module_core_size_offset -
            profile->module_core_base_offset),
        .text_size = psvr2_load_le32(
            data + profile->module_core_text_size_offset -
            profile->module_core_base_offset),
        .ro_size = psvr2_load_le32(
            data + profile->module_core_ro_size_offset -
            profile->module_core_base_offset)
    };
    if (!observed.base || !observed.size ||
        observed.text_size > observed.ro_size ||
        observed.ro_size >= observed.size)
        return false;
    *layout = observed;
    return true;
}
