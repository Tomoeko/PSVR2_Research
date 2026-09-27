#include "kernel_bootstrap_internal.h"

#include "psvr2/arm64.h"

#include <string.h>

bool psvr2_build_temporary_patch_helper(
    const psvr2_constants *constants, uint64_t address,
    uint64_t mep, uint64_t pte_address,
    uint8_t pte_byte, uint8_t endpoint_state,
    uint8_t out[PSVR2_TEMP_HELPER_SIZE]) {
    if (!constants || !out || (address & UINT64_C(3)) != 0 ||
        !mep || !pte_address)
        return false;

    uint32_t instruction;
    memset(out, 0, PSVR2_TEMP_HELPER_SIZE);

    /* Patch entry: patch one text instruction, then restore USB state. */
    psvr2_store_le32(out + 0, UINT32_C(0x2a1303e1)); /* mov w1,w19 */
    psvr2_store_le32(out + 4, UINT32_C(0xaa1403e0)); /* mov x0,x20 */
    if (!psvr2_arm64_branch(
            address + 8, constants->fw.patch_text, true, &instruction))
        return false;
    psvr2_store_le32(out + 8, instruction);
    if (!psvr2_arm64_branch(
            address + 12, address + 16, false, &instruction))
        return false;
    psvr2_store_le32(out + 12, instruction);
    psvr2_store_le32(out + 16, UINT32_C(0xaa1503e0)); /* mov x0,x21 */
    if (!psvr2_arm64_branch(
            address + 20, constants->fw.raw_spin_lock, true, &instruction))
        return false;
    psvr2_store_le32(out + 20, instruction);
    psvr2_store_le32(
        out + 24,
        UINT32_C(0x52800001) |
            ((uint32_t)endpoint_state << 5)); /* mov w1,#state */
    if (!psvr2_arm64_ldr_literal(
            0, address + 28, address + 120, &instruction))
        return false;
    psvr2_store_le32(out + 28, instruction);
    psvr2_store_le32(
        out + 32, UINT32_C(0x39033401)); /* strb w1,[x0,#0xcd] */
    if (!psvr2_arm64_branch(
            address + 36, constants->fw.mtu3_ep0_isr_epilogue,
            false, &instruction))
        return false;
    psvr2_store_le32(out + 36, instruction);

    /*
     * Finalizer entry: restore USB state, create a private TLBI frame,
     * restore PXN, and return through the untouched ISR frame. No temporary
     * instruction is fetched after the final ISB makes PXN effective.
     */
    psvr2_store_le32(out + 40, UINT32_C(0xaa1503e0)); /* mov x0,x21 */
    if (!psvr2_arm64_branch(
            address + 44, constants->fw.raw_spin_lock, true, &instruction))
        return false;
    psvr2_store_le32(out + 44, instruction);
    psvr2_store_le32(
        out + 48,
        UINT32_C(0x52800001) |
            ((uint32_t)endpoint_state << 5)); /* mov w1,#state */
    psvr2_store_le32(
        out + 52, UINT32_C(0x39033681)); /* strb w1,[x20,#0xcd] */
    psvr2_store_le32(out + 56, UINT32_C(0xd100c3ff)); /* sub sp,sp,#0x30 */
    psvr2_store_le32(out + 60, UINT32_C(0xa9007fff)); /* stp xzr,xzr,[sp] */
    if (!psvr2_arm64_ldr_literal(
            0, address + 64, address + 104, &instruction))
        return false;
    psvr2_store_le32(out + 64, instruction);
    psvr2_store_le32(out + 68, UINT32_C(0xf90007e0)); /* str x0,[sp,#8] */
    psvr2_store_le32(
        out + 72, UINT32_C(0xa9017fff)); /* stp xzr,xzr,[sp,#16] */
    psvr2_store_le32(out + 76, UINT32_C(0xf90013ff)); /* str xzr,[sp,#32] */
    psvr2_store_le32(out + 80, UINT32_C(0xf90017ff)); /* str xzr,[sp,#40] */
    if (!psvr2_arm64_ldr_literal(
            0, address + 84, address + 112, &instruction))
        return false;
    psvr2_store_le32(out + 84, instruction);
    psvr2_store_le32(
        out + 88, UINT32_C(0x52800001) | ((uint32_t)pte_byte << 5));
    psvr2_store_le32(out + 92, UINT32_C(0x39000001)); /* strb w1,[x0] */
    if (!psvr2_arm64_branch(
            address + 96, constants->fw.tlbi_gadget, false, &instruction))
        return false;
    psvr2_store_le32(out + 96, instruction);
    psvr2_store_le32(out + 100, UINT32_C(0xd503201f)); /* nop */
    psvr2_store_le64(
        out + 104, constants->fw.mtu3_ep0_isr_epilogue);
    psvr2_store_le64(out + 112, pte_address);
    psvr2_store_le64(out + 120, mep);
    return true;
}
