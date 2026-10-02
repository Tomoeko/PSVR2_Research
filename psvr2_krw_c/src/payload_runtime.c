#include "psvr2/kernel_payload.h"

#include "psvr2/arm64.h"

#include <string.h>

bool psvr2_build_patch_helper(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_PATCH_SC_SIZE]) {
    if (!constants || !out) return false;

    memset(out, 0, PSVR2_PATCH_SC_SIZE);
    uint32_t instruction;
    psvr2_store_le32(out + 0, UINT32_C(0x2a1303e1)); /* mov w1,w19 */
    psvr2_store_le32(out + 4, UINT32_C(0xaa1403e0)); /* mov x0,x20 */
    if (!psvr2_arm64_branch(
            constants->fw.patch_helper + 8,
            constants->fw.patch_text, true, &instruction))
        return false;
    psvr2_store_le32(out + 8, instruction);
    psvr2_store_le32(
        out + 12, UINT32_C(0xf9403eb4)); /* ldr x20,[x21,#0x78] */
    psvr2_store_le32(out + 16, UINT32_C(0xd503201f)); /* nop */
    if (!psvr2_arm64_branch(
            constants->fw.patch_helper + 20,
            constants->fw.clean_return, false, &instruction))
        return false;
    psvr2_store_le32(out + 20, instruction);
    return true;
}

bool psvr2_build_batch_patch_helper(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_BATCH_PATCH_SC_SIZE]) {
    if (!constants || !out) return false;

    memset(out, 0, PSVR2_BATCH_PATCH_SC_SIZE);
    uint32_t instruction;
    const uint32_t words[] = {
        UINT32_C(0xb9400261), /* loop: ldr w1,[x19] */
        UINT32_C(0x91001273), /* add x19,x19,#4 */
        UINT32_C(0xaa1403e0), /* mov x0,x20 */
        0,
        UINT32_C(0x91001294), /* add x20,x20,#4 */
        UINT32_C(0xd10402d6), /* sub x22,x22,#0x100 */
        UINT32_C(0xd348fec0), /* lsr x0,x22,#8 */
        UINT32_C(0xb5ffff20), /* cbnz x0,loop */
        UINT32_C(0xf9403eb4), /* done: ldr x20,[x21,#0x78] */
        UINT32_C(0xd503201f),
        0,
        UINT32_C(0xd503201f),
        UINT32_C(0xd503201f),
        UINT32_C(0xd503201f)
    };
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(words); ++index)
        psvr2_store_le32(out + index * 4U, words[index]);
    if (!psvr2_arm64_branch(
            constants->fw.batch_patch_helper + 12,
            constants->fw.patch_text, true, &instruction))
        return false;
    psvr2_store_le32(out + 12, instruction);
    if (!psvr2_arm64_branch(
            constants->fw.batch_patch_helper + 40,
            constants->fw.clean_return, false, &instruction))
        return false;
    psvr2_store_le32(out + 40, instruction);
    return true;
}

bool psvr2_build_workspace_allocator(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_ALLOC_HELPER_SIZE]) {
    if (!constants || !out) return false;

    memset(out, 0, PSVR2_ALLOC_HELPER_SIZE);
    psvr2_store_le32(out + 0, UINT32_C(0xaa1303e0)); /* mov x0,x19 */
    psvr2_store_le32(out + 4, UINT32_C(0x52800401)); /* mov w1,#0x20 */
    psvr2_store_le32(
        out + 8,
        UINT32_C(0x72a04101)); /* movk w1,#0x208,lsl #16 */

    uint32_t instruction;
    if (!psvr2_arm64_branch(
            constants->fw.alloc_helper + 12,
            constants->fw.kmalloc, true, &instruction))
        return false;
    psvr2_store_le32(out + 12, instruction);
    psvr2_store_le32(out + 16, UINT32_C(0xaa0003f3)); /* mov x19,x0 */
    if (!psvr2_arm64_ldr_literal(
            20, constants->fw.alloc_helper + 20,
            constants->fw.alloc_helper + 56,
            &instruction))
        return false;
    psvr2_store_le32(out + 20, instruction);
    psvr2_store_le32(out + 24, UINT32_C(0xaa1403e0)); /* mov x0,x20 */
    psvr2_store_le32(out + 28, UINT32_C(0x2a1303e1)); /* mov w1,w19 */
    if (!psvr2_arm64_branch(
            constants->fw.alloc_helper + 32,
            constants->fw.patch_text, true, &instruction))
        return false;
    psvr2_store_le32(out + 32, instruction);
    psvr2_store_le32(
        out + 36, UINT32_C(0x91001280)); /* add x0,x20,#4 */
    psvr2_store_le32(
        out + 40, UINT32_C(0xd360fe61)); /* lsr x1,x19,#32 */
    if (!psvr2_arm64_branch(
            constants->fw.alloc_helper + 44,
            constants->fw.patch_text, true, &instruction))
        return false;
    psvr2_store_le32(out + 44, instruction);
    psvr2_store_le32(
        out + 48, UINT32_C(0xf9403eb4)); /* ldr x20,[x21,#0x78] */
    if (!psvr2_arm64_branch(
            constants->fw.alloc_helper + 52,
            constants->fw.clean_return, false, &instruction))
        return false;
    psvr2_store_le32(out + 52, instruction);
    psvr2_store_le64(out + 56, constants->fw.workspace_slot);
    return true;
}

