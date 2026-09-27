/*
 * stage3_serial.c — Inject CDC-ACM serial into sieusb's composite gadget
 *
 * Navigates from /dev/usb_data3's cdev through the sieusb gbuf structure
 * to reach the USB composite gadget, then adds an ACM serial function.
 *
 * Dependencies (load order):
 *   1. libcomposite.ko  (already loaded on device)
 *   2. u_serial.ko      (tty layer — creates /dev/ttyGS0)
 *   3. usb_f_acm.ko     (registers "acm" function factory)
 *   4. stage3_serial.ko (this module)
 *
 * Result: macOS sees new ACM interface → /dev/tty.usbmodem*
 *         Device gets /dev/ttyGS0 for shell I/O
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/proc_fs.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/timer.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <asm/ioctls.h>   /* TCSETS */
#include <asm/termbits.h> /* struct termios, B921600 */
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/usb/composite.h>
#include <linux/usb/gadget.h>
#include <linux/wait.h>
#include "u_serial.h"

#include <linux/cdev.h>

#if defined(PSVR2_SOURCE_FAMILY_0110)
#define S3_FW_VERSION 0x0110
#define S3_FIRMWARE_NAME "01.10"
#elif defined(PSVR2_SOURCE_FAMILY_0600)
#define S3_FW_VERSION 0x0600
#define S3_FIRMWARE_NAME "06.00"
#else
#error "stage3_serial requires a supported PSVR2 source family"
#endif

/* Module parameter: set to 1 for diagnostic-only mode (no ACM injection) */
static int diag_only = 0;
module_param(diag_only, int, 0);
MODULE_PARM_DESC(diag_only, "1=diagnostic only (verify offsets, no injection)");

/* Preserve the single-port layout unless double eviction is explicitly
 * requested. noevict remains for command-line compatibility. */
static int noevict = 1;
module_param(noevict, int, 0);
MODULE_PARM_DESC(
    noevict,
    "1=preserve Sony data functions and inject one ACM port (default)");

static int double_evict = 0;
module_param(double_evict, int, 0);
MODULE_PARM_DESC(
    double_evict,
    "1=opt in to evict Sony data8/data9 and inject two ACM ports");

/* Module parameter: address of sieusb's 'ts' variable (ktime_get_seconds
 * timestamp). vrhmd_main.elf polls /sys/module/sieusb/parameters/ts to detect
 * host absence. We write ktime_get_seconds() here every 4s to prevent the
 * 30-minute power-off timeout. If 0, stage3_init auto-resolves from the sieusb
 * module base + 0x8550. */
static unsigned long sieusb_ts_addr = 0;
module_param(sieusb_ts_addr, ulong, 0);
MODULE_PARM_DESC(sieusb_ts_addr, "Address of sieusb dword_8550 (ts param)");

/* Module parameter: path to busybox binary (for /data/modules persistence) */
static char *bb_path = "/tmp/busybox";
module_param(bb_path, charp, 0);
MODULE_PARM_DESC(bb_path, "Path to busybox binary (default: /tmp/busybox)");

/* ── Options ── */
#define STAGE3_VERBOSE_DIAG
#define STAGE3_SHELL_ENABLED

/*
 * The 01.10 and 06.00 exact kernels share this artifact-verified arm64
 * machine_power_off().  It disables
 * local interrupts, stops secondary CPUs, and enters the registered PSCI
 * SYSTEM_OFF callback without the hanging userspace/device-shutdown path.
 */
#define S3_MACHINE_POWER_OFF_ADDR 0xffffffc000085330UL

/* ── State ── */
#define STATUS_SIZE 512
#define SIEUSB_MAX_FUNCS 13
static char status_buf[STATUS_SIZE];

static struct usb_function_instance *acm_inst[2];
static struct usb_function *acm_func[2];
static struct usb_configuration *target_config;
static struct usb_gadget *target_gadget;
static struct task_struct *keepalive_thread;
static struct task_struct *reset_thread;
static struct task_struct *bridge_thread;
static DECLARE_COMPLETION(bridge_tty_ready);
static struct task_struct *shell_thread;
static DEFINE_MUTEX(worker_lock);
static bool shell_started;
static int active_acm_ports;
static char shell_binary[256];
static bool shell_binary_is_busybox;

#define S3_DOUBLE_EVICT_COUNT 2
#define S3_EVICT_MAX_EPS 2
struct s3_evicted_endpoint {
  struct usb_ep *endpoint;
  void *driver_data;
  unsigned char address;
  bool claimed;
};
struct s3_evicted_function {
  struct usb_function *function;
  unsigned char interface_id;
  bool removed;
  struct s3_evicted_endpoint endpoints[S3_EVICT_MAX_EPS];
  unsigned int endpoint_count;
};
static struct s3_evicted_function
    evicted_functions[S3_DOUBLE_EVICT_COUNT];
static unsigned int evicted_function_count;
static unsigned char evicted_original_next_interface_id;

/* Forward declarations */
static int s3_shell_fn(void *data);
static int s3_input_bridge_fn(void *data);
static int s3_start_shell(void);
static int s3_start_bridge(void);
static int s3_start_reset(void);
static void s3_stop_shell_process(void);
static bool s3_hotpatch_vrhmd_text(bool dp_active);
static void s3_set_status(const char *fmt, ...);
static void s3_fast_stream_cleanup(void);
static void s3_fast_input_cleanup(void);
static void s3_remove_acm_function(
    struct usb_function *function);
static int s3_double_evict_sony_data(void);
static void s3_restore_evicted_sony_data(void);

static bool s3_executable_file(const char *path) {
  struct file *file;
  umode_t mode;

  if (!path || path[0] != '/')
    return false;
  file = filp_open(path, O_RDONLY, 0);
  if (IS_ERR(file))
    return false;
  mode = file_inode(file)->i_mode;
  filp_close(file, NULL);
  return S_ISREG(mode) && (mode & 0111);
}

static int s3_select_shell_binary(void) {
  const char *candidates[] = {
      bb_path, "/data/modules/busybox", "/tmp/busybox", "/bin/sh"};
  unsigned int i;

  shell_binary[0] = '\0';
  shell_binary_is_busybox = false;
  for (i = 0; i < ARRAY_SIZE(candidates); i++) {
    const char *candidate = candidates[i];
    unsigned int prior;
    bool duplicate = false;

    if (!candidate || !candidate[0])
      continue;
    for (prior = 0; prior < i; prior++) {
      if (candidates[prior] &&
          !strcmp(candidate, candidates[prior])) {
        duplicate = true;
        break;
      }
    }
    if (duplicate || !s3_executable_file(candidate))
      continue;
    if (strlcpy(shell_binary, candidate,
                sizeof(shell_binary)) >= sizeof(shell_binary))
      return -ENAMETOOLONG;
    shell_binary_is_busybox = strcmp(candidate, "/bin/sh") != 0;
    printk(KERN_INFO "stage3: interactive shell selected %s (%s)\n",
           shell_binary,
           shell_binary_is_busybox ? "BusyBox ash" : "firmware sh");
    return 0;
  }
  return -ENOENT;
}

/* ── Detection: Is Autoboot Disabled? ── */
static bool is_autoboot_disabled(void)
{
    struct file *fp;
    char *buf;
    bool fts_detected = false;
    bool vrhmd_running = false;
    bool disabled = false;

    /* Check /proc/fts for the flag (as per user suggestion) */
    buf = kmalloc(1024, GFP_KERNEL);
    if (buf) {
        fp = filp_open("/proc/fts", O_RDONLY, 0);
        if (!IS_ERR(fp)) {
            typedef int (*kr_fn_t)(struct file *, loff_t, char *, unsigned long);
            int nread = ((kr_fn_t)kernel_read)(fp, 0, buf, 1023);
            if (nread > 0) {
                buf[nread] = '\0';
                if (strstr(buf, "disable_autoboot=1")) {
                    fts_detected = true;
                }
            }
            filp_close(fp, NULL);
        }
        kfree(buf);
    }

    /* Check process list */
    {
        struct task_struct *task;
        rcu_read_lock();
        for_each_process(task) {
            if (strcmp(task->comm, "VrhmdMain") == 0) {
                vrhmd_running = true;
                break;
            }
        }
        rcu_read_unlock();
    }

    disabled = fts_detected || !vrhmd_running;

    printk(KERN_INFO "stage3: boot mode detection: fts=%d vrhmd=%s -> autoboot_disabled=%d\n",
           fts_detected, vrhmd_running ? "running" : "NOT running", disabled);

    return disabled;
}

/* Offset from chrdev to usb_function base (verified via IDA) */
#define CHRDEV_TO_FUNC_OFFSET 352
#define S3_USB_FUNCTION_CONFIG_OFFSET \
  offsetof(struct usb_function, config)
#define S3_CONFIG_CDEV_OFFSET \
  offsetof(struct usb_configuration, cdev)
#define S3_CONFIG_NEXT_INTERFACE_ID_OFFSET \
  offsetof(struct usb_configuration, next_interface_id)

/* ── /proc/stage3 status ── */
static ssize_t s3_proc_read(struct file *file, char __user *ubuf, size_t count,
                            loff_t *ppos) {
  return simple_read_from_buffer(ubuf, count, ppos, status_buf,
                                 strlen(status_buf));
}

/* ── Takeover a sieusb data endpoint for video streaming ──
 *
 * Clears GBUF_FLAG_OPEN (bit 2) and GBUF_WRITE_COMPLETELY (bit 4)
 * in the gbuf.flags of the target /dev/usb_dataN, allowing
 * dprx_stream to open() the device even while VrhmdMain has it open.
 *
 * Struct layout (verified via IDA + sizeof analysis):
 *   cdev → func (cdev - 352)
 *   func + 232: struct gbuf
 *   gbuf + 72:  unsigned long flags
 *   gbuf + 80:  char name[16]  (for verification)
 *
 * gbuf.flags bits:
 *   bit 0: GBUF_FLAG_INITIALIZING
 *   bit 1: GBUF_FLAG_INIT
 *   bit 2: GBUF_FLAG_OPEN          ← clear to allow open()
 *   bit 3: GBUF_READ_INDIVIDUALLY
 *   bit 4: GBUF_WRITE_COMPLETELY   ← clear for large writev()
 *   bit 5: GBUF_FLAG_CONSTRUCTED
 */
#define GBUF_OFFSET_FROM_FUNC  232
#define GBUF_FLAGS_OFFSET       72
#define GBUF_NAME_OFFSET        80

static void s3_probe_usb_data(int dev_idx) {
  char dev_path[32];
  struct path usb_path;
  struct cdev *char_dev;
  char *func_base;
  char name_buf[16];

  if (dev_idx < 1 || dev_idx > 9) {
    s3_set_status("ERR probe: invalid index %d", dev_idx);
    return;
  }

  snprintf(dev_path, sizeof(dev_path), "/dev/usb_data%d", dev_idx);
  if (kern_path(dev_path, LOOKUP_FOLLOW, &usb_path) != 0) {
    s3_set_status("ERR probe: %s not found", dev_path);
    return;
  }

  if (!usb_path.dentry->d_inode->i_cdev) {
    /* If vrhmd hasn't opened it, force a lazy bind for probing */
    struct file *dummy = filp_open(dev_path, O_RDONLY | O_NONBLOCK, 0);
    if (!IS_ERR(dummy)) filp_close(dummy, NULL);
  }

  char_dev = usb_path.dentry->d_inode->i_cdev;
  path_put(&usb_path);

  if (!char_dev) {
    s3_set_status("ERR probe: %s no cdev", dev_path);
    return;
  }

  func_base = (char *)char_dev - CHRDEV_TO_FUNC_OFFSET;
  memset(name_buf, 0, sizeof(name_buf));
  probe_kernel_read(name_buf, func_base + GBUF_OFFSET_FROM_FUNC + GBUF_NAME_OFFSET,
                    sizeof(name_buf) - 1);

  if (strncmp(name_buf, dev_path + strlen("/dev/usb_"),
              sizeof(name_buf) - 1) != 0) {
    printk(KERN_ERR "stage3: probe %s rejected mismatched gbuf name %s\n",
           dev_path, name_buf);
    s3_set_status("ERR probe %s name=%s", dev_path, name_buf);
    return;
  }

  printk(KERN_INFO "stage3: probe %s: name=\"%s\"\n", dev_path, name_buf);
  s3_set_status("OK probe %s name=%s", dev_path, name_buf);
}

