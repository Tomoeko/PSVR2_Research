#ifndef OPEN_VRHMD_DISPLAY_ABI_H
#define OPEN_VRHMD_DISPLAY_ABI_H

#include <stddef.h>
#include <stdint.h>

#define DSI_INIT 0x4401 /* _IO('D', 1) */

#define DSI_SET_CONFIG_DDDS 0x400C4402 /* _IOW('D', 2, 12bytes) */
#define DSI_ENABLE_DDDS 0x400C4403     /* _IOW('D', 3, 12bytes) */
#define DSI_DISABLE_DDDS 0x4404        /* _IO('D', 4) */
#define DSI_SET_SW_MUTE 0x40014406     /* _IOW('D', 6, bool) */
/* DSI_START_OUTPUT changed between firmware versions:
 *   v01.10: _IOW('D', 7, MTK_WRAPPER_DSI_WAIT)    = 0x40044407 (4-byte enum)
 *   v06.00: _IOW('D', 7, args_dsi_start_param)     = 0x40084407 (8-byte struct)
 * The build selects the exact ioctl for verified source families. */
#define DSI_START_OUTPUT_V0110 0x40044407  /* _IOW('D', 7, uint32_t) — v01.10 */
#define DSI_START_OUTPUT_V0600 0x40084407  /* _IOW('D', 7, {wait,mask}) — v06.00+ */
#define DSI_STOP_OUTPUT 0x4408         /* _IO('D', 8) */
#define DSI_RESET 0x440A               /* _IO('D', 10) */
#define DSI_SET_PANEL_PARAM 0x4024440C /* _IOW('D', 12, 36bytes) */
#define DSI_SET_LCM_PARAM                                                      \
  0x4010440D                        /* _IOW('D', 13, args_dsi_set_lcm_param)   \
                                     */
#define DSI_SET_DATARATE 0x40044410 /* _IOW('D', 16, uint32_t) */
#define DSI_SEND_VM_COMMAND                                                    \
  0x40124411                     /* _IOW('D', 17, args_dsi_send_vm_command) */
#define DSI_FINIT 0x4417         /* _IO('D', 23) */
#define DSI_LANE_SWAP 0x40014418 /* _IOW('D', 24, uint8_t) */

/* MMSYS ioctls — fd = /dev/mmsys, IOC type 'M' = 0x4D */
#define MMSYS_DSI_LANE_SWAP 0x40104D09 /* _IOW('M', 9, 16bytes) */

/* DSC ioctls — fd = /dev/dscenc, IOC type 'E' = 0x45 */
#define DSC_POWER_ON 0x4501 /* _IO('E', 1) */

/* ION ioctls */
#define ION_IOC_ALLOC_NR 0xC0204900 /* _IOWR('I', 0, 32-byte struct) */
#define ION_IOC_FREE_NR 0xC0044901 /* _IOWR('I', 1, handle) */
#define ION_IOC_SYNC_NR 0xC0084907 /* _IOWR('I', 7, ion_fd_data) */
#define ION_IOC_SHARE_NR 0xC0084904 /* _IOWR('I', 4, ion_fd_data) */

/* Base Addresses */
#define DSI0_BASE 0x14020000
#define DSI1_BASE 0x14021000
#define MIPI_TX0_BASE 0x11d50000
#define MMSYSCFG_BASE 0x14000000
#define PAGE_SIZE_4K 4096
#define DSC_INIT_HW 0x40284502 /* _IOW('E', 2, args_dsc_init_hw) = 40 bytes */
#define DSC_START 0x4503       /* _IO('E', 3) */
#define DSC_RESET 0x4504       /* _IO('E', 4) */
#define DSC_RELAY 0x4505       /* _IO('E', 5) */

/* MMSYS connect ioctls — fd = /dev/mmsys, IOC type 'M' = 0x4D */
#define MMSYS_DISCONNECT_COMP 0x40084D02 /* _IOW('M', 2, {cur,next}) */
#define MMSYS_CONNECT_COMP 0x40084D01    /* _IOW('M', 1, {cur,next}) */
#define MMSYS_CAMERA_SYNC_SEL 0x40014D04 /* _IOW('M', 4, bool) */

