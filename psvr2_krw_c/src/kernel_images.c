#include "kernel_images_internal.h"

static const uint8_t tlbi_gadget[] = {
    0x9f, 0x3a, 0x03, 0xd5,
    0x1f, 0x83, 0x08, 0xd5,
    0x9f, 0x3b, 0x03, 0xd5,
    0xdf, 0x3f, 0x03, 0xd5
};

static const uint8_t completion_resume_0600[] = {
    0xe0, 0x03, 0x15, 0xaa,
    0xcc, 0xa3, 0x01, 0x94,
    0x96, 0x36, 0x03, 0x39
};

static const uint8_t completion_resume_0110[] = {
    0xe0, 0x03, 0x15, 0xaa,
    0xca, 0xa3, 0x01, 0x94,
    0x96, 0x36, 0x03, 0x39
};

static const uint8_t completion_epilogue[] = {
    0xf3, 0x53, 0x41, 0xa9
};

static const uint8_t clean_return[] = {
    0xfd, 0x7b, 0xc1, 0xa8,
    0xc0, 0x03, 0x5f, 0xd6
};

static const uint8_t isr_epilogue[] = {
    0xf3, 0x53, 0x41, 0xa9,
    0xf5, 0x5b, 0x42, 0xa9,
    0xf7, 0x63, 0x43, 0xa9
};

void psvr2_kernel_execution_anchors(
    const psvr2_constants *constants,
    psvr2_kernel_anchor out[PSVR2_KERNEL_ANCHOR_COUNT]) {
    const uint8_t *resume =
        constants->version == PSVR2_FW_0110
            ? completion_resume_0110
            : completion_resume_0600;
    out[0] = (psvr2_kernel_anchor){
        "TLBI gadget", constants->fw.tlbi_gadget,
        tlbi_gadget, sizeof(tlbi_gadget)
    };
    out[1] = (psvr2_kernel_anchor){
        "mtu3 resume", constants->fw.mtu3_complete_resume,
        resume, sizeof(completion_resume_0600)
    };
    out[2] = (psvr2_kernel_anchor){
        "mtu3 epilogue", constants->fw.mtu3_complete_epilogue,
        completion_epilogue, sizeof(completion_epilogue)
    };
    out[3] = (psvr2_kernel_anchor){
        "clean return", constants->fw.clean_return,
        clean_return, sizeof(clean_return)
    };
    out[4] = (psvr2_kernel_anchor){
        "ISR epilogue", constants->fw.mtu3_ep0_isr_epilogue,
        isr_epilogue, sizeof(isr_epilogue)
    };
}