static void s3_takeover_usb_data(int dev_idx) {
  char dev_path[32];
  struct path usb_path;
  struct cdev *char_dev;
  char *func_base;
  unsigned long flags_val;
  char name_buf[16];
  bool setup_input = false;

  if (dev_idx < 0) {
    setup_input = true;
    dev_idx = -dev_idx;
  }

  if (dev_idx < 1 || dev_idx > 9) {
    printk(KERN_ERR "stage3: takeover: invalid index %d (use 1-9)\n", dev_idx);
    s3_set_status("ERR takeover: invalid index %d", dev_idx);
    return;
  }

  snprintf(dev_path, sizeof(dev_path), "/dev/usb_data%d", dev_idx);

  if (kern_path(dev_path, LOOKUP_FOLLOW, &usb_path) != 0) {
    printk(KERN_ERR "stage3: takeover: %s not found\n", dev_path);
    s3_set_status("ERR takeover: %s not found", dev_path);
    return;
  }

  if (!usb_path.dentry->d_inode) {
    path_put(&usb_path);
    printk(KERN_ERR "stage3: takeover: %s has no inode\n", dev_path);
    s3_set_status("ERR takeover: %s no inode", dev_path);
    return;
  }

  /* Autoboot Disabled Fix: i_cdev is populated lazily on first open.
   * If vrhmd hasn't naturally opened it, force the kernel to bind it. */
  if (!usb_path.dentry->d_inode->i_cdev && is_autoboot_disabled()) {
    struct file *dummy = filp_open(dev_path, O_RDONLY | O_NONBLOCK, 0);
    if (!IS_ERR(dummy))
      filp_close(dummy, NULL);
  }

  if (!usb_path.dentry->d_inode->i_cdev) {
    path_put(&usb_path);
    printk(KERN_ERR "stage3: takeover: %s has no cdev\n", dev_path);
    s3_set_status("ERR takeover: %s no cdev", dev_path);
    return;
  }

  char_dev = usb_path.dentry->d_inode->i_cdev;
  path_put(&usb_path);

  func_base = (char *)char_dev - CHRDEV_TO_FUNC_OFFSET;

  /* Verify by reading gbuf.name — should match "dataN" */
  memset(name_buf, 0, sizeof(name_buf));
  probe_kernel_read(name_buf, func_base + GBUF_OFFSET_FROM_FUNC + GBUF_NAME_OFFSET,
                    sizeof(name_buf) - 1);

  printk(KERN_INFO "stage3: takeover %s: cdev=%px func=%px gbuf_name=\"%s\"\n",
         dev_path, char_dev, func_base, name_buf);

  if (strncmp(name_buf, dev_path + strlen("/dev/usb_"),
              sizeof(name_buf) - 1) != 0) {
    printk(KERN_ERR "stage3: refusing %s: mismatched gbuf name %s\n",
           dev_path, name_buf);
    s3_set_status("ERR takeover %s name=%s", dev_path, name_buf);
    return;
  }

  /* Read current flags */
  probe_kernel_read(&flags_val,
                    func_base + GBUF_OFFSET_FROM_FUNC + GBUF_FLAGS_OFFSET,
                    sizeof(flags_val));

  printk(KERN_INFO "stage3: takeover %s: gbuf.flags = 0x%lx ="
         " INIT=%d OPEN=%d WRITE_COMP=%d\n",
         dev_path, flags_val,
         !!(flags_val & (1UL << 1)),
         !!(flags_val & (1UL << 2)),
         !!(flags_val & (1UL << 4)));

  /* Clear GBUF_FLAG_OPEN (bit 2) and GBUF_WRITE_COMPLETELY (bit 4) */
  {
    unsigned long new_flags = flags_val & ~((1UL << 2) | (1UL << 4));
    volatile unsigned long *flags_ptr =
        (volatile unsigned long *)(func_base + GBUF_OFFSET_FROM_FUNC +
                                   GBUF_FLAGS_OFFSET);
    *flags_ptr = new_flags;
    smp_wmb();

    printk(KERN_INFO "stage3: takeover %s: flags 0x%lx → 0x%lx "
                     "(cleared OPEN + WRITE_COMPLETELY)\n",
           dev_path, flags_val, new_flags);
    s3_set_status("OK takeover %s flags=0x%lx→0x%lx name=%s",
                  dev_path, flags_val, new_flags, name_buf);
  }

  /* ── Set up /dev/fast_stream or _input ── */
  {
    struct usb_ep *ep = NULL;
    int ep_offset = setup_input ? 224 : 216; /* v01.10 uses 216/224 for Data2 */
    probe_kernel_read(&ep, func_base + ep_offset, sizeof(ep));

    if (ep) {
      if (setup_input) {
        extern int s3_fast_input_setup(struct usb_ep *ep);
        printk(KERN_INFO "stage3: setting up fast_input on OUT ep %s\n", ep->name);
        s3_fast_input_setup(ep);
      } else {
        extern int s3_fast_stream_setup(struct usb_ep *ep);
        printk(KERN_INFO "stage3: setting up fast_stream on IN ep %s\n", ep->name);
        s3_fast_stream_setup(ep);
      }
    } else {
      printk(KERN_ERR "stage3: failed to resolve %s endpoint at offset %d\n",
             setup_input ? "OUT" : "IN", ep_offset);
    }
  }
}

/* ══════════════════════════════════════════════════════════════════════════════
 *  /dev/fast_stream — Direct-to-UDC USB streaming (bypasses gbuf 16KB limit)
 *
 *  Root cause of 9 FPS: gbuf uses 300×16KB buffers with QUEUE_MAX=16.
 *  Each of the 517 USB transfers per frame incurs ~200μs interrupt overhead.
 *  517 × 200μs = 103ms per frame.
 *
 *  Fix: Use 256KB buffers → 33 transfers/frame → ~7ms.
 * ══════════════════════════════════════════════════════════════════════════════ */

#define FAST_BUFSZ  65532         /* MTU3 UDC max request length */
#define FAST_NBUFS  130           /* enough for full frame: 8467200/65532=130 */

static struct usb_ep *fast_ep;
static struct usb_request *fast_reqs[FAST_NBUFS];
static struct completion fast_comp[FAST_NBUFS];
static DEFINE_MUTEX(fast_write_mutex);
static bool fast_ready;
static bool fast_registered;

static void fast_complete_cb(struct usb_ep *ep, struct usb_request *req) {
  int idx = (int)(long)req->context;
  complete(&fast_comp[idx]);
}

static int fast_open(struct inode *inode, struct file *file) {
  if (!fast_ready || !fast_ep) return -ENODEV;
  return 0;
}

static ssize_t fast_write(struct file *file, const char __user *buf,
                          size_t count, loff_t *ppos) {
  size_t written = 0;
  int idx = 0;

  if (!fast_ready) return -ENODEV;
  if (mutex_lock_interruptible(&fast_write_mutex)) return -ERESTARTSYS;

  while (written < count) {
    size_t n = count - written;
    int ret;
    if (n > FAST_BUFSZ) n = FAST_BUFSZ;

    /* Wait for this slot to finish transmitting from previous rounds. */
    wait_for_completion(&fast_comp[idx]);
    reinit_completion(&fast_comp[idx]);

    if (copy_from_user(fast_reqs[idx]->buf, buf + written, n)) {
      complete(&fast_comp[idx]); /* release slot */
      mutex_unlock(&fast_write_mutex);
      return written > 0 ? written : -EFAULT;
    }

    fast_reqs[idx]->length = n;
    ret = usb_ep_queue(fast_ep, fast_reqs[idx], GFP_KERNEL);
    if (ret < 0) {
      complete(&fast_comp[idx]);
      mutex_unlock(&fast_write_mutex);
      return written > 0
          ? written
          : ((ret == -ESHUTDOWN || ret == -ENODEV)
                 ? -EPIPE : ret);
    }

    written += n;
    idx = (idx + 1) % FAST_NBUFS;
  }

  /* Wait for all in-flight requests to complete */
  { int i; for (i = 0; i < FAST_NBUFS; i++) {
      wait_for_completion(&fast_comp[i]);
  }}
  /* Re-mark all slots as available for next write */
  { int i; for (i = 0; i < FAST_NBUFS; i++) complete(&fast_comp[i]); }

  mutex_unlock(&fast_write_mutex);
  return written;
}

static int fast_release(struct inode *inode, struct file *file) {
  int i;
  /* Wait for completion of all pending requests to avoid OOPS */
  if (fast_ready && fast_ep) {
    for (i = 0; i < FAST_NBUFS; i++) {
       wait_for_completion(&fast_comp[i]);
       /* Put them back to ready for next open */
       complete(&fast_comp[i]);
    }
  }
  return 0;
}

static const struct file_operations fast_fops = {
  .owner   = THIS_MODULE,
  .open    = fast_open,
  .write   = fast_write,
  .release = fast_release,
};

static struct miscdevice fast_misc = {
  .minor = MISC_DYNAMIC_MINOR,
  .name  = "fast_stream",
  .fops  = &fast_fops,
};

/* ══════════════════════════════════════════════════════════════════════════════
 *  /dev/fast_input — Host-to-Device input streaming
 *
 *  Two data paths:
 *  1. HARDWARE PATH: USB OUT endpoint completions advance head via IRQ callback
 *  2. BRIDGE PATH: s3_input_bridge_fn reads from ttyGS1 and injects directly
 *
 *  When the bridge is active, hardware USB requests are dequeued to avoid
 *  interleaving stale/duplicate data with bridge data.
 * ══════════════════════════════════════════════════════════════════════════════ */
int s3_fast_stream_setup(struct usb_ep *ep) {
  int i, ret = -ENOMEM;

  if (fast_ready) {
    printk(KERN_INFO "stage3: fast_stream already set up\n");
    return 0;
  }

  fast_ep = ep;

  for (i = 0; i < FAST_NBUFS; i++) {
    fast_reqs[i] = usb_ep_alloc_request(ep, GFP_KERNEL);
    if (!fast_reqs[i]) {
      printk(KERN_ERR "stage3: fast_stream: alloc_request %d failed\n", i);
      goto fail;
    }
    fast_reqs[i]->buf = kmalloc(FAST_BUFSZ, GFP_KERNEL);
    if (!fast_reqs[i]->buf) {
      printk(KERN_ERR "stage3: fast_stream: kmalloc %d failed\n", i);
      usb_ep_free_request(ep, fast_reqs[i]);
      fast_reqs[i] = NULL;
      goto fail;
    }
    fast_reqs[i]->complete = fast_complete_cb;
    fast_reqs[i]->context = (void *)(long)i;
    init_completion(&fast_comp[i]);
    complete(&fast_comp[i]); /* mark as free */
  }

  if (!fast_registered) {
    ret = misc_register(&fast_misc);
    if (ret < 0) {
      printk(KERN_ERR "stage3: fast_stream: misc_register failed %d\n", ret);
      goto fail;
    }
    fast_registered = true;
  }

  fast_ready = true;
  printk(KERN_INFO "stage3: /dev/fast_stream ready (ep=%s, %d×%dKB)\n",
         ep->name, FAST_NBUFS, FAST_BUFSZ / 1024);
  return 0;

fail:
  fast_ep = NULL;
  for (i = 0; i < FAST_NBUFS; i++) {
    if (fast_reqs[i]) {
      kfree(fast_reqs[i]->buf);
      usb_ep_free_request(ep, fast_reqs[i]);
      fast_reqs[i] = NULL;
    }
  }
  return ret < 0 ? ret : -ENOMEM;
}

#include "stage3_input_diagnostics.inc"
#include "stage3_fast_input.inc"

#include "stage3_stream_teardown.inc"

static void s3_set_status(const char *fmt, ...);

/* Robustly find sieaudio pdata by parsing sieAudioGetHandle instructions:
 * ADRP X0, #pdata_page
 * LDR  X19, [X0, #pdata_offset]
 */
static unsigned long get_pdata_ptr_addr(unsigned long func_addr) {
    uint32_t adrp_instr, ldr_instr;
    long immhi, immlo, imm;
    unsigned long page, base, offset;

    if (probe_kernel_read(&adrp_instr, (void *)(func_addr + 4), 4) != 0) return 0;
    if (probe_kernel_read(&ldr_instr, (void *)(func_addr + 16), 4) != 0) return 0;

    if ((adrp_instr & 0x9F000000) != 0x90000000) return 0;
    immhi = (adrp_instr >> 5) & 0x7FFFF;
    immlo = (adrp_instr >> 29) & 3;
    imm = (immhi << 2) | immlo;
    if (imm & 0x100000) imm -= 0x200000;
    page = (func_addr + 4) & ~0xFFF;
    base = page + (imm << 12);

    if ((ldr_instr & 0xFFC00000) != 0xF9400000) return 0;
    offset = ((ldr_instr >> 10) & 0xFFF) << 3;

    return base + offset;
}

/* Robustly find afe_data.prepared by parsing mt_afe_transfer_start instructions:
 * ADRP X20, #afe_data_page
 * LDRB W1, [X20, #prepared_offset]
 */
static unsigned long get_afe_data_addr(unsigned long func_addr) {
    uint32_t adrp_instr, ldrb_instr;
    long immhi, immlo, imm;
    unsigned long page, base, offset;

    if (probe_kernel_read(&adrp_instr, (void *)(func_addr + 0x14), 4) != 0) return 0;
    if (probe_kernel_read(&ldrb_instr, (void *)(func_addr + 0x20), 4) != 0) return 0;

    if ((adrp_instr & 0x9F000000) != 0x90000000) return 0;
    immhi = (adrp_instr >> 5) & 0x7FFFF;
    immlo = (adrp_instr >> 29) & 3;
    imm = (immhi << 2) | immlo;
    if (imm & 0x100000) imm -= 0x200000;
    page = (func_addr + 0x14) & ~0xFFF;
    base = page + (imm << 12);

    /* LDRB (unsigned offset): 00 111 0 01 01 imm(12) rn(5) rt(5) */
    if ((ldrb_instr & 0xFFC00000) != 0x39400000) return 0;
    offset = (ldrb_instr >> 10) & 0xFFF;

    return base + offset;
}