#define MUTEX_GET_RESOURCE 0x40044D01   /* _IOW('M', 1, enum) */
#define MUTEX_PUT_RESOURCE 0x40044D02   /* _IOW('M', 2, enum) */
#define MUTEX_SET_CONFIG 0x40144D03     /* _IOW('M', 3, args_mutex_set_config) */
#define MUTEX_ADD_COMPONENT 0x40084D04  /* _IOW('M', 4, args_mutex_add_component) */
#define MUTEX_REMOVE_COMPONENT 0x40084D05 /* _IOW('M', 5, args_mutex_remove_component) */
#define MUTEX_ENABLE 0x40084D06         /* _IOW('M', 6, args_mutex_enable) */
#define MUTEX_POWER_ON 0x40044D07       /* _IOW('M', 7, enum) */
#define MUTEX_POWER_OFF 0x40044D08      /* _IOW('M', 8, enum) */

/* RDMA ioctls — fd = /dev/rdma0, IOC type 'R' = 0x52 */
#define RDMA_POWER_ON 0x5201         /* _IO('R', 1) */
#define RDMA_POWER_OFF 0x5202        /* _IO('R', 2) */
#define RDMA_START 0x5204            /* _IO('R', 4) */
#define RDMA_STOP 0x5205             /* _IO('R', 5) */
#define RDMA_CONFIG_FRAME 0x40285206 /* _IOW('R', 6, args_rdma_set_config) */
#define RDMA_LARB_GET 0x40015208     /* _IOW('R', 8, bool) */
#define RDMA_LARB_PUT 0x40015209     /* _IOW('R', 9, bool) */
#define RDMA_TYPE_SEL 0x4001520B     /* _IOW('R', 11, uint8_t) */

/* Slicer ioctls — fd = /dev/slicer, IOC type 'S' = 0x53 */
#define SLICER_POWER_ON 0x5301 /* _IO('S', 1) */
#define SLICER_START 0x5303    /* _IO('S', 3) */
#define SLICER_SET_CONFIG                                                      \
  0x40785306 /* _IOW('S', 6, args_slicer_set_config)                           \
              */

/* LHC ioctls — fd = /dev/lhc, IOC type 'L' = 0x4C */
#define LHC_POWER_ON 0x4C01  /* _IO('L', 1) */
#define LHC_POWER_OFF 0x4C02 /* _IO('L', 2) */
#define LHC_STOP 0x4C05      /* _IO('L', 5) */

/* DPRX ioctls — fd = /dev/dprx, IOC type 0x44 */
#define DPRX_POWER_ON_V0110 0x80014401 /* _IOR('D', 1, bool); scalar arg */
#define DPRX_POWER_ON_V0600 0x80084401 /* _IOR('D', 1, 8-byte config) */
#define DPRX_POWER_OFF 0x4402        /* _IO(0x44, 2) */
#define DPRX_PHY_GCE_DEINIT 0x440B   /* _IO(0x44, 11) — v06.00 only */

/* DSI hblank update — MUST be called after dsiSetPanelParam */
#define DSI_UPDATE_HBLANKING 0x400C4414 /* _IOW('D', 20, 12 bytes) */
struct dsi_hblank_args {
  uint32_t hsync_len;
  uint32_t hback_porch;
  uint32_t hfront_porch;
};

/* DISPLAY_COMPONENT enum values (from mtk_wrapper_common.h) */
#define COMP_DP 0x00010000
#define COMP_SLICER_VID 0x00020000
#define COMP_SLICER_DSC 0x00020001
#define COMP_MDP_RDMA 0x00040000
#define COMP_PVRIC_RDMA 0x00040001
#define COMP_DISP_RDMA 0x00040002
#define COMP_MUTEX 0x00050000
#define COMP_DSI 0x00060000
#define COMP_DSC 0x00070000
#define COMP_LHC 0x000B0000

/* Pipeline structs */
struct mmsys_connect_args {
  uint32_t cur;
  uint32_t next;
};

struct mutex_set_config_args {
  uint32_t idx;                 /* MTK_WRAPPER_MUTEX_DISP = 8 */
  uint32_t sof_component;       /* MTK_WRAPPER_MUTEX_SOF_DSI = 1 */
  uint32_t sof_timer_component; /* COMP_DSI = 0x60000 */
  uint32_t src_time;
  uint32_t ref_time;
};

