#include "psvr2/constants.h"
#include "firmware_0110_internal.h"
#include "firmware_0600_internal.h"

#include <string.h>

static const psvr2_firmware_config fw_0600 = {
    .artifact_path = "06.00/kernel.bin",
    .artifact_sha256 =
        "5eea9d6da46efc8230183baf70023bef675f8dd09145d575b48f88a6337ca347",
    .provenance =
        "06.00 kernel image baseline; non-symbol operational "
        "offsets need independent validation",
    .filp_open = UINT64_C(0xffffffc000120420),
    .kernel_write = UINT64_C(0xffffffc000146d4c),
    .filp_close = UINT64_C(0xffffffc00011effc),
    .clean_return = UINT64_C(0xffffffc000305778),
    .modules_disabled = UINT64_C(0xffffffc000656150),
    .kernel_bss_start = PSVR2_KERNEL_BSS_START,
    .kernel_bss_stop = PSVR2_KERNEL_BSS_STOP,
    .kernel_page_tables_start = PSVR2_KERNEL_PAGE_TABLES_START,
    .kernel_image_end = PSVR2_KERNEL_IMAGE_END,
    .kernel_probe_byte = PSVR2_KERNEL_PROBE_BYTE,
    .stack_cookie = PSVR2_STACK_COOKIE,
    .swapper_pg_dir = PSVR2_SWAPPER_PG_DIR,
    .init_task = PSVR2_INIT_TASK,
    .uevent_helper = PSVR2_UEVENT_HELPER,
    .modprobe_path = PSVR2_MODPROBE_PATH,
    .poweroff_cmd = PSVR2_POWEROFF_CMD,
    .umh_disabled = PSVR2_UMH_DISABLED,
    .call_umh = PSVR2_CALL_UMH,
    .queue_work_on = PSVR2_QUEUE_WORK_ON,
    .system_wq = PSVR2_SYSTEM_WQ,
    .text_cave_start = PSVR2_TEXT_CAVE_START,
    .text_cave_end = PSVR2_TEXT_CAVE_END,
    .str_helper = PSVR2_STR_SC,
    .umh_trigger = PSVR2_EXEC_SC,
    .umh_worker = PSVR2_WORK_SC,
    .alloc_helper = PSVR2_ALLOC_SC,
    .free_helper = PSVR2_FREE_SC,
    .workspace_slot = PSVR2_WORKSPACE_SLOT,
    .batch_patch_helper = PSVR2_BATCH_PATCH_SC,
    .direct_read_helper = PSVR2_DIRECT_READ_SC,
    .patch_helper = PSVR2_PATCH_SC,
    .tlbi_cleanup_helper = PSVR2_TLBI_CLEANUP_SC,
    .cold_cleanup_helper = PSVR2_COLD_CLEANUP_SC,
    .cold_static_slot = PSVR2_COLD_STATIC_SLOT,
    .cold_static_stage = PSVR2_COLD_STATIC_STAGE,
    .cold_patch_epilogue = PSVR2_COLD_PATCH_EPILOGUE,
    .cold_patch_loop = PSVR2_COLD_PATCH_LOOP,
    .cold_patch_exit = PSVR2_COLD_PATCH_EXIT,
    .cold_patch_exit_original = PSVR2_COLD_PATCH_EXIT_ORIGINAL,
    .patch_text = PSVR2_PATCH_TEXT,
    .tlbi_gadget = PSVR2_TLBI_GADGET,
    .mtu3_complete_resume = PSVR2_MTU3_COMPLETE_RESUME,
    .mtu3_complete_epilogue = PSVR2_MTU3_COMPLETE_EPILOGUE,
    .mtu3_ep0_isr_epilogue = PSVR2_MTU3_EP0_ISR_EPILOGUE,
    .raw_spin_lock = PSVR2_RAW_SPIN_LOCK,
    .kmalloc = PSVR2_KMALLOC,
    .kfree = PSVR2_KFREE,
    .emergency_restart = PSVR2_EMERGENCY_RESTART,
    .machine_power_off = PSVR2_MACHINE_POWER_OFF,
    .module_list_head = PSVR2_MOD_LIST_HEAD,
    .vmemmap_base = PSVR2_VMEMMAP_BASE,
    .page_offset_base = PSVR2_PAGE_OFFSET_BASE,
    .kernel_va_limit = PSVR2_KERNEL_VA_LIMIT,
    .phys_offset = PSVR2_PHYS_OFFSET,
    .phys_mask = PSVR2_PHYS_MASK,
    .user_va_bits = 39,
    .kernel_map_start = PSVR2_KERNEL_MAP_START,
    .kernel_map_end = PSVR2_KERNEL_MAP_END,
    .bss_map_start = PSVR2_BSS_MAP_START,
    .bss_map_end = PSVR2_BSS_MAP_END,
    .previous_workspace = PSVR2_PREVIOUS_WORKSPACE,
    .gscwq_address = PSVR2_GSCWQ_ADDRESS,
    .gs_mmu_fault_data = PSVR2_GS_MMU_FAULT_DATA,
    .aes_switch_address = PSVR2_AES_SWITCH_ADDRESS,
    .module_list_offset = PSVR2_MOD_OFF_LIST,
    .module_name_offset = PSVR2_MOD_OFF_NAME,
    .module_core_base_offset = PSVR2_MOD_OFF_CORE_BASE,
    .module_core_size_offset = PSVR2_MOD_OFF_CORE_SIZE,
    .module_core_text_size_offset = PSVR2_MOD_OFF_CORE_TEXT_SIZE,
    .module_core_ro_size_offset = PSVR2_MOD_OFF_CORE_RO_SIZE,
    .task_tasks_offset = PSVR2_OFF_TASK_TASKS,
    .task_mm_offset = PSVR2_OFF_TASK_MM,
    .task_comm_offset = PSVR2_OFF_TASK_COMM,
    .task_fs_offset = PSVR2_OFF_TASK_FS,
    .task_nsproxy_offset = PSVR2_OFF_TASK_NSPROXY,
    .mm_pgd_offset = PSVR2_OFF_MM_PGD,
    .fs_root_offset = PSVR2_OFF_FS_ROOT,
    .ns_mnt_ns_offset = PSVR2_OFF_NS_MNT_NS,
    .mount_namespace_list_offset = PSVR2_OFF_MNT_NS_LIST,
    .mount_list_offset = PSVR2_OFF_MNT_LIST,
    .mount_root_offset = PSVR2_OFF_MNT_ROOT,
    .mount_point_offset = PSVR2_OFF_MNT_POINT,
    .dentry_subdirs_offset = PSVR2_OFF_D_SUBDIRS,
    .dentry_child_offset = PSVR2_OFF_D_CHILD,
    .dentry_parent_offset = PSVR2_OFF_D_PARENT,
    .dentry_name_offset = PSVR2_OFF_D_NAME,
    .dentry_inode_offset = PSVR2_OFF_D_INODE,
    .inode_mode_offset = PSVR2_OFF_I_MODE,
    .inode_size_offset = PSVR2_OFF_I_SIZE,
    .inode_mapping_offset = PSVR2_OFF_I_MAPPING,
    .radix_slots_offset = PSVR2_OFF_RADIX_SLOTS,
    .vrhmd_host_gate_offset = PSVR2_VRHMD_HOST_GATE_OFFSET,
    .vrhmd_patch_virtual_offset = 0,
    /*
     * The exact-image bootstrap uses the 120-byte mode-2 report and leaves
     * the mtu3_ep0_isr frame untouched.  Its cleanup helper restores the
     * endpoint lock and busy byte before entering the ISR epilogue.
     */
    .injected_execution_certified = true,
    .exact_image_baseline_present = true
};