static ssize_t s3_proc_write(struct file *file, const char __user *ubuf,
                             size_t count, loff_t *ppos) {
  char buf[32];
  char *command;
  int idx;

  if (!count)
    return 0;
  if (count >= sizeof(buf))
    return -E2BIG;
  if (copy_from_user(buf, ubuf, count))
    return -EFAULT;

  buf[count] = '\0';
  command = strim(buf);

  if (!strcmp(command, "reboot")) {
    /*
     * orderly_reboot() merely schedules a userspace helper and can return
     * successfully without restarting on the headset. emergency_restart()
     * is exported by this kernel and bypasses the multi-second graceful
     * device-shutdown path.
     */
    printk(KERN_EMERG
           "stage3: trigger immediate reboot via /proc/stage3\n");
    emergency_restart();
  } else if (!strcmp(command, "shutdown")) {
    void (*machine_power_off_exact)(void) =
        (void (*)(void))S3_MACHINE_POWER_OFF_ADDR;

    printk(KERN_EMERG
           "stage3: trigger immediate power off via /proc/stage3\n");
    machine_power_off_exact();
    printk(KERN_ERR "stage3: machine_power_off unexpectedly returned\n");
    return -EIO;
  } else if (!strcmp(command, "shell")) {
    printk(KERN_INFO "stage3: manual shell respawn requested...\n");
    if (s3_start_shell())
      return -EBUSY;
  } else if (!strcmp(command, "alive")) {
    printk(KERN_INFO "stage3: manual keepalive patch triggered...\n");
    s3_hotpatch_vrhmd_text(false);
  } else if (!strncmp(command, "probe ", 6) &&
             !kstrtoint(command + 6, 10, &idx) &&
             idx >= 1 && idx <= 9) {
    s3_probe_usb_data(idx);
  } else if (!strncmp(command, "takeover ", 9) &&
             !kstrtoint(command + 9, 10, &idx) &&
             idx >= 1 && idx <= 9) {
      printk(KERN_INFO "stage3: takeover usb_data%d requested\n", idx);
      s3_takeover_usb_data(idx);
  } else if (!strcmp(command, "input status")) {
    s3_input_bridge_status();
  } else if (!strcmp(command, "input bridge")) {
    int ret = s3_fast_input_bridge_setup();
    if (ret) {
      s3_set_status("ERR input bridge setup %d", ret);
      return ret;
    }
    s3_set_status("OK input bridge ttyGS1 software ring");
  } else if (!strncmp(command, "input ", 6) &&
             !kstrtoint(command + 6, 10, &idx) &&
             idx >= 1 && idx <= 9) {
    printk(KERN_INFO "stage3: input takeover usb_data%d requested\n", idx);
    s3_takeover_usb_data(-idx);
  } else if (!strcmp(command, "audiopatch")) {
    /* Patch sieaudio.ko afe_data to force full transfer_start path.
     *
     * The retail kernel's transfer_start has a "no port change" short
     * path that skips DMA start. We reset transfer_port to 0 so the
     * next TRANSFER_START ioctl takes the full path with DMA init.
     */
    unsigned long sym_get_handle = kallsyms_lookup_name("sieAudioGetHandle");
    unsigned long sym_transfer_start = kallsyms_lookup_name("mt_afe_transfer_start");

    if (!sym_get_handle || !sym_transfer_start) {
      printk(KERN_ERR "stage3: audiopatch: required symbols not in kallsyms\n");
      s3_set_status("ERR audiopatch: symbols missing");
    } else {
      unsigned long afe_prepared_addr = get_afe_data_addr(sym_transfer_start);
      unsigned long pdata_ptr_addr = get_pdata_ptr_addr(sym_get_handle);

      if (!afe_prepared_addr || !pdata_ptr_addr) {
        printk(KERN_ERR "stage3: audiopatch: failed to parse instructions (afe=0x%lx, pdata=0x%lx)\n",
               afe_prepared_addr, pdata_ptr_addr);
        s3_set_status("ERR audiopatch: parse failed");
      } else {
        volatile uint8_t *p_prepared = (volatile uint8_t *)afe_prepared_addr;
        volatile uint8_t *p_ts = (volatile uint8_t *)(afe_prepared_addr + 0x18);
        volatile uint32_t *p_port = (volatile uint32_t *)(afe_prepared_addr + 0x1C);
        void **pp_pdata = (void **)pdata_ptr_addr;
        void *p_pdata_val = NULL;
        uint8_t old_prep, old_ts_local;
        uint32_t old_port;

        if (probe_kernel_read(&old_prep, (void *)p_prepared, 1) == 0 &&
            probe_kernel_read(&old_ts_local, (void *)p_ts, 1) == 0 &&
            probe_kernel_read(&old_port, (void *)p_port, 4) == 0) {

          printk(KERN_INFO "stage3: audiopatch: found afe_data at 0x%lx (prep=%u port=%u ts=%u)\n",
                 pdata_ptr_addr - 0x3B8, old_prep, old_port, old_ts_local);

          *p_prepared = 0;
          *p_port = 0;
          *p_ts = 0;
          smp_wmb();

          /* Also clear sieaudio.ko's global transfer_started flag to defuse takeover race */
          if (probe_kernel_read(&p_pdata_val, pp_pdata, sizeof(void *)) == 0 && p_pdata_val != NULL) {
            volatile int *p_ts_global = (volatile int *)((unsigned char *)p_pdata_val + 8);
            int old_ts_global;
            if (probe_kernel_read(&old_ts_global, (void *)p_ts_global, 4) == 0) {
              *p_ts_global = 0;
              printk(KERN_INFO "stage3: audiopatch: cleared sieaudio pdata->ts (%d -> 0)\n", old_ts_global);
            }
          }
          s3_set_status("OK audiopatch prep=%u port=%u ts=%u", old_prep, old_port, old_ts_local);
        } else {
          printk(KERN_ERR "stage3: audiopatch: probe failed at 0x%lx\n", pdata_ptr_addr);
          s3_set_status("ERR audiopatch: probe failed");
        }
      }
    }
  } else {
    s3_set_status("ERR unknown command");
    return -EINVAL;
  }
  return count;
}

static const struct file_operations s3_proc_fops = {
    .owner = THIS_MODULE,
    .read = s3_proc_read,
    .write = s3_proc_write,
};

static struct proc_dir_entry *s3_proc;

static void s3_set_status(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(status_buf, STATUS_SIZE, fmt, args);
  va_end(args);
}

typedef int (*access_vm_fn_t)(struct task_struct *, unsigned long, void *, int,
                              unsigned int);

/* v01.10 Autonomous Keepalive: Apply .text hotpatches securely */
static bool s3_hotpatch_vrhmd_text(bool dp_active) {
#if defined(PSVR2_SOURCE_FAMILY_0600)
  (void)dp_active;
  printk(KERN_INFO
         "stage3: VrhmdMain keepalive text patches are unavailable on %s\n",
         S3_FIRMWARE_NAME);
  return false;
#else
  struct task_struct *task;
  struct mm_struct *mm;
  unsigned long base_addr = 0;
  bool injection_success = false;
  static access_vm_fn_t kfn_access_vm = NULL;

  if (!kfn_access_vm)
    kfn_access_vm = (access_vm_fn_t)kallsyms_lookup_name("access_process_vm");

  if (!kfn_access_vm) {
    printk(KERN_ERR
           "stage3: v01.10 KEEP ALIVE FAILED - access_process_vm not found!\n");
    return false;
  }

  rcu_read_lock();
  for_each_process(task) {
    if (strcmp(task->comm, "VrhmdMain") == 0) {
      get_task_struct(task);
      rcu_read_unlock(); /* Exit RCU early since we have a reference to the task
                          */

      mm = get_task_mm(task);
      if (mm) {
        struct vm_area_struct *vma;

        down_read(&mm->mmap_sem);
        vma = mm->mmap;
        while (vma) {
          if (vma->vm_flags & VM_EXEC) {
            base_addr = vma->vm_start;
            break;
          }
          vma = vma->vm_next;
        }
        up_read(&mm->mmap_sem);

        if (base_addr) {
          unsigned long patch1 = 0xD65F03C052800020ULL; /* MOV W0, #1 ; RET */
          unsigned long patch2 = 0xD65F03C052800080ULL; /* MOV W0, #4 ; RET */
          unsigned int patch3 = 0xd503201f;             /* NOP */
          unsigned long original1 = 0;
          unsigned long original2 = 0;
          unsigned int original3 = 0;
          int read1, read2, read3;

          read1 = kfn_access_vm(task, base_addr + 0xE340, &original1,
                                sizeof(original1), 0);
          read2 = kfn_access_vm(task, base_addr + 0x2423C, &original2,
                                sizeof(original2), 0);
          read3 = kfn_access_vm(task, base_addr + 0x30018, &original3,
                                sizeof(original3), 0);
          if (read1 != sizeof(original1) ||
              read2 != sizeof(original2) ||
              read3 != sizeof(original3) ||
              (original1 != 0x395FB000D0000AA0ULL &&
               original1 != patch1) ||
              (original2 != 0xB945D000F00009E0ULL &&
               original2 != patch2) ||
              (original3 != 0x97FF6FEC && original3 != patch3)) {
            printk(KERN_ERR
                   "stage3: refusing VrhmdMain patch: exact 01.10 anchors "
                   "mismatch (%016lx %016lx %08x)\n",
                   original1, original2, original3);
            mmput(mm);
            put_task_struct(task);
            return false;
          }

          if (dp_active) {
            int ret_write3;
            printk(KERN_INFO
                   "stage3: Active DP! Applying only log spam patch.\n");
            ret_write3 = kfn_access_vm(task, base_addr + 0x30018, &patch3,
                                       sizeof(patch3), 1);
            injection_success = (ret_write3 == sizeof(patch3));
          } else {
            int ret_write1, ret_write2, ret_write3;
            printk(
                KERN_INFO
                "stage3: Headless mode! Applying Video Stable hotpatches...\n");
            ret_write1 = kfn_access_vm(task, base_addr + 0xE340, &patch1,
                                       sizeof(patch1), 1);
            ret_write2 = kfn_access_vm(task, base_addr + 0x2423C, &patch2,
                                       sizeof(patch2), 1);
            ret_write3 = kfn_access_vm(task, base_addr + 0x30018, &patch3,
                                       sizeof(patch3), 1);

            if (ret_write1 == sizeof(patch1) && ret_write2 == sizeof(patch2) &&
                ret_write3 == sizeof(patch3)) {
              printk(KERN_INFO "stage3: v01.10 autonomous keepalive .text "
                               "patching successful!\n");
              injection_success = true;
            } else {
              printk(KERN_ERR "stage3: v01.10 .text patch failed\n");
            }
          }
        }
        mmput(mm);
      }
      put_task_struct(task);
      return injection_success;
    }
  }
  rcu_read_unlock();
  return false;
#endif
}

/* ── Keepalive thread: fake the host HID keepalive for vrhmd_main ──
 * vrhmd_main.elf reads /sys/module/sieusb/parameters/ts every ~1.5s.
 * That sysfs node exposes a ktime_get_seconds() timestamp that sieusb
 * normally sets on each USB ctrl_transfer(0x42, 0x09) from the host.
 * After USB re-enumeration the host channel is gone, so we write the
 * current kernel time directly to sieusb's backing variable to prevent
 * the 30-minute power-off timeout. */
static int s3_keepalive_fn(void *data) {
  extern bool dprx_get_video_stable_status(void);
  extern int dprx_get_dpcd_value(u32);
  extern int dprx_get_unplug_status(void);
  static bool log_spam_patched = false;

  if (sieusb_ts_addr) {
    printk(KERN_INFO "stage3: keepalive started (TS mode) at 0x%lx every 4s\n",
           sieusb_ts_addr);
  } else {
    printk(KERN_INFO "stage3: keepalive started (v01.10 Watchdog mode)\n");
  }

  while (!kthread_should_stop()) {
    if (sieusb_ts_addr) {
      unsigned int now = (unsigned int)ktime_get_seconds();
      unsigned int old = *(volatile unsigned int *)sieusb_ts_addr;
      /* Direct write to sieusb's dword_8550 (module param 'ts').
       * vrhmd polls this every ~1.5s and checks if it's ≤5s stale.
       * Must write faster than 5s to stay in HOST_ALIVE state. */
      *(volatile unsigned int *)sieusb_ts_addr = now;
      smp_wmb();
      /* Log first few updates for diagnostics */
      if (now - old > 10 || old == 0)
        printk(KERN_INFO "stage3: ts %u → %u (Δ%u)\n", old, now, now - old);
      msleep(4000);
    } else {
      /* v01.10 Keepalive Logic: Deep hardware DP vs Headless detection */
      unsigned int uptime = (unsigned int)ktime_get_seconds();
      bool video_stable = dprx_get_video_stable_status();
      int link_bw = dprx_get_dpcd_value(0x100); /* DPCD 0x100: LINK_BW_SET */
      int lane_status =
          dprx_get_dpcd_value(0x202);        /* DPCD 0x202: LANE0_1_STATUS */
      int unplug = dprx_get_unplug_status(); /* 0=Plugged, 1=Unplugged */

      /* A connection is ONLY active if Lane 0 has Clock Recovery (CR) done.
       * If unplugged or link is dead, lane_status will be 0 or -1. */
      bool cr_done = (lane_status != -1) && (lane_status & 0x01);
      bool dp_active = (unplug == 0) && cr_done;

      if (dp_active) {
        static bool dp_logged = false;
        if (!dp_logged) {
          printk(KERN_INFO "stage3: DP Alt Mode Active (stable=%d, bw=0x%x, "
                           "lane=0x%x). Skipping keepalive.\n",
                 video_stable, link_bw, lane_status);
          dp_logged = true;
        }

        /* Apply log spam fix if not already done */
        if (!log_spam_patched) {
          if (s3_hotpatch_vrhmd_text(true)) {
            log_spam_patched = true;
            printk(KERN_INFO "stage3: DP mode log-spam suppression applied.\n");
          }
        }
      }
      /* Transition to Headless: Wait for 22s timeout OR unmistakable physical
         unplug OR link loss */
      else if (uptime >= 22 || unplug == 1 ||
               (uptime > 5 && !cr_done && link_bw == 0)) {
        static bool headless_logged = false;
        if (!headless_logged) {
          printk(KERN_INFO "stage3: Headless mode (uptime=%us, unplug=%d, "
                           "lane=0x%x). Applying Keepalive...\n",
                 uptime, unplug, lane_status);
          headless_logged = true;
        }

        if (s3_hotpatch_vrhmd_text(false)) {
          printk(KERN_INFO "stage3: Headless Keepalive applied successfully. "
                           "Terminating monitoring.\n");
          break; /* Applied full patches, exit thread */
        }
      }

      msleep(2000);
    }
  }
  printk(KERN_INFO "stage3: keepalive thread stopped\n");
  return 0;
}