struct mutex_add_component_args {
  uint32_t idx;       /* 8 */
  uint32_t component; /* DISPLAY_COMPONENT */
};

struct mutex_enable_args {
  uint32_t idx;    /* 8 */
  uint32_t enable; /* 0xB0001 */
};

/* ION structs — must match kernel sizeof = 32 bytes on aarch64 */
struct ion_allocation_data {
  uint64_t len;          /* 8 bytes (size_t on aarch64) */
  uint64_t align;        /* 8 bytes (size_t on aarch64) */
  uint32_t heap_id_mask; /* 4 bytes */
  uint32_t flags;        /* 4 bytes */
  int32_t handle;        /* 4 bytes — output */
  int32_t _pad;          /* 4 bytes — tail padding to 32 */
};                       /* total = 32 bytes = 0x20, matches ioctl 0xC0204900 */

struct ion_fd_data {
  int32_t handle;
  int32_t fd;
};

struct ion_handle_data {
  int32_t handle;
};

/* RDMA config struct (40 bytes on aarch64) */
struct rdma_set_config {
  uint32_t format;  /* 3 = RGB */
  int32_t ionfd[3]; /* dma_buf fds */
  int32_t buffer_num;
  uint32_t pitch;
  uint32_t width;
  uint32_t height;
  uint32_t pvric_header_offset;
  uint8_t rbfc_setting;
  uint8_t use_sram;
};

/* Slicer config struct (0x78 = 120 bytes) */
struct slicer_set_config {
  uint32_t inp_sel; /* 0=video, 1=dsc, 2=video_dsc */
  /* video sub-struct: 60 bytes */
  int32_t vid_width;
  int32_t vid_height;
  uint8_t vid_in_hsync_inv;
  uint8_t vid_in_vsync_inv;
  uint8_t _pad_vid[2];
  int32_t vid_sop[4];
  int32_t vid_eop[4];
  int32_t vid_en[4];
  /* dsc sub-struct: 48 bytes */
  int32_t dsc_width;
  int32_t dsc_height;
  uint8_t dsc_in_hsync_inv;
  uint8_t dsc_in_vsync_inv;
  uint8_t _pad_dsc[2];
  int32_t dsc_sop[4];
  int32_t dsc_eop[4];
  uint8_t dsc_bit_rate;
  uint8_t dsc_chunk_num;
  uint8_t dsc_valid_byte;
  uint8_t _pad_dsc2;
  /* gce sub-struct: 8 bytes */
  int32_t gce_width;
  int32_t gce_height;
};

/* DSC encoder config struct (dscenc_config = 40 bytes on aarch64) */
struct dsc_init_config {
  uint32_t disp_pic_w;     /* display width */
  uint32_t disp_pic_h;     /* display height */
  uint32_t slice_h;        /* slice height */
  uint32_t inout_sel;      /* 0=1in1out, 1=2in1out, 2=2in2out */
  uint32_t version;        /* 0=DSC_V1.1, 1=DSC_V1.2 */
  uint32_t ich_line_clear; /* 0=auto, 1=at_line, 2=at_slice */
  uint32_t format;         /* MTK_WRAPPER_DSI_FORMAT (4=COMP_24_8) */
  uint64_t pps;            /* pointer to u32[19] PPS, or NULL for auto */
};

/* Panel command tables. */
#define REGFLAG_DELAY 0xFFFC

struct lcm_setting_entry {
  unsigned int cmd;
  unsigned char count;
  unsigned char para_list[128];
};

/* args_dsi_set_lcm_param for ioctl */
struct lcm_param_ioctl {
  const struct lcm_setting_entry *table;
  int table_num;
  int is_vm_mode;
};

/* args_dsi_send_vm_command */
struct vm_cmd_ioctl {
  uint8_t cmd;
  uint8_t count;
  uint8_t para_list[16];
};

/* ═══════════════════════════════════════════════════════════════
 *  Display parameters — verified from vrhmd binary
 *  Panel info struct at index 3 (splash screen, DSC_8BPP)
 *  Address: 0x15E658 in vrhmd
 * ═══════════════════════════════════════════════════════════════ */