void psvr2_constants_invalidate_live_state(psvr2_constants *c) {
    if (!c) return;
    c->live_profile_certified = false;
    c->certified_connection_generation = 0;
    c->runtime_work = 0;
    c->runtime_data = 0;
}

bool psvr2_constants_set_firmware(psvr2_constants *c, uint32_t version,
                                  bool firmware_forced) {
    if (!c) return false;
    memset(c, 0, sizeof(*c));
    c->fw = fw_0600;
    if (version == PSVR2_FW_0110) {
        c->fw.artifact_path =
            "01.10/kernel.bin";
        c->fw.artifact_sha256 =
            "a6d3cfcbf7e1f490aa043464c14230e927ad9c4ca54d81df8f927ee48ce88773";
        c->fw.provenance =
            "complete live 01.10 kernel dump plus device-extracted "
            "sieusb command-table baseline";
        c->fw.filp_open = PSVR2_0110_FILP_OPEN;
        c->fw.kernel_write = PSVR2_0110_KERNEL_WRITE;
        c->fw.filp_close = PSVR2_0110_FILP_CLOSE;
        c->fw.umh_disabled = PSVR2_0110_UMH_DISABLED;
        c->fw.call_umh = PSVR2_0110_CALL_UMH;
        c->fw.queue_work_on = PSVR2_0110_QUEUE_WORK_ON;
        c->fw.system_wq = PSVR2_0110_SYSTEM_WQ;
        c->fw.clean_return = PSVR2_0110_CLEAN_RETURN;
        c->fw.init_task = PSVR2_0110_INIT_TASK;
        c->fw.text_cave_start = PSVR2_0110_TEXT_CAVE_START;
        c->fw.text_cave_end = PSVR2_0110_TEXT_CAVE_END;
        c->fw.str_helper = PSVR2_0110_STR_SC;
        c->fw.umh_trigger = PSVR2_0110_EXEC_SC;
        c->fw.umh_worker = PSVR2_0110_WORK_SC;
        c->fw.alloc_helper = PSVR2_0110_ALLOC_SC;
        c->fw.free_helper = PSVR2_0110_FREE_SC;
        c->fw.workspace_slot = PSVR2_0110_WORKSPACE_SLOT;
        c->fw.batch_patch_helper = PSVR2_0110_BATCH_PATCH_SC;
        c->fw.direct_read_helper = PSVR2_0110_DIRECT_READ_SC;
        c->fw.patch_helper = PSVR2_0110_PATCH_SC;
        c->fw.tlbi_cleanup_helper = PSVR2_0110_TLBI_CLEANUP_SC;
        c->fw.cold_cleanup_helper = PSVR2_0110_COLD_CLEANUP_SC;
        c->fw.cold_static_slot = PSVR2_0110_COLD_STATIC_SLOT;
        c->fw.cold_static_stage = PSVR2_0110_COLD_STATIC_STAGE;
        c->fw.tlbi_gadget = PSVR2_0110_TLBI_GADGET;
        c->fw.mtu3_complete_resume =
            PSVR2_0110_MTU3_COMPLETE_RESUME;
        c->fw.mtu3_complete_epilogue =
            PSVR2_0110_MTU3_COMPLETE_EPILOGUE;
        c->fw.mtu3_ep0_isr_epilogue =
            PSVR2_0110_MTU3_EP0_ISR_EPILOGUE;
        c->fw.raw_spin_lock = PSVR2_0110_RAW_SPIN_LOCK;
        c->fw.kmalloc = PSVR2_0110_KMALLOC;
        c->fw.kfree = PSVR2_0110_KFREE;
        c->fw.module_list_head = PSVR2_0110_MODULE_LIST_HEAD;
        c->fw.emergency_restart = PSVR2_0110_EMERGENCY_RESTART;
        c->fw.machine_power_off = PSVR2_0110_MACHINE_POWER_OFF;
        c->fw.aes_switch_address = PSVR2_0110_AES_SWITCH_ADDRESS;
        c->fw.task_tasks_offset = PSVR2_0110_OFF_TASK_TASKS;
        c->fw.task_mm_offset = PSVR2_0110_OFF_TASK_MM;
        c->fw.task_comm_offset = PSVR2_0110_OFF_TASK_COMM;
        c->fw.task_fs_offset = PSVR2_0110_OFF_TASK_FS;
        c->fw.task_nsproxy_offset = PSVR2_0110_OFF_TASK_NSPROXY;
        /* The full live dump validates the shared UMH/workqueue globals and
         * the exact 01.10 allocator, cleanup, and endpoint-return anchors. */
        c->fw.injected_execution_certified = true;
        c->fw.exact_image_baseline_present = true;
    } else if (version != PSVR2_FW_0600) {
        memset(c, 0, sizeof(*c));
        return false;
    }
    c->version = version;
    c->firmware_forced = firmware_forced;
    /* Profile selection is provenance, not evidence from the live kernel. */
    psvr2_constants_invalidate_live_state(c);
    c->str_helper = c->fw.str_helper;
    c->umh_trigger = c->fw.umh_trigger;
    c->umh_worker = c->fw.umh_worker;
    return true;
}