/* ── Shell thread: launch respawning interactive shell on ttyGS0 ── */
static int s3_shell_fn(void *data) {
  struct s3_shell_command_buffers {
    char exec_cmd[1024];
    char inner_cmd[640];
    char cleanup_cmd[384];
    char sleep_cmd[320];
  } *commands;
  char *exec_cmd;
  char *inner_cmd;
  char *cleanup_cmd;
  char *sleep_cmd;
  char path_env[256];
  char *bb_dir;
  char bb_dir_buf[128];
  int i, last_slash;
  char *argv[5];
  char *envp[4];
  int ret;

  ret = s3_select_shell_binary();
  if (ret) {
    s3_set_status("ERR no executable interactive shell %d", ret);
    printk(KERN_WARNING
           "stage3: neither BusyBox nor /bin/sh is executable (%d)\n",
           ret);
    goto out;
  }
  commands = kzalloc(sizeof(*commands), GFP_KERNEL);
  if (!commands) {
    ret = -ENOMEM;
    s3_set_status("ERR shell command allocation");
    goto out;
  }
  exec_cmd = commands->exec_cmd;
  inner_cmd = commands->inner_cmd;
  cleanup_cmd = commands->cleanup_cmd;
  sleep_cmd = commands->sleep_cmd;

  /* Extract directory from the selected BusyBox or firmware shell. */
  last_slash = 0;
  for (i = 0; shell_binary[i]; i++) {
    if (shell_binary[i] == '/')
      last_slash = i;
  }
  if (last_slash > 0 && last_slash < (int)sizeof(bb_dir_buf) - 1) {
    memcpy(bb_dir_buf, shell_binary, last_slash);
    bb_dir_buf[last_slash] = '\0';
    bb_dir = bb_dir_buf;
  } else {
    bb_dir = "/tmp";
  }

  snprintf(path_env, sizeof(path_env),
           "PATH=%s:/tmp/bin:/tmp:/bin:/sbin:/usr/bin:/usr/sbin", bb_dir);

  argv[0] = shell_binary;
  if (shell_binary_is_busybox) {
    argv[1] = "ash";
    argv[2] = "-c";
    argv[3] = exec_cmd;
    argv[4] = NULL;
    snprintf(inner_cmd, sizeof(commands->inner_cmd),
             "%s setsid %s ash -i", shell_binary, shell_binary);
    snprintf(sleep_cmd, sizeof(commands->sleep_cmd),
             "%s sleep 1", shell_binary);
    snprintf(cleanup_cmd, sizeof(commands->cleanup_cmd),
             "%s rm -f /tmp/.stage3_shell_pid "
             "/tmp/.stage3_shell_child /tmp/.stage3_shell_ready /tmp/.stage3_shell_stop",
             shell_binary);
  } else {
    argv[1] = "-c";
    argv[2] = exec_cmd;
    argv[3] = NULL;
    argv[4] = NULL;
    snprintf(inner_cmd, sizeof(commands->inner_cmd),
             "%s -i", shell_binary);
    snprintf(sleep_cmd, sizeof(commands->sleep_cmd), "sleep 1");
    snprintf(cleanup_cmd, sizeof(commands->cleanup_cmd),
             "rm -f /tmp/.stage3_shell_pid "
             "/tmp/.stage3_shell_child /tmp/.stage3_shell_ready /tmp/.stage3_shell_stop");
  }

  envp[0] = "HOME=/";
  envp[1] = path_env;
  envp[2] = "TERM=vt100";
  envp[3] = NULL;

  /* ── Launch self-respawning interactive shell ──
   * Logs each spawn/exit to /tmp/.shell_diag for post-mortem.
   *
   * CRITICAL DESIGN: The wrapper shell must NOT hold /dev/ttyGS0 open.
   * Only the inner `sh -i` should own the ttyGS0 file descriptors.
   * This ensures that when `sh -i` exits, gs_close() fires (count→0),
   * and when the next `sh -i` spawns, gs_open() sees count 0→1 and
   * calls gs_start_io() to re-queue the USB I/O buffers.
   *
   * If the wrapper held ttyGS0 open (e.g. via `exec <>/dev/ttyGS0`),
   * gs_open would just bump count (2→3→4...) without ever calling
   * gs_start_io, and data would silently stop flowing.
   *
   * TIMING: This runs BEFORE usb_gadget_connect(), so the first
   * `sh -i` opens ttyGS0 while port_usb == NULL. gs_open sets
   * port.count = 1 but skips gs_start_io (no USB yet). When we
   * reconnect and the host sends SET_CONFIGURATION, gserial_connect
   * sees port.count > 0 and calls gs_start_io with freshly-enabled
   * endpoints. This is the ONLY reliable activation path. */
  snprintf(exec_cmd, sizeof(commands->exec_cmd),
           "%s; "
           "echo $$ >/tmp/.stage3_shell_pid; "
           "trap '' HUP; "
           "echo \"shell_start $$ binary=%s\" >>/tmp/.shell_diag; "
           "while [ ! -e /tmp/.stage3_shell_stop ]; do "
           "  echo \"spawn $$ binary=%s\" >>/tmp/.shell_diag; "
           "  rm -f /tmp/.stage3_shell_ready; "
           "  ( : >/tmp/.stage3_shell_ready; exec %s ) "
           "    </dev/ttyGS0 >/dev/ttyGS0 2>&1 & "
           "  C=$!; echo $C >/tmp/.stage3_shell_child; wait $C; "
           "  RC=$?; "
           "  rm -f /tmp/.stage3_shell_child /tmp/.stage3_shell_ready; "
           "  echo \"exit rc=$RC\" >>/tmp/.shell_diag; "
           "  [ -e /tmp/.stage3_shell_stop ] || %s; "
           "done; "
           "%s",
           cleanup_cmd, shell_binary, shell_binary, inner_cmd,
           sleep_cmd, cleanup_cmd);

  printk(KERN_INFO "stage3: launching respawning shell on ttyGS0 "
                   "with %s (BEFORE USB reconnect — port_usb is NULL)\n",
         shell_binary);
  ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_EXEC);

  if (ret) {
    s3_set_status("ERR shell launch %d", ret);
    printk(KERN_WARNING "stage3: shell launch failed %d\n", ret);
  }
  kfree(commands);

out:
  mutex_lock(&worker_lock);
  if (shell_thread == current)
    shell_thread = NULL;
  if (ret)
    shell_started = false;
  mutex_unlock(&worker_lock);
  return 0;
}

static void s3_stop_shell_process(void) {
  char command[768];
  char sleep_command[320];
  char *argv[5];
  char *envp[3];
  const char *runner;
  int ret;

  if (!shell_started)
    return;

  runner = shell_binary[0] ? shell_binary : "/bin/sh";
  if (shell_binary_is_busybox)
    snprintf(sleep_command, sizeof(sleep_command),
             "%s sleep 1", runner);
  else
    snprintf(sleep_command, sizeof(sleep_command), "sleep 1");
  snprintf(
      command, sizeof(command),
      ": >/tmp/.stage3_shell_stop; "
      "if [ -r /tmp/.stage3_shell_child ]; then "
      "read C </tmp/.stage3_shell_child; kill -TERM \"$C\"; fi; "
      "%s; "
      "if [ -r /tmp/.stage3_shell_pid ]; then "
      "read P </tmp/.stage3_shell_pid; kill -TERM \"$P\"; fi",
      sleep_command);

  argv[0] = (char *)runner;
  if (shell_binary_is_busybox) {
    argv[1] = "ash";
    argv[2] = "-c";
    argv[3] = command;
    argv[4] = NULL;
  } else {
    argv[1] = "-c";
    argv[2] = command;
    argv[3] = NULL;
    argv[4] = NULL;
  }
  envp[0] = "HOME=/";
  envp[1] = "PATH=/tmp/bin:/tmp:/bin:/sbin:/usr/bin:/usr/sbin";
  envp[2] = NULL;

  ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
  if (ret)
    printk(KERN_WARNING "stage3: shell stop helper failed %d\n", ret);
  else
    printk(KERN_INFO "stage3: shell wrapper stopped before ACM teardown\n");
  shell_started = false;
}

static int s3_start_shell(void) {
  struct task_struct *thread;

  mutex_lock(&worker_lock);
  if (shell_started || shell_thread) {
    mutex_unlock(&worker_lock);
    return -EALREADY;
  }

  thread = kthread_create(s3_shell_fn, NULL, "stage3_shell");
  if (IS_ERR(thread)) {
    int ret = PTR_ERR(thread);
    mutex_unlock(&worker_lock);
    return ret;
  }

  shell_thread = thread;
  shell_started = true;
  wake_up_process(thread);
  mutex_unlock(&worker_lock);
  return 0;
}

/* ── Input bridge: bridge /dev/ttyGS1 to /dev/fast_input ── */
static int s3_input_bridge_fn(void *data) {
    struct file *ser_fp = ERR_PTR(-ENODEV);
    unsigned char stream_buf[1024];
    int stream_len = 0;
    int n;
    int retries = 0;
    int idle_count = 0;
    bool reopen_hangup = false;
    unsigned int pps_count = 0;
    unsigned long last_jiffies = jiffies;
    struct sched_param param = { .sched_priority = MAX_RT_PRIO - 10 };

    printk(KERN_INFO "stage3: input bridge starting (Real-Time priority 90)...\n");

    /* Elevate to Real-Time priority (SCHED_FIFO) */
    sched_setscheduler(current, SCHED_FIFO, &param);

    while (!kthread_should_stop()) {
        /* Persistent Latch: Open once and stay open until fatal error or stop */
        ser_fp = ERR_PTR(-ENODEV);
        retries = 0;
        while (!kthread_should_stop() && retries < 50) {
            ser_fp = filp_open("/dev/ttyGS1", O_RDONLY | O_NOCTTY | O_NONBLOCK, 0);
            if (!IS_ERR(ser_fp))
                break;
            s3_input_diagnostics_open(false, PTR_ERR(ser_fp), -EINPROGRESS);
            retries++;
            msleep(200);
        }
        if (kthread_should_stop()) {
            if (!IS_ERR(ser_fp))
                filp_close(ser_fp, NULL);
            goto out;
        }
        if (IS_ERR(ser_fp)) {
            printk(KERN_ERR "stage3: bridge failed to open /dev/ttyGS1: %ld\n", PTR_ERR(ser_fp));
            goto out;
        }

        /* N_TTY remains the line discipline; raw flags disable its character
         * processing. Its read path can still wait for tty workqueue drainage. */
        {
            mm_segment_t oldfs = get_fs();
            struct termios raw_termios;
            int raw_result = -ENOTTY;
            memset(&raw_termios, 0, sizeof(raw_termios));
            raw_termios.c_cflag = CS8 | CREAD | CLOCAL | B921600;
            raw_termios.c_cc[VMIN] = 1;
            set_fs(KERNEL_DS);
            if (ser_fp->f_op && ser_fp->f_op->unlocked_ioctl)
                raw_result = ser_fp->f_op->unlocked_ioctl(ser_fp, TCSETS,
                    (unsigned long)&raw_termios);
            set_fs(oldfs);
            s3_input_diagnostics_open(true, 0, raw_result);
            if (raw_result) {
                printk(KERN_ERR "stage3: ttyGS1 raw termios failed: %d\n", raw_result);
                filp_close(ser_fp, NULL);
                s3_input_diagnostics_open(false, 0, raw_result);
                msleep(1000);
                continue;
            }
            /* Opening establishes gs_port.count before reconnect queues OUT
             * requests. Publish only after the raw ioctl has also succeeded. */
            complete_all(&bridge_tty_ready);
            printk(KERN_INFO "stage3: ttyGS1 open and raw termios verified\n");
        }

        printk(KERN_INFO "stage3: input highway bridge LATCHED to /dev/ttyGS1\n");
        pps_count = 0;
        last_jiffies = jiffies;
        stream_len = 0;
        idle_count = 0;
        reopen_hangup = false;

        while (!kthread_should_stop()) {
            loff_t pos = ser_fp->f_pos;
            /* Retry after setup publication even when the TTY never closes. */
            s3_input_bridge_latch();
            /* 1. Append new data to the residue buffer */
            s3_input_diagnostics_read_begin();
            n = kernel_read(ser_fp, pos, stream_buf + stream_len,
                            sizeof(stream_buf) - stream_len);
            s3_input_diagnostics_read_end(n);
            if (s3_input_read_should_reopen(ser_fp, n, &stream_len)) {
                reopen_hangup = true;
                printk(KERN_INFO "stage3: ttyGS1 hung up; reopening input descriptor\n");
                break;
            }

            if (n > 0) {
                ser_fp->f_pos += n;
                stream_len += n;
                idle_count = 0; /* Reset backoff — data is flowing */

                /* Drain frames while retaining incomplete stream residue. */
                pps_count += s3_input_bridge_drain(stream_buf, &stream_len);

                /* 4. Guard against buffer overflow */
                if (stream_len >= (int)sizeof(stream_buf) - 32) {
                    stream_len = 0; /* Forced reset on corruption */
                }

            } else if (n == 0 || n == -EAGAIN || n == -EINTR) {
                /* Adaptive sleep: when data is flowing, poll quickly.
                 * When idle (no host connected to ttyGS1), back off to 10ms
                 * to avoid eating an entire CPU core at RT priority.
                 * This is critical for MCPE which uses /dev/fast_input directly. */
                idle_count++;
                if (idle_count < 20) {
                    usleep_range(50, 100);    /* First 20 empty reads: 50μs (fast response) */
                } else if (idle_count < 100) {
                    usleep_range(1000, 2000); /* Next 80: 1ms (transition) */
                } else {
                    usleep_range(10000, 15000); /* Steady idle: 10ms (CPU-friendly) */
                }
            } else {
                printk(KERN_ERR "stage3: bridge fatal error on /dev/ttyGS1: %d\n", n);
                break;
            }
            /* Report PPS every second */
            if (time_after(jiffies, last_jiffies + HZ)) {
                if (pps_count > 0) {
                    printk(KERN_INFO "stage3: Input Highway PPS: %u\n", pps_count);
                }
                pps_count = 0;
                last_jiffies = jiffies;
            }

            cond_resched();
        }
        filp_close(ser_fp, NULL);
        s3_input_diagnostics_closed();
        printk(KERN_INFO "stage3: bridge disconnected from /dev/ttyGS1\n");
        /* A host/ACM reconfiguration makes the old descriptor unusable.
         * Recover promptly, without spinning on EOF or penalizing idle reads. */
        if (!kthread_should_stop())
            msleep(reopen_hangup ? 20 : 1000);
    }
out:
    s3_input_diagnostics_closed();
    mutex_lock(&worker_lock);
    if (bridge_thread == current)
      bridge_thread = NULL;
    mutex_unlock(&worker_lock);
    return 0;
}

static int s3_start_bridge(void) {
  struct task_struct *thread;

  if (active_acm_ports < 2)
    return -ENODEV;

  mutex_lock(&worker_lock);
  if (bridge_thread) {
    mutex_unlock(&worker_lock);
    return -EALREADY;
  }
  reinit_completion(&bridge_tty_ready);
  s3_input_diagnostics_reset();
  thread = kthread_create(s3_input_bridge_fn, NULL, "stage3_bridge");
  if (IS_ERR(thread)) {
    int ret = PTR_ERR(thread);
    mutex_unlock(&worker_lock);
    return ret;
  }
  bridge_thread = thread;
  wake_up_process(thread);
  mutex_unlock(&worker_lock);
  return 0;
}

/* ── Navigate to composite config from /dev/usb_data3 ── */

/*
 * From sieusb data.c + IDA decompilation of data_bind (sub_35E0):
 *
 *   inode->i_cdev == &usb_data.chrdev
 *   In data_bind: a2 = &usb_data.interface (struct usb_function*)
 *                 cdev_init(a2 + 352, &data_fops)
 *
 * Therefore chrdev is at usb_function + 352 bytes.
 * And usb_function.config gives us the usb_configuration.
 *
 * Verified offsets from IDA (sieusb.ko):
 *   func + 0:    usb_function.name
 *   func + 48:   usb_function.config  (→ usb_configuration*)
 *   func + 216:  source_ep
 *   func + 224:  sink_ep
 *   func + 232:  gbuf                 (struct gbuf)
 *   func + 304:  gbuf.flags           (gbuf + 72)
 *   func + 312:  gbuf.name[16]        (gbuf + 80)
 *   func + 336:  usb_data.flags       (FLAG_DATA_INTERFACE_ENABLED)
 *   func + 344:  devid
 *   func + 352:  chrdev (struct cdev)  ← i_cdev
 */
static struct usb_configuration *find_sieusb_config(void) {
  int dev_idx;
  char dev_path[32];

