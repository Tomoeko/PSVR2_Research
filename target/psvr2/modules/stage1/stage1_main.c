/*
 * stage1_main.c - Fast file transfer + command execution for PSVR2
 *
 * Kernel module: creates /proc/stage1 for USB bulk file transfer
 * and /proc/stage1_out for command output retrieval.
 *
 * Protocol (via /proc/stage1):
 *   Write "recv <path> <size>"      -> receive one file from USB OUT ep5
 *   Write "recv_multi <p1> <s1> .." -> receive files in one session
 *   Write "recv_stop"               -> abort active receive
 *   Write "send <path>"             -> send file to host via USB IN ep5
 *   Write "send_stop"               -> abort active send
 *   Write "send_prepare"            -> cancel native IN requests before drain
 *   Write "send_probe"              -> dump gbuf->in structure
 *   Read                            -> return the status string
 *
 * Command mailbox (direct BSS polling, no proc write needed):
 *   Host writes command to cmd_in_buf via write_u64_fast
 *   Host writes length to cmd_in_len -> triggers kthread
 *   Kthread executes via call_usermodehelper(UMH_WAIT_PROC)
 *   Output stored in cmd_out_buf, read via /proc/stage1_out or arbread
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/namei.h>
#include <linux/usb/gadget.h>
#include <linux/kallsyms.h>
#include <linux/mm.h>
#include <linux/rcupdate.h>
#include <asm/cacheflush.h>

#if defined(PSVR2_SOURCE_FAMILY_0110)
#define S1_FW_VERSION  0x0110
#elif defined(PSVR2_SOURCE_FAMILY_0600)
#define S1_FW_VERSION  0x0600
#else
#error "stage1 requires a supported PSVR2 source family"
#endif

#define STATUS_SIZE    256
#define MAX_PATH_LEN   128

/* Shared state */
static char status_buf[STATUS_SIZE];

/* gbuf layout (from IDA: sieusb.ko) */
struct s1_gbuf {
    void *in;   /* gbuf_one *in  at offset 0 */
    void *out;  /* gbuf_one *out at offset 8 */
};

#define GO_BUSYCOUNT  96
#define GO_PACKETLEN  128
#define GO_EP         160
#define GO_ITEMS      136
#define GO_DMABUFS    144

#define GO_PKTLEN(base)    (*(size_t *)((char *)(base) + GO_PACKETLEN))
#define GO_EPPTR(base)     (*(struct usb_ep **)((char *)(base) + GO_EP))
#define GO_BUSYCNT(base)   (*(atomic_t *)((char *)(base) + GO_BUSYCOUNT))

/* dmabuf offsets */
#define DMA_REQ       16

/*
 * Keep the private implementation fragments in one translation unit.  The
 * mailbox is accessed by direct kernel memory clients, so this split improves
 * ownership without introducing accidental linker-order ABI changes.
 */
#include "stage1_auth.inc"
#include "stage1_usb.inc"
#include "stage1_recv.inc"
#include "stage1_mailbox.inc"
#include "stage1_send.inc"
#include "stage1_proc.inc"
