#ifndef PSVR2_FIRMWARE_0110_INTERNAL_H
#define PSVR2_FIRMWARE_0110_INTERNAL_H

/*
 * Firmware 01.10 profile.  The command-table layout comes from the
 * matching device-extracted 01.10 sieusb module. Its
 * usb_cmd_init clears 0x21000 bytes at .bss+0x68 and initializes 256
 * 0x210-byte records.  The loader places .bss at module_core+0x9180.
 */
#define PSVR2_0110_SIEUSB_BASE UINT64_C(0xffffffbffc0a6000)
#define PSVR2_0110_SIEUSB_COMMAND_SLOTS \
    (PSVR2_0110_SIEUSB_BASE + UINT64_C(0x9180) + UINT64_C(0x68))
#define PSVR2_0110_COLD_STATIC_SLOT UINT64_C(0x10)
#define PSVR2_0110_COLD_STATIC_STAGE \
    (PSVR2_0110_SIEUSB_COMMAND_SLOTS + \
     PSVR2_0110_COLD_STATIC_SLOT * UINT64_C(0x210))

/* 01.10 kernel symbols and structure layout from the matching source tree. */
#define PSVR2_0110_FILP_OPEN UINT64_C(0xffffffc0001203d4)
#define PSVR2_0110_KERNEL_WRITE UINT64_C(0xffffffc000146d00)
#define PSVR2_0110_FILP_CLOSE UINT64_C(0xffffffc00011efb0)
#define PSVR2_0110_UMH_DISABLED UINT64_C(0xffffffc0005a4750)
#define PSVR2_0110_CALL_UMH UINT64_C(0xffffffc0000a0f40)
#define PSVR2_0110_QUEUE_WORK_ON UINT64_C(0xffffffc0000a2c1c)
#define PSVR2_0110_SYSTEM_WQ UINT64_C(0xffffffc00059e870)
#define PSVR2_0110_KMALLOC UINT64_C(0xffffffc00011ba84)
#define PSVR2_0110_KFREE UINT64_C(0xffffffc00011cc7c)
#define PSVR2_0110_MODULE_LIST_HEAD UINT64_C(0xffffffc0005a83f0)
#define PSVR2_0110_EMERGENCY_RESTART UINT64_C(0xffffffc0000aa468)
#define PSVR2_0110_MACHINE_POWER_OFF UINT64_C(0xffffffc000085330)
#define PSVR2_0110_CLEAN_RETURN UINT64_C(0xffffffc000305750)
#define PSVR2_0110_INIT_TASK UINT64_C(0xffffffc00059f380)
/*
 * Confirmed independently by the live 01.10 kallsyms table and two data
 * references in the exact kernel dump's MT3612 MSDC driver.  This differs
 * from the 06.00 global by 0x10 bytes.
 */
#define PSVR2_0110_AES_SWITCH_ADDRESS UINT64_C(0xffffffc000668a00)

/*
 * The matching source objects prove that 01.10 mtu3.o is 0x14 bytes larger
 * and that each relevant in-object site is +0x14.  The independently known
 * gadget-core clean epilogue is -0x28 in the 01.10 image, placing mtu3.o at
 * -0x3c and therefore each live mtu3 site at a net -0x28 from 06.00.
 */
#define PSVR2_0110_MTU3_COMPLETE_RESUME UINT64_C(0xffffffc00030465c)
#define PSVR2_0110_MTU3_COMPLETE_EPILOGUE UINT64_C(0xffffffc000304668)
#define PSVR2_0110_MTU3_EP0_ISR_EPILOGUE UINT64_C(0xffffffc000303c3c)
#define PSVR2_0110_TLBI_GADGET UINT64_C(0xffffffc000112ba8)

/*
 * The paired links place the final text/rodata boundary at the same page and
 * show identical tail contents.  The shipping zero cave is therefore stable.
 * The _raw_spin_lock entry below is taken from the live 01.10 kernel dump;
 * 0xffffffc00036d570 is the preceding function's restore epilogue, not the
 * lock entry.
 */
#define PSVR2_0110_TEXT_CAVE_START UINT64_C(0xffffffc00036edf0)
#define PSVR2_0110_TEXT_CAVE_END UINT64_C(0xffffffc00036f000)
#define PSVR2_0110_STR_SC UINT64_C(0xffffffc00036edf0)
#define PSVR2_0110_EXEC_SC UINT64_C(0xffffffc00036ee00)
#define PSVR2_0110_WORK_SC UINT64_C(0xffffffc00036ee40)
#define PSVR2_0110_ALLOC_SC UINT64_C(0xffffffc00036ee80)
#define PSVR2_0110_FREE_SC UINT64_C(0xffffffc00036eec0)
#define PSVR2_0110_WORKSPACE_SLOT UINT64_C(0xffffffc00036eee0)
#define PSVR2_0110_BATCH_PATCH_SC UINT64_C(0xffffffc00036eef0)
#define PSVR2_0110_DIRECT_READ_SC UINT64_C(0xffffffc00036ef28)
#define PSVR2_0110_PATCH_SC UINT64_C(0xffffffc00036ef80)
#define PSVR2_0110_TLBI_CLEANUP_SC UINT64_C(0xffffffc00036efa0)
#define PSVR2_0110_COLD_CLEANUP_SC UINT64_C(0xffffffc00036efc0)
#define PSVR2_0110_RAW_SPIN_LOCK UINT64_C(0xffffffc00036d588)

#define PSVR2_0110_OFF_TASK_TASKS 0x3a8U
#define PSVR2_0110_OFF_TASK_MM 0x3f8U
#define PSVR2_0110_OFF_TASK_COMM 0x658U
#define PSVR2_0110_OFF_TASK_FS 0x690U
#define PSVR2_0110_OFF_TASK_NSPROXY 0x6a0U

#endif