  for (dev_idx = 0; dev_idx <= 8; dev_idx++) {
    struct path usb_path;
    struct cdev *char_dev;
    char *func_base;
    void *cfg_ptr = NULL;
    void *cdev_ptr = NULL;
    void *list_next = NULL;
    void *list_prev = NULL;
    unsigned char next_iface = 0;

    snprintf(dev_path, sizeof(dev_path), "/dev/usb_data%d", dev_idx + 1);

    if (kern_path(dev_path, LOOKUP_FOLLOW, &usb_path) != 0)
      continue;

    if (!usb_path.dentry->d_inode) {
      path_put(&usb_path);
      continue;
    }

    /* Autoboot Disabled Fix: i_cdev is populated lazily on first open.
   * If vrhmd hasn't naturally opened it, force the kernel to bind it. */
    if (!usb_path.dentry->d_inode->i_cdev && is_autoboot_disabled()) {
      struct file *dummy = filp_open(dev_path, O_RDONLY | O_NONBLOCK, 0);
      if (!IS_ERR(dummy))
        filp_close(dummy, NULL);
    }

    if (!usb_path.dentry->d_inode->i_cdev) {
      path_put(&usb_path);
      continue;
    }

    char_dev = usb_path.dentry->d_inode->i_cdev;
    path_put(&usb_path);

    func_base = (char *)char_dev - CHRDEV_TO_FUNC_OFFSET;

    /* Read config at func+48 */
    probe_kernel_read(&cfg_ptr,
                      func_base + S3_USB_FUNCTION_CONFIG_OFFSET,
                      sizeof(cfg_ptr));
    /* Read list.next at func+176, list.prev at func+184 */
    probe_kernel_read(&list_next, func_base + 176, sizeof(list_next));
    probe_kernel_read(&list_prev, func_base + 184, sizeof(list_prev));

    printk(KERN_INFO "stage3: %s cdev=%px func=%px "
                     "config=%px list={%px,%px}\n",
           dev_path, char_dev, func_base, cfg_ptr, list_next, list_prev);

    if (!cfg_ptr)
      continue;

    /* Validate: read config->cdev and config->next_interface_id */
    probe_kernel_read(&cdev_ptr,
                      (char *)cfg_ptr + S3_CONFIG_CDEV_OFFSET,
                      sizeof(cdev_ptr));
    probe_kernel_read(&next_iface,
                      (char *)cfg_ptr + S3_CONFIG_NEXT_INTERFACE_ID_OFFSET,
                      sizeof(next_iface));

    printk(KERN_INFO "stage3:   → cdev=%px next_iface=%u\n", cdev_ptr,
           (unsigned)next_iface);

    /* Valid config has: cdev is a kernel heap ptr (ffffffc0...),
     * next_iface in range 1..16 */
    if (cdev_ptr && ((unsigned long)cdev_ptr >> 56) == 0xff &&
        next_iface >= 1 && next_iface <= MAX_CONFIG_INTERFACES) {
      printk(KERN_INFO "stage3: VALID config from %s\n", dev_path);
      s3_set_status("OK config=%px cdev=%px ifaces=%u (from %s)", cfg_ptr,
                    cdev_ptr, (unsigned)next_iface, dev_path);
      return (struct usb_configuration *)cfg_ptr;
    }
  }

  s3_set_status("ERR no valid config found in any usb_data device");
  return NULL;
}

/* ── Helper: resolve config → cdev → gadget pointer chain ──
 * This pattern was repeated 3 times; consolidated here. */
static struct usb_gadget *resolve_gadget(struct usb_configuration *cfg) {
  void *cdev_ptr = NULL;
  void *gadget_ptr = NULL;

  probe_kernel_read(&cdev_ptr,
                    (char *)cfg + S3_CONFIG_CDEV_OFFSET,
                    sizeof(cdev_ptr));
  if (cdev_ptr)
    probe_kernel_read(&gadget_ptr,
                      (char *)cdev_ptr +
                          offsetof(struct usb_composite_dev, gadget),
                      sizeof(gadget_ptr));
  return (struct usb_gadget *)gadget_ptr;
}

/*
 * The Sony configuration is live while Stage3 is inserted.  Do not use the
 * normal list_for_each_entry() helpers for diagnostics here: one bad link (or
 * a concurrent configuration transition) turns a harmless audit into a kernel
 * OOPS.  Walk a snapshot one link at a time with fault-tolerant reads and
 * validate each backlink before dereferencing the containing usb_function.
 */
static int s3_audit_function_list(struct usb_configuration *config) {
  struct list_head *head = &config->functions;
  struct list_head head_links;
  struct list_head links;
  struct list_head *position;
  struct list_head *previous = head;
  unsigned int count = 0;
  int ret;

  ret = probe_kernel_read(&head_links, head, sizeof(head_links));
  if (ret)
    return ret;
  position = head_links.next;

  printk(KERN_INFO "stage3: === Gadget Function Audit ===\n");
  while (position != head) {
    struct usb_function *function;
    const char *name_pointer = NULL;
    char name[32] = {0};

    if (!position || count >= SIEUSB_MAX_FUNCS + 2) {
      printk(KERN_ERR
             "stage3: invalid function list: node=%px count=%u\n",
             position, count);
      return -EUCLEAN;
    }
    ret = probe_kernel_read(&links, position, sizeof(links));
    if (ret || links.prev != previous) {
      printk(KERN_ERR
             "stage3: invalid function list link: node=%px prev=%px "
             "expected=%px read=%d\n",
             position, ret ? NULL : links.prev, previous, ret);
      return ret ? ret : -EUCLEAN;
    }

    function = list_entry(position, struct usb_function, list);
    ret = probe_kernel_read(&name_pointer, &function->name,
                            sizeof(name_pointer));
    if (ret)
      return ret;
    if (name_pointer) {
      ret = probe_kernel_read(name, name_pointer, sizeof(name) - 1);
      if (ret)
        strlcpy(name, "<unreadable>", sizeof(name));
    } else {
      strlcpy(name, "NULL", sizeof(name));
    }
    printk(KERN_INFO "stage3: function[%u] name=%s node=%px\n",
           count, name, position);

    previous = position;
    position = links.next;
    count++;
  }

  if (head_links.prev != previous) {
    printk(KERN_ERR
           "stage3: invalid function list tail: tail=%px expected=%px\n",
           head_links.prev, previous);
    return -EUCLEAN;
  }
  printk(KERN_INFO "stage3: function list audit complete (%u functions)\n",
         count);
  return 0;
}

static int s3_find_function_by_name(
    struct usb_configuration *config, const char *wanted,
    struct usb_function **result) {
  struct list_head *head = &config->functions;
  struct list_head head_links;
  struct list_head links;
  struct list_head *position;
  struct list_head *previous = head;
  unsigned int count = 0;
  int ret;

  if (!wanted || !result)
    return -EINVAL;
  *result = NULL;
  ret = probe_kernel_read(&head_links, head, sizeof(head_links));
  if (ret)
    return ret;
  position = head_links.next;

  while (position != head) {
    struct usb_function *function;
    const char *name_pointer = NULL;
    char name[32] = {0};

    if (!position || count >= SIEUSB_MAX_FUNCS + 2)
      return -EUCLEAN;
    ret = probe_kernel_read(&links, position, sizeof(links));
    if (ret || links.prev != previous)
      return ret ? ret : -EUCLEAN;
    function = list_entry(position, struct usb_function, list);
    ret = probe_kernel_read(
        &name_pointer, &function->name, sizeof(name_pointer));
    if (ret)
      return ret;
    if (name_pointer) {
      ret = probe_kernel_read(name, name_pointer, sizeof(name) - 1);
      if (ret)
        return ret;
    }
    if (!strcmp(name, wanted)) {
      *result = function;
      return 0;
    }
    previous = position;
    position = links.next;
    count++;
  }
  return -ENOENT;
}

static int s3_plan_double_eviction(void) {
  static const char * const names[S3_DOUBLE_EVICT_COUNT] = {
      "data8", "data9"};
  unsigned char next_interface_id = 0;
  unsigned int index;
  unsigned int occupied_tail = 0;
  int ret;

  memset(evicted_functions, 0, sizeof(evicted_functions));
  evicted_function_count = 0;
  evicted_original_next_interface_id = 0;
  ret = probe_kernel_read(
      &next_interface_id, &target_config->next_interface_id,
      sizeof(next_interface_id));
  if (ret)
    return ret;
  if (next_interface_id < S3_DOUBLE_EVICT_COUNT)
    return -ENOSPC;

  for (index = 0; index < S3_DOUBLE_EVICT_COUNT; index++) {
    struct usb_function *function = NULL;
    unsigned int interface_id;
    unsigned int matches = 0;

    ret = s3_find_function_by_name(
        target_config, names[index], &function);
    if (ret)
      return ret;
    for (interface_id = 0;
         interface_id < next_interface_id; interface_id++) {
      struct usb_function *mapped = NULL;
      ret = probe_kernel_read(
          &mapped, &target_config->interface[interface_id],
          sizeof(mapped));
      if (ret)
        return ret;
      if (mapped != function)
        continue;
      evicted_functions[index].interface_id =
          (unsigned char)interface_id;
      matches++;
    }
    if (matches != 1)
      return -EUCLEAN;
    evicted_functions[index].function = function;
  }

  if (evicted_functions[0].interface_id >
      evicted_functions[1].interface_id) {
    struct s3_evicted_function temporary = evicted_functions[0];
    evicted_functions[0] = evicted_functions[1];
    evicted_functions[1] = temporary;
  }
  for (index = 0; index < S3_DOUBLE_EVICT_COUNT; index++) {
    unsigned char expected =
        next_interface_id - S3_DOUBLE_EVICT_COUNT + index;
    if (evicted_functions[index].interface_id == expected)
      occupied_tail++;
  }
  if (occupied_tail != S3_DOUBLE_EVICT_COUNT)
    return -ERANGE;

  evicted_original_next_interface_id = next_interface_id;
  evicted_function_count = S3_DOUBLE_EVICT_COUNT;
  return 0;
}

static int s3_capture_evicted_endpoints(
    struct s3_evicted_function *entry) {
  struct usb_descriptor_header **descriptor;
  struct usb_gadget *gadget = resolve_gadget(target_config);

  if (!entry || !entry->function || !gadget ||
      !entry->function->fs_descriptors)
    return -ENODEV;
  entry->endpoint_count = 0;
  for (descriptor = entry->function->fs_descriptors;
       *descriptor; descriptor++) {
    struct usb_endpoint_descriptor *endpoint_descriptor;
    struct usb_ep *hardware_endpoint;

    if ((*descriptor)->bDescriptorType != USB_DT_ENDPOINT)
      continue;
    if (entry->endpoint_count >= S3_EVICT_MAX_EPS)
      return -E2BIG;
    endpoint_descriptor =
        (struct usb_endpoint_descriptor *)*descriptor;
    list_for_each_entry(
        hardware_endpoint, &gadget->ep_list, ep_list) {
      struct s3_evicted_endpoint *snapshot;

      if (hardware_endpoint->address !=
          endpoint_descriptor->bEndpointAddress)
        continue;
      snapshot = &entry->endpoints[entry->endpoint_count++];
      snapshot->endpoint = hardware_endpoint;
      snapshot->driver_data = hardware_endpoint->driver_data;
      snapshot->address = hardware_endpoint->address;
      snapshot->claimed = hardware_endpoint->claimed;
      break;
    }
  }
  return entry->endpoint_count ? 0 : -ENODEV;
}

static int s3_double_evict_sony_data(void) {
  unsigned char reclaimed_next;
  unsigned int index;
  int ret = s3_plan_double_eviction();

  if (ret) {
    printk(KERN_ERR
           "stage3: double-eviction preflight failed %d; "
           "Sony functions unchanged\n",
           ret);
    return ret;
  }

  for (index = 0; index < evicted_function_count; index++) {
    ret = s3_capture_evicted_endpoints(
        &evicted_functions[index]);
    if (ret) {
      printk(KERN_ERR
             "stage3: cannot snapshot endpoints for %s: %d\n",
             evicted_functions[index].function->name, ret);
      return ret;
    }
  }

  for (index = 0; index < evicted_function_count; index++) {
    struct s3_evicted_function *entry =
        &evicted_functions[index];
    unsigned int endpoint_index;

    printk(KERN_WARNING
           "stage3: double-evict removing %s at interface %u\n",
           entry->function->name, entry->interface_id);
    /*
     * Do not call usb_remove_function() here. Sony's data_unbind() reaches
     * device_destroy() on a corrupt live class klist and OOPSes in
     * klist_next(). Disable I/O, detach only the composite list/interface
     * ownership, and retain the bound cdev/function object for restoration.
     */
    if (entry->function->disable)
      entry->function->disable(entry->function);
    list_del_init(&entry->function->list);
    entry->function->config = NULL;
    target_config->interface[entry->interface_id] = NULL;
    for (endpoint_index = 0;
         endpoint_index < entry->endpoint_count;
         endpoint_index++)
      usb_ep_autoconfig_release(
          entry->endpoints[endpoint_index].endpoint);
    entry->removed = true;
  }
  reclaimed_next =
      evicted_original_next_interface_id - evicted_function_count;
  ret = probe_kernel_write(
      &target_config->next_interface_id, &reclaimed_next,
      sizeof(reclaimed_next));
  if (ret)
    return ret;

  printk(KERN_WARNING
         "stage3: double eviction enabled: data8/data9 removed, "
         "next_iface=%u; secondary ACM/controller bridge permitted\n",
         reclaimed_next);
  return 0;
}

static void s3_restore_evicted_sony_data(void) {
  unsigned int index;

  if (!target_config || !evicted_function_count)
    return;
  for (index = 0; index < evicted_function_count; index++) {
    struct s3_evicted_function *entry =
        &evicted_functions[index];
    unsigned char next_interface_id = 0;
    int ret;

    if (!entry->removed || !entry->function)
      continue;
    probe_kernel_read(
        &next_interface_id, &target_config->next_interface_id,
        sizeof(next_interface_id));
    if (next_interface_id != entry->interface_id) {
      printk(KERN_ERR
             "stage3: cannot restore %s: next_iface=%u expected=%u\n",
             entry->function->name, next_interface_id,
             entry->interface_id);
      continue;
    }
    {
      unsigned int endpoint_index;
      for (endpoint_index = 0;
           endpoint_index < entry->endpoint_count;
           endpoint_index++) {
        struct s3_evicted_endpoint *snapshot =
            &entry->endpoints[endpoint_index];
        snapshot->endpoint->address = snapshot->address;
        snapshot->endpoint->claimed = snapshot->claimed;
        snapshot->endpoint->driver_data = snapshot->driver_data;
      }
    }
    entry->function->config = target_config;
    list_add_tail(
        &entry->function->list, &target_config->functions);
    target_config->interface[entry->interface_id] =
        entry->function;
    target_config->next_interface_id++;
    entry->removed = false;
    ret = entry->function->set_alt(
        entry->function, entry->interface_id, 0);
    if (ret)
      printk(KERN_WARNING
             "stage3: restored %s but set_alt failed %d\n",
             entry->function->name, ret);
    else
      printk(KERN_INFO "stage3: restored Sony %s at interface %u\n",
             entry->function->name, entry->interface_id);
  }
  memset(evicted_functions, 0, sizeof(evicted_functions));
  evicted_function_count = 0;
  evicted_original_next_interface_id = 0;
}