#define DSI_DATA_RATE_KHZ 907252
#define DISPLAY_WIDTH 2000 /* full display width for RDMA/DSC */
#define DSI_HACTIVE 1000   /* per DSI partition (panel param) */
#define DSI_VACTIVE 2040
#define DSI_VSYNC_LEN 1
#define DSI_VBACK_PORCH 151
#define DSI_VFRONT_PORCH 8
#define DSI_HSYNC_LEN 22
#define DSI_HBACK_PORCH 38
#define DSI_HFRONT_PORCH 970
#define DSI_FORMAT                                                             \
  4 /* MTK_WRAPPER_DSI_FORMAT_COMPRESSION_24_8 (matches vrhmd) */

/* Panel param struct (from args_dsi_set_panel_param, 9 x u32 = 36 bytes) */
struct panel_param {
  uint32_t hactive;
  uint32_t vactive;
  uint32_t vsync_len;
  uint32_t vback_porch;
  uint32_t vfront_porch;
  uint32_t hsync_len;
  uint32_t hback_porch;
  uint32_t hfront_porch;
  uint32_t format;
};

/* DDDS config struct (from args_dsi_set_config_ddds, 12 bytes) */
struct ddds_config {
  uint32_t target_freq;
  uint32_t max_freq;
  uint8_t xtal;
  uint8_t pad[3]; /* alignment */
};

/* MMSYS DSI lane swap config (4 x u32 = 16 bytes) */
struct mmsys_dsi_lane_swap {
  uint32_t field0;
  uint32_t field1;
  uint32_t field2;
  uint32_t field3;
};


struct dprx_config {
  uint8_t hdcp2;
  uint8_t pad[3];
  uint32_t edid_type;
};

struct dsi_start_param {
  uint32_t wait;
  uint32_t mask;
};

#if defined(PSVR2_SOURCE_FAMILY_0600)
#define OPEN_VRHMD_FIRMWARE "06.00"
#define DSI_START_OUTPUT DSI_START_OUTPUT_V0600
#define DPRX_POWER_ON DPRX_POWER_ON_V0600
#elif defined(PSVR2_SOURCE_FAMILY_0110)
#define OPEN_VRHMD_FIRMWARE "01.10"
#define DSI_START_OUTPUT DSI_START_OUTPUT_V0110
#define DPRX_POWER_ON DPRX_POWER_ON_V0110
#else
#define OPEN_VRHMD_FIRMWARE "unverified (runtime ABI fallback)"
#endif

/* Fail compilation if a userspace payload drifts from the kernel ABI. */
#define ABI_SIZE(type, bytes) _Static_assert(sizeof(struct type) == (bytes), #type " size")
ABI_SIZE(dprx_config, 8);
ABI_SIZE(dsi_start_param, 8);
ABI_SIZE(dsi_hblank_args, 12);
ABI_SIZE(mmsys_connect_args, 8);
ABI_SIZE(mutex_set_config_args, 20);
ABI_SIZE(mutex_add_component_args, 8);
ABI_SIZE(mutex_enable_args, 8);
ABI_SIZE(ion_allocation_data, 32);
ABI_SIZE(ion_fd_data, 8);
ABI_SIZE(ion_handle_data, 4);
ABI_SIZE(rdma_set_config, 40);
ABI_SIZE(slicer_set_config, 120);
ABI_SIZE(dsc_init_config, 40);
ABI_SIZE(lcm_setting_entry, 136);
ABI_SIZE(lcm_param_ioctl, 16);
ABI_SIZE(vm_cmd_ioctl, 18);
ABI_SIZE(panel_param, 36);
ABI_SIZE(ddds_config, 12);
ABI_SIZE(mmsys_dsi_lane_swap, 16);
#undef ABI_SIZE
_Static_assert(offsetof(struct dsc_init_config, pps) == 32, "DSC PPS offset");
_Static_assert(offsetof(struct rdma_set_config, rbfc_setting) == 36, "RDMA flags offset");
_Static_assert(offsetof(struct slicer_set_config, dsc_width) == 64, "slicer DSC offset");

#endif