bool psvr2_parse_firmware(const char *text, uint32_t *version) {
    if (!text || !version) return false;
    if (!strcmp(text, "01.10")) { *version = PSVR2_FW_0110; return true; }
    if (!strcmp(text, "06.00")) { *version = PSVR2_FW_0600; return true; }
    uint64_t value;
    if (!psvr2_parse_u64(text, &value) ||
        (value != PSVR2_FW_0110 && value != PSVR2_FW_0600))
        return false;
    *version = (uint32_t)value;
    return true;
}

const char *psvr2_firmware_name(uint32_t version) {
    if (version == PSVR2_FW_0110) return "01.10";
    if (version == PSVR2_FW_0600) return "06.00";
    return "unknown";
}

bool psvr2_constants_shellcode_execution_safe(const psvr2_constants *c) {
    return c && c->fw.injected_execution_certified &&
           psvr2_constants_kernel_probe_safe(c) &&
           c->str_helper == c->fw.str_helper &&
           c->umh_trigger == c->fw.umh_trigger &&
           c->umh_worker == c->fw.umh_worker &&
           c->fw.call_umh && c->fw.queue_work_on && c->fw.system_wq &&
           c->fw.umh_disabled && c->fw.patch_text &&
           c->fw.raw_spin_lock && c->fw.kmalloc && c->fw.kfree &&
           c->fw.text_cave_start <= c->str_helper &&
           c->umh_worker + PSVR2_UMH_WORKER_SIZE <=
               c->fw.text_cave_end;
}