static void s3_remove_acm_function(
    struct usb_function *function) {
  struct usb_descriptor_header **descriptor;
  struct usb_gadget *gadget;
  struct usb_ep *endpoints[3];
  unsigned int endpoint_count = 0;
  unsigned int i;

  if (!function || !function->fs_descriptors || !target_config)
    return;
  gadget = resolve_gadget(target_config);
  if (!gadget)
    return;

  for (descriptor = function->fs_descriptors;
       *descriptor; descriptor++) {
    struct usb_endpoint_descriptor *endpoint;
    struct usb_ep *hardware_endpoint;

    if ((*descriptor)->bDescriptorType != USB_DT_ENDPOINT)
      continue;
    endpoint = (struct usb_endpoint_descriptor *)*descriptor;
    list_for_each_entry(
        hardware_endpoint, &gadget->ep_list, ep_list) {
      if (hardware_endpoint->address !=
          endpoint->bEndpointAddress)
        continue;
      if (endpoint_count < ARRAY_SIZE(endpoints))
        endpoints[endpoint_count++] = hardware_endpoint;
      break;
    }
  }

  /*
   * disable/unbind must run while ep->driver_data still points at u_serial's
   * gs_port. usb_ep_autoconfig_release() clears driver_data, so releasing
   * before usb_remove_function() makes completion and request teardown
   * dereference invalid state.
   */
  usb_remove_function(target_config, function);
  for (i = 0; i < endpoint_count; i++) {
    usb_ep_autoconfig_release(endpoints[i]);
    /*
     * Linux 4.4's release helper leaves the dynamically assigned address
     * behind. Our Sony-endpoint preservation pass treats any non-zero address
     * as pre-existing, so a later Stage3 load would otherwise re-claim its own
     * former ACM endpoints and report -ENODEV from acm_bind().
     */
    endpoints[i]->address = 0;
  }
}

/* Module parameter: trigger USB re-enumeration after ACM injection */
static int usb_reset = 1;
module_param(usb_reset, int, 0);
MODULE_PARM_DESC(
    usb_reset,
    "1=trigger USB re-enumeration after ACM injection (default), 0=skip");

/*
 * A short grace period lets the insmod UMH return before EP0 disappears.
 * The former five-second diagnostic delay dominated native startup time.
 */
static unsigned int reset_delay_ms = 500;
module_param(reset_delay_ms, uint, 0);
MODULE_PARM_DESC(
    reset_delay_ms,
    "Delay before USB re-enumeration in milliseconds (default: 500)");

/* ── Nop set_alt stub: always return 0 (success) ── */
static int nop_set_alt(struct usb_function *f, unsigned intf, unsigned alt) {
  printk(KERN_INFO "stage3: nop_set_alt(%s, intf=%u, alt=%u)\n",
         f->name ? f->name : "?", intf, alt);
  return 0;
}

/* ── Nop disable stub: do nothing ── */
static void nop_disable(struct usb_function *f) {
  printk(KERN_INFO "stage3: nop_disable(%s)\n", f->name ? f->name : "?");
}

#include "stage3_platform.inc"

static int s3_activate_acm_ports(const char *reason) {
  unsigned char base_id = 0;
  int first_error = 0;
  int i;

  if (!target_config)
    return -ENODEV;
  if (probe_kernel_read(
          &base_id,
          (char *)target_config + S3_CONFIG_NEXT_INTERFACE_ID_OFFSET,
          1))
    return -EFAULT;

  for (i = 0; i < active_acm_ports; i++) {
    unsigned char ctrl_id =
        base_id - ((active_acm_ports - i) * 2);
    unsigned char data_id = ctrl_id + 1;
    int ret;

    if (!acm_func[i]) {
      if (!first_error)
        first_error = -ENODEV;
      continue;
    }
    printk(KERN_INFO
           "stage3: activating acm[%d] (%s) ctrl_id=%u data_id=%u\n",
           i, reason, ctrl_id, data_id);
    ret = acm_func[i]->set_alt(acm_func[i], ctrl_id, 0);
    if (!ret)
      ret = acm_func[i]->set_alt(acm_func[i], data_id, 0);
    if (ret) {
      printk(KERN_WARNING
             "stage3: acm[%d] activation (%s) failed %d\n",
             i, reason, ret);
      if (!first_error)
        first_error = ret;
    } else {
      printk(KERN_INFO
             "stage3: acm[%d] endpoints enabled (%s)\n",
             i, reason);
    }
  }
  return first_error;
}

static bool s3_wait_for_path(const char *path, unsigned int timeout_ms) {
  unsigned int elapsed = 0;

  while (!(current->flags & PF_KTHREAD) || !kthread_should_stop()) {
    struct path resolved;
    if (!kern_path(path, LOOKUP_FOLLOW, &resolved)) {
      path_put(&resolved);
      return true;
    }
    if (elapsed >= timeout_ms)
      break;
    msleep(25);
    elapsed += 25;
  }
  return false;
}

static bool s3_wait_for_configuration(
    struct usb_gadget *gadget, unsigned int timeout_ms) {
  unsigned int elapsed = 0;

  while (!kthread_should_stop()) {
    if (READ_ONCE(gadget->state) == USB_STATE_CONFIGURED)
      return true;
    if (elapsed >= timeout_ms)
      break;
    msleep(25);
    elapsed += 25;
  }
  return false;
}

/* ── Delayed USB reset thread ── */
static int s3_reset_fn(void *data) {
  int i;
  struct usb_gadget *gadget;

  /* Let insmod return before taking EP0 away from the host. */
  printk(KERN_INFO "stage3: USB re-enumeration in %u ms...\n",
         reset_delay_ms);
  if (reset_delay_ms)
    msleep(reset_delay_ms);

  if (kthread_should_stop()) {
    s3_set_status("ERR reset: worker stopped before re-enumeration");
    goto out;
  }

  gadget = target_gadget;
  if (!gadget)
    gadget = resolve_gadget(target_config);
  if (!gadget) {
    printk(KERN_WARNING "stage3: no gadget ptr, "
                        "can't force re-enumeration\n");
    s3_set_status("ERR reset: gadget pointer unavailable");
    goto out;
  }

  {

    /* Step 1: Patch sieusb's set_alt and disable handlers to nop stubs.
     *
     * After disconnect, SET_CONFIGURATION triggers set_alt for ALL
     * interfaces. sieusb's handlers fail without Sony host init.
     * By replacing them with nop stubs that return 0, set_config
     * succeeds and the host xHCI configures ALL endpoints — including
     * ACM's. ACM's real set_alt still runs, enabling gserial. */
    printk(KERN_INFO "stage3: patching sieusb set_alt/disable "
                     "handlers to nop stubs\n");

    for (i = 0; i < SIEUSB_MAX_FUNCS && i < MAX_CONFIG_INTERFACES; i++) {
      struct usb_function *f = target_config->interface[i];
      void *old_set_alt;
      void *old_disable;

      if (!f)
        break;
      /* Only patch sieusb functions (not our ACM) */
      if (f == acm_func[0] || f == acm_func[1])
        continue;

      if (sony_function_patch_count >=
          ARRAY_SIZE(sony_function_patches)) {
        printk(KERN_WARNING
               "stage3: Sony callback patch table full at interface %d\n", i);
        break;
      }
      old_set_alt = f->set_alt;
      old_disable = f->disable;
      sony_function_patches[sony_function_patch_count].function = f;
      sony_function_patches[sony_function_patch_count].set_alt =
          f->set_alt;
      sony_function_patches[sony_function_patch_count].disable =
          f->disable;
      sony_function_patch_count++;
      f->set_alt = nop_set_alt;
      f->disable = nop_disable;
      printk(KERN_INFO "stage3:   stubbed [%d] %s "
                       "set_alt=%px→nop disable=%px→nop\n",
             i, f->name ? f->name : "?", old_set_alt, old_disable);

      /* ── Anti-Spam Patch ──
       * MacOS strictly claims HID interfaces (class 0x03) and probes
       * them after re-enumeration. Since sieusb disable is NOP'd,
       * the headset auth sequences remain out-of-sync, causing relentless
       * dmesg log spam that starves the CPU and ruins exploit timings!
       * Fix: mutate the HID descriptor into Vendor-Specific (0xFF),
       * preventing the native Mac driver from ever touching it. */
      s3_cloak_hid_descriptors(f, f->fs_descriptors, "fs");
      s3_cloak_hid_descriptors(f, f->hs_descriptors, "hs");
      s3_cloak_hid_descriptors(f, f->ss_descriptors, "ss");
    }

    /* Step 3: Soft USB disconnect */
    printk(KERN_INFO "stage3: disconnecting USB gadget=%px\n", gadget);
    usb_gadget_disconnect(gadget);
    msleep(1000);

    /* Step 4: CRITICAL — Spawn shell BEFORE USB reconnect.
     *
     * ROOT CAUSE: gserial_connect() (called during SET_CONFIGURATION)
     * only calls gs_start_io() — which queues USB bulk I/O — when
     * port.count > 0 (someone has ttyGS0 open). If the shell opens
     * ttyGS0 AFTER SET_CONFIGURATION, gs_open calls gs_start_io but
     * usb_ep_queue fails, gs_open ignores the error, and data never
     * flows. This caused the original 70% failure rate.
     *
     * FIX: Launch the shell NOW (while USB is disconnected). The shell
     * opens ttyGS0, setting port.count = 1. port_usb is NULL, so
     * gs_open correctly skips gs_start_io. When we reconnect and the
     * host sends SET_CONFIGURATION, gserial_connect sees port.count=1
     * and calls gs_start_io with freshly usb_ep_enable'd endpoints.
     * This is the ONLY reliable code path in u_serial.c. */
    printk(KERN_INFO "stage3: spawning shell AND bridge BEFORE USB reconnect "
                     "(ttyGS0/1 will open with port_usb=NULL)\n");
    {
      int shell_ret = s3_start_shell();
      int bridge_ret = s3_start_bridge();
      if (shell_ret && shell_ret != -EALREADY)
        printk(KERN_WARNING "stage3: shell launch failed %d\n", shell_ret);
      if (bridge_ret && bridge_ret != -EALREADY &&
          bridge_ret != -ENODEV)
        printk(KERN_WARNING "stage3: bridge launch failed %d\n", bridge_ret);
    }

    /*
     * The child writes this marker only after its ttyGS0 redirections
     * have opened. The separate parent-written PID marker is for teardown.
     * Wait on that concrete readiness
     * signal instead of always sleeping three seconds.
     */
    if (!s3_wait_for_path("/tmp/.stage3_shell_ready", 3000))
      printk(KERN_WARNING
             "stage3: shell tty readiness marker timed out; "
             "continuing with bounded legacy timing\n");

    /* The second tty must be open as well: gserial_connect only starts its
     * OUT request queue when gs_port.count is nonzero. A published kthread
     * alone does not establish that ordering. Keep recovery bounded. */
    if (active_acm_ports > 1 &&
        !wait_for_completion_timeout(&bridge_tty_ready, msecs_to_jiffies(3000))) {
      printk(KERN_ERR "stage3: ttyGS1 readiness timed out before reconnect; "
                      "control port recovery continues\n");
      s3_input_bridge_status();
    }
    if (kthread_should_stop())
      goto out;

    /* Step 5: Reconnect — host re-enumerates, SET_CONFIGURATION
     * triggers gserial_connect which sees port.count > 0 and
     * calls gs_start_io → USB I/O starts! */
    printk(KERN_INFO "stage3: reconnecting USB (shell has ttyGS0 "
                     "open, port.count should be >0)\n");
    /* NOTE: The intermittent bulk endpoint stall with DP Alt Mode at
     * SuperSpeed is a macOS xHCI timing issue, NOT a speed negotiation
     * problem. Evidence: the DWC3 DCFG register scan found a false
     * positive (a config struct, not MMIO) and the write was a no-op,
     * yet the shell worked anyway at speed=5 (SS). On other runs, the
     * same SS setup fails. The real fix is host-side serial port
     * reopen/retry in display_ctl.py.
     *
     * We previously set max_speed to HIGH here, but that breaks Linux
     * xHCI hosts which strictly validate descriptor lengths against the
     * physical link speed (expecting SS companion descriptors).
     * Leaving it at its default (USB_SPEED_SUPER) fixes Linux error -71. */
    if (!gadget_max_speed_changed) {
      original_gadget_max_speed = gadget->max_speed;
      gadget_max_speed_changed = true;
    }
    gadget->max_speed = USB_SPEED_SUPER;
    usb_gadget_connect(gadget);
    /*
     * macOS can enumerate the ACM descriptors without issuing the complete
     * SET_CONFIGURATION sequence through interface 14.  In that case
     * acm_disable() from the disconnect left all three endpoints disabled and
     * the tty exists on the host but carries no traffic.  Re-activate after
     * enumeration; acm_set_alt() is explicitly reset-safe and reconnects
     * gserial with the already-open ttyGS0 port.
     */
    /*
     * Activate as soon as the host reaches SET_CONFIGURATION rather than
     * imposing a fixed two-second delay on every successful enumeration.
     */
    if (!s3_wait_for_configuration(gadget, 2000))
      printk(KERN_WARNING
             "stage3: host configuration wait timed out; "
             "attempting explicit ACM activation\n");
    if (!kthread_should_stop()) {
      int activate_ret =
          s3_activate_acm_ports("post-reconnect");
      if (activate_ret) {
        s3_set_status("ERR post-reconnect ACM activation %d",
                      activate_ret);
      } else {
        unsigned char final_iface = 0;
        probe_kernel_read(
            &final_iface,
            (char *)target_config + S3_CONFIG_NEXT_INTERFACE_ID_OFFSET,
            sizeof(final_iface));
        s3_set_status(
            "OK acm x%d active after USB reset iface_next=%u%s",
            active_acm_ports, (unsigned)final_iface,
            double_evict ? " double-evict" : "");
      }
    }
  }
out:
  mutex_lock(&worker_lock);
  if (reset_thread == current)
    reset_thread = NULL;
  mutex_unlock(&worker_lock);
  return 0;
}