bool psvr2_build_umh_worker(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_UMH_WORKER_SIZE]) {
    if (!constants || !out) return false;

    memset(out, 0, PSVR2_UMH_WORKER_SIZE);
    uint32_t literal;
    uint32_t call;
    if (!psvr2_arm64_ldr_literal(
            19, constants->umh_worker + 0x0c,
            constants->umh_worker + 0x38, &literal) ||
        !psvr2_arm64_branch(
            constants->umh_worker + 0x20,
            constants->fw.call_umh, true, &call))
        return false;
    const uint32_t words[] = {
        UINT32_C(0xa9be7bfd), UINT32_C(0xa90153f3),
        UINT32_C(0x910003fd), literal,
        UINT32_C(0xaa1303e0), UINT32_C(0x91040261),
        UINT32_C(0xd2800002), /* mov x2,#0: envp */
        UINT32_C(0xd2800043), /* mov x3,#2: UMH_WAIT_PROC */
        call, UINT32_C(0xf9007a60),
        UINT32_C(0xa94153f3), UINT32_C(0xa8c27bfd),
        UINT32_C(0xd65f03c0), UINT32_C(0xd503201f)
    };
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(words); ++index)
        psvr2_store_le32(out + index * 4U, words[index]);
    psvr2_store_le64(out + 56, constants->runtime_data);
    return true;
}

bool psvr2_build_umh_trigger(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_UMH_TRIGGER_SIZE]) {
    if (!constants || !out) return false;

    memset(out, 0, PSVR2_UMH_TRIGGER_SIZE);
    uint32_t load_work;
    uint32_t load_queue;
    uint32_t queue_call;
    uint32_t clean_branch;
    if (!psvr2_arm64_ldr_literal(
            2, constants->umh_trigger,
            constants->umh_trigger + 0x28, &load_work) ||
        !psvr2_arm64_ldr_literal(
            1, constants->umh_trigger + 4,
            constants->umh_trigger + 0x30, &load_queue) ||
        !psvr2_arm64_branch(
            constants->umh_trigger + 0x10,
            constants->fw.queue_work_on, true, &queue_call) ||
        !psvr2_arm64_branch(
            constants->umh_trigger + 0x1c,
            constants->fw.clean_return, false, &clean_branch))
        return false;

    const uint32_t words[] = {
        load_work, load_queue,
        UINT32_C(0xf9400021), UINT32_C(0x52800080),
        queue_call, UINT32_C(0xf9403eb4),
        UINT32_C(0xd503201f), clean_branch,
        UINT32_C(0xd503201f), UINT32_C(0xd503201f)
    };
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(words); ++index)
        psvr2_store_le32(out + index * 4U, words[index]);
    psvr2_store_le64(out + 40, constants->runtime_work);
    psvr2_store_le64(out + 48, constants->fw.system_wq);
    return true;
}

bool psvr2_build_str_helper(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_STR_HELPER_SIZE]) {
    if (!constants || !out) return false;

    uint32_t instruction;
    psvr2_store_le32(out + 0, UINT32_C(0xf9000293)); /* str x19,[x20] */
    psvr2_store_le32(
        out + 4, UINT32_C(0xf9403eb4)); /* ldr x20,[x21,#0x78]: mep */
    psvr2_store_le32(out + 8, UINT32_C(0xd503201f)); /* nop */
    if (!psvr2_arm64_branch(
            constants->fw.str_helper + 12, constants->fw.clean_return,
            false, &instruction))
        return false;
    psvr2_store_le32(out + 12, instruction);
    return true;
}

bool psvr2_build_tlbi_cleanup(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_TLBI_CLEANUP_SIZE], uint8_t endpoint_state) {
    if (!constants || !out) return false;
    (void)endpoint_state;

    uint32_t instruction;
    psvr2_store_le32(
        out + 0, UINT32_C(0xf9403eb4)); /* ldr x20,[x21,#0x78] */
    psvr2_store_le32(out + 4, UINT32_C(0xaa1503e0)); /* mov x0,x21 */
    if (!psvr2_arm64_branch(
            constants->fw.tlbi_cleanup_helper + 8,
            constants->fw.raw_spin_lock, true, &instruction))
        return false;
    psvr2_store_le32(out + 8, instruction);
    psvr2_store_le32(
        out + 12, UINT32_C(0x3903369f)); /* strb wzr,[x20,#0xcd]: busy=0 */
    if (!psvr2_arm64_branch(
            constants->fw.tlbi_cleanup_helper + 16,
            constants->fw.mtu3_ep0_isr_epilogue, false, &instruction))
        return false;
    psvr2_store_le32(out + 16, instruction);
    return true;
}
