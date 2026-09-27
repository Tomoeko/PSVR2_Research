#ifndef PSVR2_CONSTANTS_H
#define PSVR2_CONSTANTS_H

#include "psvr2/common.h"

#define PSVR2_VID 0x054c
#define PSVR2_PID 0x0cde
#define PSVR2_WORK_CPU_UNBOUND 4U

#define PSVR2_UMH_TRIGGER_SIZE 56U
#define PSVR2_UMH_WORKER_SIZE 64U
#define PSVR2_ALLOC_HELPER_SIZE 64U
#define PSVR2_DIRECT_CONTEXT_SC_SIZE 16U
#define PSVR2_BATCH_PATCH_SC_SIZE 56U
#define PSVR2_DIRECT_READ_SC_SIZE 84U
#define PSVR2_COMPOSITE_EP0_TRANSFER_LIMIT 1024U
#define PSVR2_AUTH_RESPONSE_HEADER_SIZE 16U
#define PSVR2_DIRECT_READ_BLOCK_SIZE \
    (PSVR2_COMPOSITE_EP0_TRANSFER_LIMIT - \
     PSVR2_AUTH_RESPONSE_HEADER_SIZE)
#define PSVR2_PATCH_SC_SIZE 24U
#define PSVR2_TLBI_CLEANUP_SIZE 20U
#define PSVR2_COLD_CLEANUP_SIZE 32U

#define PSVR2_RUNTIME_WORKSPACE_SIZE UINT64_C(0x1000)
#define PSVR2_RUNTIME_DATA_OFFSET UINT64_C(0x20)
#define PSVR2_RUNTIME_DATA_CAPACITY UINT64_C(0x500)

#define PSVR2_MTU3_REQUEST_MEP_OFFSET UINT64_C(0x68)
#define PSVR2_MTU3_REQUEST_MTU_OFFSET UINT64_C(0x70)
#define PSVR2_MTU3_EP0_POINTER_OFFSET UINT64_C(0x78)
#define PSVR2_MTU3_ENDPOINT_STATE_OFFSET UINT64_C(0xcd)
#define PSVR2_GFP_ATOMIC UINT32_C(0x02080020)

#define PSVR2_FW_0110 UINT32_C(0x01100103)
#define PSVR2_FW_0600 UINT32_C(0x06000102)

#define PSVR2_PAGE_SIZE 4096U
#define PSVR2_STRUCT_PAGE_SIZE 64U
typedef struct {
    const char *artifact_path;
    const char *artifact_sha256;
    const char *provenance;
    uint64_t filp_open;
    uint64_t kernel_write;
    uint64_t filp_close;
    uint64_t clean_return;
    uint64_t modules_disabled;
    uint64_t kernel_bss_start;
    uint64_t kernel_bss_stop;
    uint64_t kernel_page_tables_start;
    uint64_t kernel_image_end;
    uint64_t kernel_probe_byte;
    uint64_t stack_cookie;
    uint64_t swapper_pg_dir;
    uint64_t init_task;
    uint64_t uevent_helper;
    uint64_t modprobe_path;
    uint64_t poweroff_cmd;
    uint64_t umh_disabled;
    uint64_t call_umh;
    uint64_t queue_work_on;
    uint64_t system_wq;
    uint64_t text_cave_start;
    uint64_t text_cave_end;
    uint64_t str_helper;
    uint64_t umh_trigger;
    uint64_t umh_worker;
    uint64_t alloc_helper;
    uint64_t free_helper;
    uint64_t workspace_slot;
    uint64_t batch_patch_helper;
    uint64_t direct_read_helper;
    uint64_t patch_helper;
    uint64_t tlbi_cleanup_helper;
    uint64_t cold_cleanup_helper;
    uint64_t cold_static_slot;
    uint64_t cold_static_stage;
    uint64_t cold_patch_epilogue;
    uint64_t cold_patch_loop;
    uint64_t cold_patch_exit;
    uint32_t cold_patch_exit_original;
    uint64_t patch_text;
    uint64_t tlbi_gadget;
    uint64_t mtu3_complete_resume;
    uint64_t mtu3_complete_epilogue;
    uint64_t mtu3_ep0_isr_epilogue;
    uint64_t raw_spin_lock;
    uint64_t kmalloc;
    uint64_t kfree;
    uint64_t emergency_restart;
    uint64_t machine_power_off;
    uint64_t module_list_head;
    uint64_t vmemmap_base;
    uint64_t page_offset_base;
    uint64_t kernel_va_limit;
    uint64_t phys_offset;
    uint64_t phys_mask;
    uint8_t user_va_bits;
    uint64_t kernel_map_start;
    uint64_t kernel_map_end;
    uint64_t bss_map_start;
    uint64_t bss_map_end;
    uint64_t previous_workspace;
    uint64_t gscwq_address;
    uint64_t gs_mmu_fault_data;
    uint64_t aes_switch_address;
    uint32_t module_list_offset;
    uint32_t module_name_offset;
    uint32_t module_core_base_offset;
    uint32_t module_core_size_offset;
    uint32_t module_core_text_size_offset;
    uint32_t module_core_ro_size_offset;
    uint32_t task_tasks_offset;
    uint32_t task_mm_offset;
    uint32_t task_comm_offset;
    uint32_t task_fs_offset;
    uint32_t task_nsproxy_offset;
    uint32_t mm_pgd_offset;
    uint32_t fs_root_offset;
    uint32_t ns_mnt_ns_offset;
    uint32_t mount_namespace_list_offset;
    uint32_t mount_list_offset;
    uint32_t mount_root_offset;
    uint32_t mount_point_offset;
    uint32_t dentry_subdirs_offset;
    uint32_t dentry_child_offset;
    uint32_t dentry_parent_offset;
    uint32_t dentry_name_offset;
    uint32_t dentry_inode_offset;
    uint32_t inode_mode_offset;
    uint32_t inode_size_offset;
    uint32_t inode_mapping_offset;
    uint32_t radix_slots_offset;
    uint64_t vrhmd_host_gate_offset;
    uint64_t vrhmd_patch_virtual_offset;
    /* Known-profile metadata; private image fixtures are not bundled.
     * Live certification is tracked separately in psvr2_constants. */
    bool injected_execution_certified;
    bool exact_image_baseline_present;
} psvr2_firmware_profile;

typedef psvr2_firmware_profile psvr2_firmware_config;

typedef struct {
    uint32_t version;
    bool firmware_forced;
    bool live_profile_certified;
    uint64_t certified_connection_generation;
    psvr2_firmware_config fw;
    uint64_t str_helper;
    uint64_t umh_trigger;
    uint64_t umh_worker;
    uint64_t runtime_work;
    uint64_t runtime_data;
} psvr2_constants;

bool psvr2_constants_set_firmware(psvr2_constants *c, uint32_t version,
                                  bool firmware_forced);
void psvr2_constants_invalidate_live_state(psvr2_constants *c);
bool psvr2_parse_firmware(const char *text, uint32_t *version);
const char *psvr2_firmware_name(uint32_t version);
bool psvr2_constants_shellcode_execution_safe(const psvr2_constants *c);
bool psvr2_constants_runtime_workspace_safe(const psvr2_constants *c);
bool psvr2_constants_kernel_probe_safe(const psvr2_constants *c);

#endif