static int s3_start_reset(void) {
  struct task_struct *thread;

  mutex_lock(&worker_lock);
  if (reset_thread) {
    mutex_unlock(&worker_lock);
    return -EALREADY;
  }
  thread = kthread_create(s3_reset_fn, NULL, "stage3_reset");
  if (IS_ERR(thread)) {
    int ret = PTR_ERR(thread);
    mutex_unlock(&worker_lock);
    return ret;
  }
  reset_thread = thread;
  wake_up_process(thread);
  mutex_unlock(&worker_lock);
  return 0;
}

/* ── Module init ── */
static int __init stage3_init(void) {
  int i, ret;
  int num_acm_ports = double_evict ? 2 : 1;

  if (!noevict && !double_evict) {
    printk(KERN_ERR
           "stage3: noevict=0 refused: live Sony function eviction "
           "requires the explicit double_evict=1 opt-in\n");
    return -EOPNOTSUPP;
  }
  if (double_evict)
    printk(KERN_WARNING
           "stage3: DOUBLE EVICTION OPT-IN active; data8/data9 "
           "will be removed for the secondary ACM port\n");

  /* Log boot mode detection status for diagnostics */
  is_autoboot_disabled();

  printk(KERN_INFO "stage3: module loading... (elegant reboot via /proc/stage3 "
                   "supported!)\n");
  s3_set_status("INIT");
  active_acm_ports = num_acm_ports;

  s3_proc = proc_create("stage3", 0600, NULL, &s3_proc_fops);
  if (!s3_proc)
    return -ENOMEM;

  /* Step 0: Defuse Sony's panic-on-userspace-crash kill chain.
   *
   * Sony's kernel has CONFIG_SIE_CRASH_DUMP_SUPPORT_FOR_USER=y which
   * calls die() from do_coredump(). Combined with CONFIG_PANIC_ON_OOPS=y,
   * ANY userspace crash (VrtSlam, vrhmd, etc.) escalates to a full
   * kernel panic. After USB re-enumeration, VrtSlam often crashes because
   * the sensor pipeline is not properly connected, killing the system.
   *
   * Fix: set panic_on_oops=0 so userspace crashes just SIGKILL the
   * faulting process. Also set panic_timeout=10 as a safety net so
   * if something else does trigger a kernel panic, it reboots instead
   * of hanging forever (Sony sets panic_timeout=-1). */
  {
    panic_on_oops_ptr =
        (int *)kallsyms_lookup_name("panic_on_oops");
    panic_timeout_ptr =
        (int *)kallsyms_lookup_name("panic_timeout");

    if (panic_on_oops_ptr) {
      printk(KERN_INFO "stage3: panic_on_oops %d -> 0 "
                       "(userspace crashes won't kill kernel)\n",
             *panic_on_oops_ptr);
      original_panic_on_oops = *panic_on_oops_ptr;
      *panic_on_oops_ptr = 0;
      panic_state_changed = true;
    } else {
      printk(KERN_WARNING "stage3: panic_on_oops symbol not found\n");
    }

    if (panic_timeout_ptr) {
      printk(KERN_INFO "stage3: panic_timeout %d -> 10 "
                       "(reboot after 10s on panic instead of hang)\n",
             *panic_timeout_ptr);
      original_panic_timeout = *panic_timeout_ptr;
      *panic_timeout_ptr = 10;
      panic_state_changed = true;
    } else {
      printk(KERN_WARNING "stage3: panic_timeout symbol not found\n");
    }
  }

  /* Step 1: Navigate to sieusb's composite configuration */
  s3_set_status("FINDING config");
  target_config = find_sieusb_config();
  if (!target_config) {
    /* status already set by find_sieusb_config */
    goto fail_proc;
  }

  {
    unsigned char next_iface = 0;
    struct usb_gadget *gadget = resolve_gadget(target_config);

    target_gadget = gadget;
    if (!target_gadget) {
      s3_set_status("ERR config has no gadget pointer");
      goto fail_proc;
    }

    probe_kernel_read(
                      &next_iface,
                      (char *)target_config +
                          S3_CONFIG_NEXT_INTERFACE_ID_OFFSET,
                      sizeof(next_iface));

    printk(KERN_INFO "stage3: config found, next_iface=%u "
                     "(MAX=%d) gadget=%px\n",
           (unsigned)next_iface, MAX_CONFIG_INTERFACES, gadget);

    /* Diagnostic-only mode: just verify navigation and exit */
    if (diag_only) {
      s3_set_status("DIAG OK config=%px next_iface=%u gadget=%px",
                    target_config, (unsigned)next_iface, gadget);
      printk(KERN_INFO "stage3: DIAG ONLY mode — "
                       "not injecting ACM\n");
      s3_restore_platform_mutations();
      return 0;
    }

    if (next_iface >= MAX_CONFIG_INTERFACES) {
      s3_set_status("ERR no free interface slots (%u/%d)", (unsigned)next_iface,
                    MAX_CONFIG_INTERFACES);
      goto fail_proc;
    }
    /* ── Diagnostic Audit of existing functions ── */
    ret = s3_audit_function_list(target_config);
    if (ret) {
      s3_set_status("ERR invalid function list %d", ret);
      goto fail_proc;
    }
  }

  {
    if (!double_evict) {
      printk(KERN_INFO "stage3: noevict=1 — using single ACM port (shell only). "
                       "All Sony data endpoints preserved.\n");
    } else {
      printk(KERN_WARNING
             "stage3: double_evict=1 overrides noevict and enables "
             "two ACM ports (shell + controller bridge)\n");
    }

    /* Step 2: Allocate ACM function instances from factory */
    s3_set_status("ALLOCATING acm x%d", num_acm_ports);
    for (i = 0; i < num_acm_ports; i++) {
      printk(KERN_INFO "stage3: calling usb_get_function_instance(acm) for port %d\n", i);
      acm_inst[i] = usb_get_function_instance("acm");
      if (IS_ERR(acm_inst[i])) {
        s3_set_status("ERR get_function_instance(acm:%d) %ld", i, PTR_ERR(acm_inst[i]));
        acm_inst[i] = NULL;
        goto fail_inst;
      }
    }

    /* Step 3: Create ACM functions from instances */
    s3_set_status("CREATING acm functions");
    for (i = 0; i < num_acm_ports; i++) {
      printk(KERN_INFO "stage3: calling usb_get_function(acm_inst[%d])\n", i);
      acm_func[i] = usb_get_function(acm_inst[i]);
      if (IS_ERR(acm_func[i])) {
        s3_set_status("ERR get_function(acm:%d) %ld", i, PTR_ERR(acm_func[i]));
        acm_func[i] = NULL;
        goto fail_func;
      }
    }
  }

  /* Step 3b: Re-claim endpoints used by existing sieusb functions.
   *
   * ROOT CAUSE: composite_disconnect() calls usb_ep_autoconfig_reset()
   * which clears the "claimed" flag on ALL hardware endpoints. When ACM's
   * usb_ep_autoconfig() runs inside usb_add_function(), it sees all EPs
   * as unclaimed and grabs ep1in/ep1out — which overlap with sieusb's
   * auth function. On SET_CONFIGURATION both try to enable the same
   * physical EPs → conflict → macOS rejects the device.
   *
   * Fix: any EP with a non-zero address was previously assigned by
   * usb_ep_autoconfig during sieusb's initial bind. Claim ALL of them
   * so ACM gets genuinely free endpoints. */
  {
    struct usb_gadget *gadget = resolve_gadget(target_config);
    int reclaimed = 0;

    if (gadget) {
      struct usb_ep *hw_ep;

      list_for_each_entry(hw_ep, &gadget->ep_list, ep_list) {
        if (hw_ep->address && !hw_ep->claimed) {
          if (ep_claim_patch_count >= ARRAY_SIZE(ep_claim_patches)) {
            printk(KERN_WARNING
                   "stage3: endpoint claim table full at %s\n",
                   hw_ep->name);
            continue;
          }
          ep_claim_patches[ep_claim_patch_count].ep = hw_ep;
          ep_claim_patches[ep_claim_patch_count].claimed =
              hw_ep->claimed;
          ep_claim_patch_count++;
          hw_ep->claimed = true;
          printk(KERN_INFO "stage3:   re-claimed EP %s "
                           "addr=0x%02x\n",
                 hw_ep->name, hw_ep->address);
          reclaimed++;
        }
      }
      printk(KERN_INFO "stage3: re-claimed %d sieusb EPs "
                       "before ACM bind\n",
             reclaimed);
    }
  }

  if (double_evict) {
    ret = s3_double_evict_sony_data();
    if (ret) {
      s3_set_status("ERR double eviction preflight/remove %d", ret);
      goto fail_func;
    }
  }

  probe_kernel_read(
      &original_acm_next_interface_id,
      &target_config->next_interface_id,
      sizeof(original_acm_next_interface_id));
  acm_interface_state_saved = true;

  s3_set_status("ADDING acm to config");
  {
    struct usb_gadget *gadget = resolve_gadget(target_config);
    const struct usb_gadget_ops *ops_orig = NULL;
    struct usb_gadget_ops *ops_copy = NULL;

    if (gadget) {
      probe_kernel_read(&ops_orig,
                        (char *)gadget + offsetof(struct usb_gadget, ops),
                        sizeof(ops_orig));

      if (ops_orig) {
        ops_copy = kmalloc(sizeof(*ops_copy), GFP_KERNEL);
        if (ops_copy) {
          memcpy(ops_copy, ops_orig, sizeof(*ops_copy));
          ops_copy->match_ep = NULL;
          /* Swap in our patched ops table */
          probe_kernel_write((char *)gadget + offsetof(struct usb_gadget, ops),
                             &ops_copy, sizeof(ops_copy));
          printk(KERN_INFO "stage3: disabled match_ep "
                           "for ACM bind (ops %px → %px)\n",
                 ops_orig, ops_copy);
        }
      }
    }

    for (i = 0; i < num_acm_ports; i++) {
      unsigned char current_iface = 0;
      probe_kernel_read(
          &current_iface,
          (char *)target_config + S3_CONFIG_NEXT_INTERFACE_ID_OFFSET,
          sizeof(current_iface));

      printk(KERN_INFO "stage3: pre-inject port %d: next_iface=%u "
                       "(need 2 slots, free up to %d)\n",
             i, (unsigned)current_iface, MAX_CONFIG_INTERFACES - 1);

      printk(KERN_INFO "stage3: calling usb_add_function"
                       "(config=%px, func=%px [%d])\n",
             target_config, acm_func[i], i);
      ret = usb_add_function(target_config, acm_func[i]);
      if (ret) {
         printk(KERN_ERR "stage3: usb_add_function failed for port %d: %d\n", i, ret);
         break;
      }
    }
    printk(KERN_INFO "stage3: usb_add_function loop finished with ret=%d\n", ret);

    /* Restore original ops table */
    if (gadget && ops_copy) {
      probe_kernel_write((char *)gadget + offsetof(struct usb_gadget, ops),
                         &ops_orig, sizeof(ops_orig));
      kfree(ops_copy);
      printk(KERN_INFO "stage3: restored original ops %px\n", ops_orig);
    }
  }
  if (ret) {
    int j;
    s3_set_status("ERR add_function %d (exhaustion?)", ret);
    /* Clean rollback: remove any successfully bound functions from this set */
    for (j = 0; j < num_acm_ports; j++) {
       if (acm_func[j] && acm_func[j]->config == target_config) {
           s3_remove_acm_function(acm_func[j]);
       }
    }
    goto fail_func;
  }

  {
    unsigned char post_iface = 0;
    unsigned char ss_flag = 0;

    probe_kernel_read(
                      &post_iface,
                      (char *)target_config +
                          S3_CONFIG_NEXT_INTERFACE_ID_OFFSET,
                      sizeof(post_iface));
    probe_kernel_read(&ss_flag, (char *)target_config + 89, sizeof(ss_flag));
    printk(KERN_INFO "stage3: ACM injected! next_iface now=%u "
                     "ss_flag=0x%02x\n",
           (unsigned)post_iface, (unsigned)ss_flag);
    printk(KERN_INFO "stage3: acm_func[0] descriptors: "
                     "fs=%px hs=%px ss=%px ssp=%px\n",
           acm_func[0]->fs_descriptors, acm_func[0]->hs_descriptors,
           acm_func[0]->ss_descriptors, acm_func[0]->ssp_descriptors);
    printk(KERN_INFO "stage3: acm_func[0]->config=%px "
                     "acm_func[0]->list={%px,%px}\n",
           acm_func[0]->config, acm_func[0]->list.next, acm_func[0]->list.prev);

#ifdef STAGE3_VERBOSE_DIAG
    {
      int i;
      /* ── DIAG A: Dump config->interface[] array ──
       * set_config() iterates this and BREAKS on first NULL.
       * If there's a NULL gap before our ACM interfaces (13,14),
       * set_alt() is never called for ACM → endpoints never enabled. */
      printk(KERN_INFO "stage3: === config->interface[] dump ===\n");
      for (i = 0; i < MAX_CONFIG_INTERFACES; i++) {
        void *intf_func = NULL;
        /* config->interface is at offset 96 in usb_configuration
         * (after next_interface_id at 88 + speed flags) */
        probe_kernel_read(&intf_func,
                          (char *)target_config +
                              offsetof(struct usb_configuration, interface) +
                              i * sizeof(void *),
                          sizeof(intf_func));
        if (intf_func) {
          char fname[32] = {0};
          void *name_ptr = NULL;
          /* usb_function->name is at offset 0 */
          probe_kernel_read(&name_ptr, intf_func, sizeof(name_ptr));
          if (name_ptr)
            probe_kernel_read(fname, name_ptr, sizeof(fname) - 1);
          printk(KERN_INFO "stage3:   interface[%2d] = %px  \"%s\"\n", i,
                 intf_func, fname);
        } else {
          printk(KERN_INFO "stage3:   interface[%2d] = NULL%s\n", i,
                 (i < post_iface) ? " *** GAP ***" : "");
        }
      }

      /* ── DIAG B: Dump gadget endpoint list ──
       * Shows all hardware EPs, which are claimed, and addresses.
       * ACM needs 3 unclaimed EPs: bulk IN, bulk OUT, int IN. */
      {
        struct usb_gadget *diag_gadget = resolve_gadget(target_config);
        if (diag_gadget) {
          struct list_head ep_list_head;
          void *pos;
          int ep_count = 0;
          unsigned char in_epnum = 0, out_epnum = 0;

          /* Read gadget->in_epnum and out_epnum for autoconfig state */
          probe_kernel_read(&in_epnum,
                            (char *)diag_gadget +
                                offsetof(struct usb_gadget, in_epnum),
                            sizeof(in_epnum));
          probe_kernel_read(&out_epnum,
                            (char *)diag_gadget +
                                offsetof(struct usb_gadget, out_epnum),
                            sizeof(out_epnum));
          printk(KERN_INFO "stage3: === gadget EP list "
                           "(in_epnum=%u out_epnum=%u) ===\n",
                 (unsigned)in_epnum, (unsigned)out_epnum);

          /* Walk gadget->ep_list (linked list of struct usb_ep) */
          probe_kernel_read(&ep_list_head,
                            (char *)diag_gadget +
                                offsetof(struct usb_gadget, ep_list),
                            sizeof(ep_list_head));

          pos = ep_list_head.next;
          while (pos &&
                 pos != (void *)((char *)diag_gadget +
                                 offsetof(struct usb_gadget, ep_list)) &&
                 ep_count < 40) {
            /* pos points to the ep_list member INSIDE usb_ep
             * (at offset 24). Subtract to get usb_ep base ptr.
             * struct usb_ep layout (ARM64):
             *   0: driver_data (ptr)
             *   8: name (ptr)
             *  16: ops (ptr)
             *  24: ep_list (list_head, 16 bytes)
             *  40: caps (4 bytes)
             *  44: claimed (bool) — NO already_seen in this kernel
             *  45: enabled (bool)
             *  48: maxpacket:16, maxpacket_limit:16 (bitfield)
             *  52: max_streams:16, mult:2, maxburst:5 (bitfield)
             *  56: address (u8)
             */
            char *ep_base = (char *)pos - offsetof(struct usb_ep, ep_list);
            char ep_name[16] = {0};
            void *name_ptr = NULL;
            unsigned char ep_addr = 0;
            unsigned char ep_claimed = 0;

            probe_kernel_read(&name_ptr, ep_base + 8, sizeof(name_ptr));
            if (name_ptr)
              probe_kernel_read(ep_name, name_ptr, sizeof(ep_name) - 1);
            probe_kernel_read(&ep_addr,
                              ep_base + offsetof(struct usb_ep, address), 1);
            probe_kernel_read(&ep_claimed, ep_base + 44, 1);

            printk(KERN_INFO "stage3:   EP %-10s addr=0x%02x "
                             "claimed=%d\n",
                   ep_name, ep_addr, ep_claimed);

            /* Advance: read ep_list.next at pos (which IS ep_list) */
            {
              struct list_head next_link;
              probe_kernel_read(&next_link, pos, sizeof(next_link));
              pos = next_link.next;
            }
            ep_count++;
          }
          printk(KERN_INFO "stage3: total EPs enumerated: %d\n", ep_count);
        }
      }

      /* ── DIAG C: Dump ACM's allocated endpoint addresses ──
       * f_acm struct: port.in at +200, port.out at +208,
       * notify at +248 (from struct f_acm in f_acm.c)
       * Actually these are offsets within usb_function which is
       * embedded via gserial.func. Let's read from the descriptors. */
      if (acm_func[0]->fs_descriptors) {
        int d;
        printk(KERN_INFO "stage3: === ACM FS descriptors ===\n");
        for (d = 0; d < 15; d++) {
          void *desc_ptr = NULL;
          unsigned char bLen = 0, bType = 0;

          probe_kernel_read(&desc_ptr, (void **)acm_func[0]->fs_descriptors + d,
                            sizeof(desc_ptr));
          if (!desc_ptr)
            break;

          probe_kernel_read(&bLen, desc_ptr, 1);
          probe_kernel_read(&bType, (char *)desc_ptr + 1, 1);

          if (bType == USB_DT_ENDPOINT) {
            unsigned char bAddr = 0;
            unsigned char bmAttr = 0;
            unsigned short wMaxPkt = 0;
            probe_kernel_read(&bAddr, (char *)desc_ptr + 2, 1);
            probe_kernel_read(&bmAttr, (char *)desc_ptr + 3, 1);
            probe_kernel_read(&wMaxPkt, (char *)desc_ptr + 4, 2);
            printk(KERN_INFO "stage3:   EP desc: "
                             "addr=0x%02x attr=0x%02x maxpkt=%u "
                             "(%s %s)\n",
                   bAddr, bmAttr, wMaxPkt, (bAddr & 0x80) ? "IN" : "OUT",
                   (bmAttr & 3) == 2   ? "BULK"
                   : (bmAttr & 3) == 3 ? "INT"
                                       : "?");
          } else if (bType == USB_DT_INTERFACE) {
            unsigned char bIfNum = 0, bNumEP = 0, bClass = 0;
            probe_kernel_read(&bIfNum, (char *)desc_ptr + 2, 1);
            probe_kernel_read(&bNumEP, (char *)desc_ptr + 4, 1);
            probe_kernel_read(&bClass, (char *)desc_ptr + 5, 1);
            printk(KERN_INFO "stage3:   IF desc: "
                             "num=%u numEP=%u class=0x%02x\n",
                   bIfNum, bNumEP, bClass);
          } else if (bType == USB_DT_INTERFACE_ASSOCIATION) {
            unsigned char bFirst = 0, bCount = 0, bFnClass = 0;
            probe_kernel_read(&bFirst, (char *)desc_ptr + 2, 1);
            probe_kernel_read(&bCount, (char *)desc_ptr + 3, 1);
            probe_kernel_read(&bFnClass, (char *)desc_ptr + 4, 1);
            printk(KERN_INFO "stage3:   IAD desc: "
                             "first=%u count=%u class=0x%02x\n",
                   bFirst, bCount, bFnClass);
          }
        }
      }

      /* ── DIAG D: Walk config->functions list ──
       * Print all registered function names to confirm ACM is in list */
      {
        struct list_head func_list_head;
        void *fpos;
        int fcount = 0;
        probe_kernel_read(&func_list_head,
                          (char *)target_config +
                              offsetof(struct usb_configuration, functions),
                          sizeof(func_list_head));

        fpos = func_list_head.next;
        printk(KERN_INFO "stage3: === config->functions list ===\n");
        while (fpos &&
               fpos !=
                   (void *)((char *)target_config +
                            offsetof(struct usb_configuration, functions)) &&
               fcount < 20) {
          /* usb_function.list is at offset 168 (offsetof list field)
           * so usb_function* = list_entry - offset_of(list)
           * list is at offsetof(struct usb_function, list) */
          char *func_ptr = (char *)fpos - offsetof(struct usb_function, list);
          void *name_ptr = NULL;
          char fname[32] = {0};

          probe_kernel_read(&name_ptr, func_ptr, sizeof(name_ptr));
          if (name_ptr)
            probe_kernel_read(fname, name_ptr, sizeof(fname) - 1);
          printk(KERN_INFO "stage3:   func[%2d] %px \"%s\"\n", fcount, func_ptr,
                 fname);

          /* Advance via list.next */
          {
            struct list_head fl;
            probe_kernel_read(&fl, fpos, sizeof(fl));
            fpos = fl.next;
          }
          fcount++;
        }
        printk(KERN_INFO "stage3: total functions: %d\n", fcount);
      }
    }
#endif /* STAGE3_VERBOSE_DIAG */
  }
  /* Step 5b: Auto-resolve sieusb_ts_addr if not passed via module param.
   * Look up the sieusb module's 'ts' kernel_param and get the pointer to
   * its backing variable directly (avoids computing offset from module_core
   * which can be wrong if the kernel linker reorders sections). */
  if (!sieusb_ts_addr) {
    struct module *sieusb_mod;
    mutex_lock(&module_mutex);
    sieusb_mod = find_module("sieusb");
    if (sieusb_mod) {
      /* Iterate module's kernel params to find 'ts' */
      struct kernel_param *kp;
      unsigned int i;
      int found = 0;
      for (i = 0; i < sieusb_mod->num_kp; i++) {
        kp = &sieusb_mod->kp[i];
        if (kp->name && strcmp(kp->name, "ts") == 0) {
          sieusb_ts_addr = (unsigned long)kp->arg;
          printk(KERN_INFO "stage3: auto-resolved ts param "
                           "at 0x%lx (via kp->arg)\n",
                 sieusb_ts_addr);
          found = 1;
          break;
        }
      }
      if (!found) {
        printk(KERN_WARNING "stage3: sieusb 'ts' param not found via kparam, "
                            "attempting signature fallback...\n");

        /* Fingerprint sieusb version via Patch1 address signatures */
        if (*(u32 *)((unsigned long)sieusb_mod->module_core + 0x47c) ==
            0x540002a0) {
          sieusb_ts_addr = (unsigned long)sieusb_mod->module_core + 0x8550;
          printk(KERN_INFO "stage3: resolved fv01.10 ts at 0x%lx\n",
                 sieusb_ts_addr);
        } else if (*(u32 *)((unsigned long)sieusb_mod->module_core + 0x490) ==
                   0x540002e0) {
          sieusb_ts_addr = (unsigned long)sieusb_mod->module_core + 0x8670;
          printk(KERN_INFO "stage3: resolved fv06.00 ts at 0x%lx\n",
                 sieusb_ts_addr);
        } else {
          printk(KERN_WARNING
                 "stage3: sieusb version check failed, keepalive disabled\n");
        }
      }
    } else {
      printk(KERN_WARNING "stage3: sieusb module not found, "
                          "keepalive will be disabled\n");
    }
    mutex_unlock(&module_mutex);
  }

  /* Start keepalive — fake the host HID keepalive for vrhmd_main.
   * After USB re-enum the host's ctrl_transfer keepalive channel is gone.
   * We periodically write ktime_get_seconds() to sieusb's 'ts' variable
   * so vrhmd_main.elf thinks the host is still present. */
  /* Start keepalive thread unconditionally.
   * For v06.00+, sieusb_ts_addr is set and we update it.
   * For v01.10, sieusb_ts_addr is 0, so the thread falls back to memory
   * patching vrhmd_main. */
  keepalive_thread = kthread_run(s3_keepalive_fn, NULL, "stage3_keepalive");
  if (IS_ERR(keepalive_thread)) {
    printk(KERN_WARNING "stage3: keepalive thread failed %ld\n",
           PTR_ERR(keepalive_thread));
    keepalive_thread = NULL;
  } else {
    printk(KERN_INFO
           "stage3: keepalive active — autonomous memory patching enabled\n");
  }

  /* Step 6: Optionally trigger delayed USB re-enumeration.
   * This runs in a separate thread after a short insmod grace period.
   * The reset thread launches the shell before reconnecting USB. */
  {
    unsigned char final_iface = 0;
    probe_kernel_read(
                      &final_iface,
                      (char *)target_config +
                          S3_CONFIG_NEXT_INTERFACE_ID_OFFSET,
                      sizeof(final_iface));
    /* Publish before waking the worker so its ACTIVE/ERR result wins. */
    s3_set_status(
        "OK acm x%d injected iface_next=%u%s%s",
        num_acm_ports, (unsigned)final_iface,
        double_evict ? " double-evict" : "",
        usb_reset ? " (USB reset pending)" : "");
  }

  if (usb_reset) {
    ret = s3_start_reset();
    if (ret) {
      printk(KERN_WARNING "stage3: reset thread failed %d\n", ret);
      s3_set_status("ERR reset thread start %d", ret);
      return 0;
    }
  } else {
    /* Open tty ports before enabling endpoints: gserial_connect then queues
     * OUT requests through the same open-port path used during reconnect. */
    ret = s3_start_shell();
    if (ret && ret != -EALREADY)
      printk(KERN_WARNING "stage3: shell thread failed %d\n", ret);
    if (num_acm_ports > 1) {
      ret = s3_start_bridge();
      if (ret && ret != -EALREADY)
        printk(KERN_WARNING "stage3: bridge thread failed %d\n", ret);
      if (!wait_for_completion_timeout(&bridge_tty_ready, msecs_to_jiffies(3000))) {
        printk(KERN_ERR "stage3: ttyGS1 readiness timed out before initial activation\n");
        s3_input_bridge_status();
      }
    }
    if (!s3_wait_for_path("/tmp/.stage3_shell_ready", 3000))
      printk(KERN_WARNING "stage3: initial shell tty readiness marker timed out\n");
    ret = s3_activate_acm_ports("initial after tty open");
    if (ret)
      s3_set_status("ERR initial ACM activation %d", ret);
  }

  return 0;

fail_func:
  s3_restore_acm_interface_state();
  s3_restore_evicted_sony_data();
  for (i = 0; i < 2; i++) {
    if (acm_func[i]) {
      usb_put_function(acm_func[i]);
      acm_func[i] = NULL;
    }
  }
fail_inst:
  for (i = 0; i < 2; i++) {
    if (acm_inst[i]) {
      usb_put_function_instance(acm_inst[i]);
      acm_inst[i] = NULL;
    }
  }
fail_proc:
  s3_restore_platform_mutations();
  /* Keep /proc/stage3 alive so error status is readable via
   * "cat /proc/stage3" — essential for debugging on headless device.
   * Return 0 so module stays loaded and status is accessible. */
  printk(KERN_ERR "stage3: init failed, status: %s\n", status_buf);
  return 0;
}