bool psvr2_constants_runtime_workspace_safe(const psvr2_constants *c) {
    if (!psvr2_constants_shellcode_execution_safe(c) ||
        c->runtime_work < c->fw.kernel_image_end ||
        (c->runtime_work & UINT64_C(0xf)) != 0 ||
        c->runtime_work >
            UINT64_MAX -
                (PSVR2_RUNTIME_DATA_OFFSET +
                 PSVR2_RUNTIME_DATA_CAPACITY) ||
        c->runtime_data !=
            c->runtime_work + PSVR2_RUNTIME_DATA_OFFSET)
        return false;
    return (c->runtime_work & ~UINT64_C(0xfff)) ==
           ((c->runtime_data +
             PSVR2_RUNTIME_DATA_CAPACITY - UINT64_C(1)) &
            ~UINT64_C(0xfff));
}

bool psvr2_constants_kernel_probe_safe(const psvr2_constants *c) {
    return c && c->live_profile_certified &&
           (c->version == PSVR2_FW_0110 ||
            c->version == PSVR2_FW_0600) &&
           c->fw.exact_image_baseline_present &&
           c->fw.artifact_path && c->fw.artifact_sha256 &&
           c->fw.kernel_bss_start ==
               PSVR2_KERNEL_BSS_START &&
           c->fw.kernel_bss_stop ==
               PSVR2_KERNEL_BSS_STOP &&
           c->fw.kernel_page_tables_start ==
               PSVR2_KERNEL_PAGE_TABLES_START &&
           c->fw.kernel_image_end ==
               PSVR2_KERNEL_IMAGE_END &&
           c->fw.kernel_probe_byte ==
               PSVR2_KERNEL_PROBE_BYTE;
}