#include "stage3_workers.inc"

/* ── Module exit ── */
static void stage3_exit(void) {
  int i;

  /* proc_remove drains active callbacks before their workers, ring, or ACM
   * objects can disappear. Do not hold callback locks while it waits. */
  if (s3_proc) {
    proc_remove(s3_proc);
    s3_proc = NULL;
  }

  s3_stop_worker(&reset_thread);

  if (keepalive_thread) {
    kthread_stop(keepalive_thread);
    keepalive_thread = NULL;
  }

  s3_stop_worker(&bridge_thread);
  s3_stop_worker(&shell_thread);

  s3_stop_shell_process();

  s3_fast_input_cleanup();
  s3_fast_stream_cleanup();

  for (i = 0; i < 2; i++) {
    if (acm_func[i] && target_config) {
      s3_remove_acm_function(acm_func[i]);
      usb_put_function(acm_func[i]);
      acm_func[i] = NULL;
    }
    if (acm_inst[i]) {
      usb_put_function_instance(acm_inst[i]);
      acm_inst[i] = NULL;
    }
  }
  s3_restore_acm_interface_state();
  s3_restore_evicted_sony_data();

  s3_restore_platform_mutations();

  target_config = NULL;
  target_gadget = NULL;
  active_acm_ports = 0;
  shell_started = false;
  shell_binary[0] = '\0';
  shell_binary_is_busybox = false;
}

module_init(stage3_init);
module_exit(stage3_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("PSVR2 Stage3 — USB ACM Serial Injection");
