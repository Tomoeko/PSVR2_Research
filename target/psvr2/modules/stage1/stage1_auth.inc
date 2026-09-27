/* Autoboot-disabled detection */
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

    printk(KERN_INFO "stage1: boot mode detection: fts=%d vrhmd=%s -> autoboot_disabled=%d\n",
           fts_detected, vrhmd_running ? "running" : "NOT running", disabled);

    return disabled;
}

#if defined(PSVR2_SOURCE_FAMILY_0110)
#define S1_SIEUSB_AUTH_BRANCH_OFFSET       0x474
#define S1_SIEUSB_SEQUENCE_BRANCH_OFFSET   0x530
#define S1_SIEUSB_FIRMWARE_NAME            "01.10"
#else
#define S1_SIEUSB_AUTH_BRANCH_OFFSET       0x490
#define S1_SIEUSB_SEQUENCE_BRANCH_OFFSET   0x54c
#define S1_SIEUSB_FIRMWARE_NAME            "06.00"
#endif

#define S1_SIEUSB_AUTH_BRANCH_ORIGINAL     0x540002e0
#define S1_SIEUSB_AUTH_BRANCH_PATCHED      0x14000017
#define S1_SIEUSB_SEQUENCE_BRANCH_ORIGINAL 0x54000140
/* Preserve the original B.EQ destination.  The former 0x14000011 value
 * jumped into memcpy with uninitialized argument registers. */
#define S1_SIEUSB_SEQUENCE_BRANCH_PATCHED  0x1400000a
#define S1_SIEUSB_AUTH_POINTER_OFFSET      0x9188

static bool s1_sieusb_layout_valid(unsigned long base,
                                   unsigned int size)
{
    u32 first;
    u32 second;

    if (size < S1_SIEUSB_SEQUENCE_BRANCH_OFFSET + sizeof(u32))
        return false;
    first = READ_ONCE(*(u32 *)(base + S1_SIEUSB_AUTH_BRANCH_OFFSET));
    second = READ_ONCE(*(u32 *)(base + S1_SIEUSB_SEQUENCE_BRANCH_OFFSET));
    return (first == S1_SIEUSB_AUTH_BRANCH_ORIGINAL ||
            first == S1_SIEUSB_AUTH_BRANCH_PATCHED) &&
           (second == S1_SIEUSB_SEQUENCE_BRANCH_ORIGINAL ||
            second == S1_SIEUSB_SEQUENCE_BRANCH_PATCHED ||
            second == 0x14000011);
}

static int s1_set_sieusb_bypass(unsigned long base, bool enabled)
{
    int (*kfn_set_memory_rw)(unsigned long, int) =
        (void *)kallsyms_lookup_name("set_memory_rw");
    int (*kfn_set_memory_ro)(unsigned long, int) =
        (void *)kallsyms_lookup_name("set_memory_ro");
    u32 *patch_addr1 =
        (u32 *)(base + S1_SIEUSB_AUTH_BRANCH_OFFSET);
    u32 *patch_addr2 =
        (u32 *)(base + S1_SIEUSB_SEQUENCE_BRANCH_OFFSET);
    unsigned long page1, page2;
    int ret;

    if (!kfn_set_memory_rw || !kfn_set_memory_ro) {
        printk(KERN_ERR
               "stage1: refusing sieusb text patch without set_memory_rw/ro\n");
        return -ENOENT;
    }

    page1 = (unsigned long)patch_addr1 & PAGE_MASK;
    page2 = (unsigned long)patch_addr2 & PAGE_MASK;
    ret = kfn_set_memory_rw(page1, 1);
    if (!ret && page2 != page1)
        ret = kfn_set_memory_rw(page2, 1);
    if (ret) {
        printk(KERN_ERR "stage1: set_memory_rw failed: %d\n", ret);
        if (page2 != page1)
            kfn_set_memory_ro(page2, 1);
        kfn_set_memory_ro(page1, 1);
        return ret;
    }

    WRITE_ONCE(*patch_addr1, enabled ? S1_SIEUSB_AUTH_BRANCH_PATCHED :
                                      S1_SIEUSB_AUTH_BRANCH_ORIGINAL);
    WRITE_ONCE(*patch_addr2, enabled ? S1_SIEUSB_SEQUENCE_BRANCH_PATCHED :
                                      S1_SIEUSB_SEQUENCE_BRANCH_ORIGINAL);
    flush_icache_range((unsigned long)patch_addr1,
                       (unsigned long)patch_addr2 + sizeof(*patch_addr2));

    ret = kfn_set_memory_ro(page1, 1);
    if (page2 != page1) {
        int ret2 = kfn_set_memory_ro(page2, 1);
        if (!ret)
            ret = ret2;
    }
    if (ret) {
        printk(KERN_ERR "stage1: set_memory_ro failed: %d\n", ret);
    } else {
        printk(KERN_INFO "stage1: sieusb authentication bypass %s\n",
               enabled ? "enabled" : "disabled");
    }
    return ret;
}

static void s1_force_auth_ok(void)
{
    struct file *auth_file;
    struct module *sieusb_mod;
    bool direct_repaired = false;
    int retry;

    mutex_lock(&module_mutex);
    sieusb_mod = find_module("sieusb");
    if (sieusb_mod) {
        unsigned long base = (unsigned long)sieusb_mod->module_core;
        if (s1_sieusb_layout_valid(base, sieusb_mod->core_size)) {
            /*
             * IDA's 0x8508 is .bss+8 in the linked image.  The live 06.00
             * module loader discards init sections and places .bss at
             * module_core+0x9180, so the auth pointer is at +0x9188.
             */
            void **qword_ptr =
                (void **)(base + S1_SIEUSB_AUTH_POINTER_OFFSET);

            printk(KERN_INFO "stage1: detected exact sieusb fv%s layout\n",
                   S1_SIEUSB_FIRMWARE_NAME);
            s1_set_sieusb_bypass(base, true);
            if (qword_ptr && *qword_ptr && *qword_ptr != (void *)-1LL) {
                unsigned char *p_auth = (unsigned char *)(*qword_ptr);

                WRITE_ONCE(p_auth[0xEC], 0xFF);
                WRITE_ONCE(p_auth[0xED], 0);
                WRITE_ONCE(p_auth[0xEF], 0x80);
                WRITE_ONCE(p_auth[0x2EC], 0);
                smp_wmb();
                if (READ_ONCE(p_auth[0xEC]) == 0xFF &&
                    READ_ONCE(p_auth[0xED]) == 0 &&
                    READ_ONCE(p_auth[0xEF]) == 0x80 &&
                    READ_ONCE(p_auth[0x2EC]) == 0) {
                    direct_repaired = true;
                    printk(KERN_INFO
                           "stage1: sieusb auth state reset verified "
                           "(ptr=0x%lx)\n",
                           (unsigned long)p_auth);
                } else {
                    printk(KERN_ERR
                           "stage1: sieusb auth state reset did not stick\n");
                }
            } else {
                printk(KERN_WARNING
                       "stage1: p_auth_ptr is NULL/unresolved at live 0x%lx\n",
                       (unsigned long)qword_ptr);
            }
        } else {
            printk(KERN_WARNING
                   "stage1: refusing non-%s sieusb layout\n",
                   S1_SIEUSB_FIRMWARE_NAME);
        }
    } else {
        printk(KERN_WARNING "stage1: sieusb module NOT found via find_module\n");
    }
    mutex_unlock(&module_mutex);

    /* The IOCTL path is slower and can generate more auth traffic. */
    if (direct_repaired)
        return;

    /* Original IOCTL method only as a fallback when direct repair failed. */
    for (retry = 0; retry < 5; retry++) {
        auth_file = filp_open("/dev/usb_auth", O_RDWR, 0);
        if (!IS_ERR(auth_file)) {
#ifdef KERNEL_DS
            mm_segment_t old_fs = get_fs();
            set_fs(KERNEL_DS);
#endif
            if (auth_file->f_op && auth_file->f_op->unlocked_ioctl) {
                /* IOCTL 707 sets authentication state to 0x80 (Authenticated)
                 * based on sieusb.ko analysis. 706 only sets 0x40. */
                auth_file->f_op->unlocked_ioctl(auth_file, 707, 0);
                printk(KERN_INFO "stage1: OK auth forced via IOCTL 707\n");
            }
#ifdef KERNEL_DS
            set_fs(old_fs);
#endif
            filp_close(auth_file, NULL);
            return;
        }
        msleep(100);
    }
}

static void s1_auth_repair_workfn(struct work_struct *work)
{
    (void)work;
    s1_force_auth_ok();
    pr_info("stage1: deferred post-transport auth repair complete\n");
}

static DECLARE_DELAYED_WORK(s1_auth_repair_work,
                            s1_auth_repair_workfn);

static void s1_schedule_auth_repair(unsigned int delay_ms)
{
    mod_delayed_work(system_wq, &s1_auth_repair_work,
                     msecs_to_jiffies(delay_ms));
}

static void s1_cancel_auth_repair(void)
{
    cancel_delayed_work_sync(&s1_auth_repair_work);
}

/*
 * Bounded selector-13 reflection session.
 *
 * The normal Stage1 transport deliberately bypasses sieusb authentication.
 * A real VrhmdMain authentication transaction therefore needs a short,
 * explicitly armed window in which the original branches are restored.  A
 * temporary /dev/siedrv ioctl interposer observes only VrhmdMain's successful
 * AUTH_CHALLENGE call, then uses the exact returned key/expected-response pair
 * to build the matching 60-byte host response.  No stale selector output is
 * ever replayed.
 *
 * The host cannot use the exploit mailbox during that window because its HID
 * traffic shares the authentication report path.  Three fixed-size events are
 * consequently streamed over Stage1's independent bulk-IN endpoint:
 * certificate, response, and terminal authentication state.
 */
#define S1_REFLECTION_EVENT_MAGIC 0x3152463156525350ULL
#define S1_REFLECTION_EVENT_ABI   1
#define S1_REFLECTION_EVENT_SIZE  192
#define S1_REFLECTION_EVENT_CERTIFICATE 1
#define S1_REFLECTION_EVENT_RESPONSE    2
#define S1_REFLECTION_EVENT_RESULT      3
#define S1_REFLECTION_TIMEOUT_MS        15000
#define S1_REFLECTION_START_TIMEOUT_MS  60000
#define S1_REFLECTION_ARM_DELAY_MS      750
#define S1_REFLECTION_STATE_POLL_MS     100

#define S1_SIEDRV_AUTH_CHALLENGE 0xC0F44507U
#define S1_SIEDRV_CALC_AES_CMAC  0xC0684508U
#define S1_SIEDRV_GET_HOST_TYPE  0x8004450AU
#define S1_SIEDRV_GET_PERI_CERT  0x80A0450BU

struct s1_reflection_event {
    u64 magic;
    u16 abi_version;
    u16 firmware;
    u32 kind;
    u64 generation;
    s32 result;
    u32 payload_length;
    u8 payload[160];
};

struct s1_auth_challenge_request {
    u8 host_certificate[160];
    u8 host_challenge[16];
    u8 response[4][16];
    u32 secure_error_reason;
};

struct s1_cmac_request {
    u8 output[16];
    u8 message[64];
    u32 message_size;
    u8 caller_key[16];
    u32 secure_status;
};

typedef long (*s1_ioctl_fn)(struct file *, unsigned int, unsigned long);

static DEFINE_MUTEX(s1_reflection_lock);
static DECLARE_WAIT_QUEUE_HEAD(s1_reflection_wq);
static struct s1_reflection_event s1_reflection_events[3];
static unsigned int s1_reflection_ready;
static unsigned long s1_reflection_generation;
static bool s1_reflection_pending;
static bool s1_reflection_armed;
static bool s1_reflection_launch_daemon;
static bool s1_reflection_module_held;
static struct module *s1_reflection_siedrv_module;
static struct file *s1_reflection_siedrv_file;
static struct file_operations *s1_reflection_siedrv_fops;
static s1_ioctl_fn s1_reflection_original_ioctl;
static unsigned long s1_reflection_start_deadline;

static void s1_reflection_arm_workfn(struct work_struct *work);
static void s1_reflection_state_workfn(struct work_struct *work);
static void s1_reflection_timeout_workfn(struct work_struct *work);
static void s1_reflection_finish_workfn(struct work_struct *work);
static long s1_reflection_ioctl(struct file *file, unsigned int command,
                                unsigned long argument);
static DECLARE_DELAYED_WORK(s1_reflection_arm_work,
                            s1_reflection_arm_workfn);
static DECLARE_DELAYED_WORK(s1_reflection_state_work,
                            s1_reflection_state_workfn);
static DECLARE_DELAYED_WORK(s1_reflection_timeout_work,
                            s1_reflection_timeout_workfn);
static DECLARE_DELAYED_WORK(s1_reflection_finish_work,
                            s1_reflection_finish_workfn);

static void s1_reflection_init_event(struct s1_reflection_event *event,
                                     unsigned int kind)
{
    memset(event, 0, sizeof(*event));
    event->magic = S1_REFLECTION_EVENT_MAGIC;
    event->abi_version = S1_REFLECTION_EVENT_ABI;
    event->firmware = S1_FW_VERSION;
    event->kind = kind;
    event->generation = s1_reflection_generation;
}

static void s1_reflection_publish_locked(unsigned int index, int result,
                                         const void *payload,
                                         size_t payload_length)
{
    struct s1_reflection_event *event;

    if (index >= ARRAY_SIZE(s1_reflection_events) ||
        payload_length > sizeof(s1_reflection_events[index].payload))
        return;
    event = &s1_reflection_events[index];
    s1_reflection_init_event(event, index + 1);
    event->result = result;
    event->payload_length = payload_length;
    if (payload_length)
        memcpy(event->payload, payload, payload_length);
    smp_wmb();
    if (s1_reflection_ready < index + 1)
        WRITE_ONCE(s1_reflection_ready, index + 1);
    wake_up_all(&s1_reflection_wq);
}

static long s1_reflection_kernel_ioctl(unsigned int command, void *buffer)
{
    long result;

    if (!s1_reflection_siedrv_file ||
        !s1_reflection_original_ioctl)
        return -ENODEV;
#ifdef KERNEL_DS
    {
        mm_segment_t old_fs = get_fs();

        set_fs(KERNEL_DS);
        result = s1_reflection_original_ioctl(
            s1_reflection_siedrv_file, command,
            (unsigned long)buffer);
        set_fs(old_fs);
    }
#else
    result = -EOPNOTSUPP;
#endif
    return result;
}

static unsigned char *s1_reflection_auth_context(void)
{
    struct module *sieusb_mod;
    unsigned char *context = NULL;

    mutex_lock(&module_mutex);
    sieusb_mod = find_module("sieusb");
    if (sieusb_mod) {
        unsigned long base = (unsigned long)sieusb_mod->module_core;

        if (s1_sieusb_layout_valid(base, sieusb_mod->core_size)) {
            void **pointer =
                (void **)(base + S1_SIEUSB_AUTH_POINTER_OFFSET);

            if (*pointer && *pointer != (void *)-1LL)
                context = *pointer;
        }
    }
    mutex_unlock(&module_mutex);
    return context;
}

static int s1_reflection_restore_stock_auth(void)
{
    struct module *sieusb_mod;
    int result = -ENODEV;

    mutex_lock(&module_mutex);
    sieusb_mod = find_module("sieusb");
    if (sieusb_mod) {
        unsigned long base = (unsigned long)sieusb_mod->module_core;

        if (s1_sieusb_layout_valid(base, sieusb_mod->core_size)) {
            void **pointer =
                (void **)(base + S1_SIEUSB_AUTH_POINTER_OFFSET);

            result = s1_set_sieusb_bypass(base, false);
            if (!result && *pointer && *pointer != (void *)-1LL) {
                unsigned char *auth = *pointer;

                WRITE_ONCE(auth[0xEC], 0xFF);
                WRITE_ONCE(auth[0xED], 0);
                WRITE_ONCE(auth[0xEF], 0x10);
                WRITE_ONCE(auth[0x2EC], 0);
                smp_wmb();
            } else if (!result) {
                result = -ENODEV;
            }
        } else {
            result = -EINVAL;
        }
    }
    mutex_unlock(&module_mutex);
    return result;
}

static int s1_reflection_write_fops(s1_ioctl_fn replacement)
{
    int (*set_rw)(unsigned long, int) =
        (void *)kallsyms_lookup_name("set_memory_rw");
    int (*set_ro)(unsigned long, int) =
        (void *)kallsyms_lookup_name("set_memory_ro");
    unsigned long page;
    int result;

    if (!s1_reflection_siedrv_fops || !set_rw || !set_ro)
        return -ENOENT;
    page = (unsigned long)s1_reflection_siedrv_fops & PAGE_MASK;
    result = set_rw(page, 1);
    if (result)
        return result;
    WRITE_ONCE(s1_reflection_siedrv_fops->unlocked_ioctl, replacement);
    smp_wmb();
    result = set_ro(page, 1);
    if (result && replacement == s1_reflection_ioctl &&
        s1_reflection_original_ioctl) {
        /* The write already happened.  Never return an installation error
         * while leaving the shared fops table pointing into Stage1. */
        set_rw(page, 1);
        WRITE_ONCE(s1_reflection_siedrv_fops->unlocked_ioctl,
                   s1_reflection_original_ioctl);
        smp_wmb();
        set_ro(page, 1);
    }
    return result;
}

static long s1_reflection_ioctl(struct file *file, unsigned int command,
                                unsigned long argument)
{
    long result = s1_reflection_original_ioctl(file, command, argument);

    if (result == 0 && command == S1_SIEDRV_AUTH_CHALLENGE &&
        READ_ONCE(s1_reflection_armed) &&
        !strcmp(current->comm, "VrhmdMain")) {
        struct s1_auth_challenge_request request;
        struct s1_cmac_request cmac;
        unsigned char response[60];
        unsigned char *auth;
        u32 host_type = 0;
        long cmac_result;
        long type_result;

        memset(&request, 0, sizeof(request));
        if (copy_from_user(&request, (void __user *)argument,
                           sizeof(request)) != 0)
            return result;

        mutex_lock(&s1_reflection_lock);
        if (!s1_reflection_armed || s1_reflection_ready >= 2) {
            mutex_unlock(&s1_reflection_lock);
            memset(&request, 0, sizeof(request));
            return result;
        }

        memset(response, 0, sizeof(response));
        response[0] = 0xF0;
        response[1] = 2;
        auth = s1_reflection_auth_context();
        response[2] = auth ? READ_ONCE(auth[0xEC]) : 0;
        memcpy(response + 4, request.response[3], 16);

        type_result = s1_reflection_kernel_ioctl(
            S1_SIEDRV_GET_HOST_TYPE, &host_type);
        if (!type_result)
            response[20] = (u8)host_type;

        memset(&cmac, 0, sizeof(cmac));
        memcpy(cmac.message, response, 32);
        cmac.message_size = 32;
        memcpy(cmac.caller_key, request.response[1], 16);
        cmac_result = s1_reflection_kernel_ioctl(
            S1_SIEDRV_CALC_AES_CMAC, &cmac);
        if (!cmac_result && !type_result) {
            memcpy(response + 32, cmac.output, 16);
            s1_reflection_publish_locked(
                1, 0, response, sizeof(response));
            mod_delayed_work(system_wq, &s1_reflection_finish_work,
                             msecs_to_jiffies(100));
        } else {
            int error = cmac_result ? (int)cmac_result : (int)type_result;

            s1_reflection_publish_locked(1, error, NULL, 0);
            s1_reflection_publish_locked(2, error, NULL, 0);
        }
        mutex_unlock(&s1_reflection_lock);
        memset(&cmac, 0, sizeof(cmac));
        memset(response, 0, sizeof(response));
        memset(&request, 0, sizeof(request));
    }
    return result;
}

static int s1_reflection_install_hook(void)
{
    const struct file_operations *operations;
    struct module *siedrv_mod;
    bool owner_held = false;
    bool self_held = false;
    int result;

    s1_reflection_siedrv_file =
        filp_open("/dev/siedrv", O_RDONLY, 0);
    if (IS_ERR(s1_reflection_siedrv_file)) {
        result = PTR_ERR(s1_reflection_siedrv_file);
        s1_reflection_siedrv_file = NULL;
        return result;
    }
    operations = s1_reflection_siedrv_file->f_op;
    if (!operations || !operations->unlocked_ioctl) {
        pr_err("stage1: siedrv file operations lack ioctl\n");
        filp_close(s1_reflection_siedrv_file, NULL);
        s1_reflection_siedrv_file = NULL;
        return -EINVAL;
    }
    mutex_lock(&module_mutex);
    siedrv_mod = find_module("siedrv");
    if (siedrv_mod &&
        (unsigned long)operations->unlocked_ioctl >=
            (unsigned long)siedrv_mod->module_core &&
        (unsigned long)operations->unlocked_ioctl <
            (unsigned long)siedrv_mod->module_core +
                siedrv_mod->core_size)
        owner_held = try_module_get(siedrv_mod);
    mutex_unlock(&module_mutex);
    if (!owner_held)
        pr_err("stage1: siedrv ioctl is outside the live module core\n");
    if (owner_held)
        self_held = try_module_get(THIS_MODULE);
    if (!owner_held || !self_held) {
        if (self_held)
            module_put(THIS_MODULE);
        if (owner_held)
            module_put(siedrv_mod);
        filp_close(s1_reflection_siedrv_file, NULL);
        s1_reflection_siedrv_file = NULL;
        return -EBUSY;
    }
    s1_reflection_module_held = true;
    s1_reflection_siedrv_module = siedrv_mod;
    s1_reflection_siedrv_fops =
        (struct file_operations *)operations;
    s1_reflection_original_ioctl = operations->unlocked_ioctl;
    if (s1_reflection_original_ioctl == s1_reflection_ioctl) {
        module_put(THIS_MODULE);
        module_put(siedrv_mod);
        s1_reflection_module_held = false;
        s1_reflection_siedrv_module = NULL;
        s1_reflection_siedrv_fops = NULL;
        s1_reflection_original_ioctl = NULL;
        filp_close(s1_reflection_siedrv_file, NULL);
        s1_reflection_siedrv_file = NULL;
        return -EBUSY;
    }
    result = s1_reflection_write_fops(s1_reflection_ioctl);
    if (result) {
        module_put(THIS_MODULE);
        module_put(siedrv_mod);
        s1_reflection_module_held = false;
        s1_reflection_siedrv_module = NULL;
        s1_reflection_siedrv_fops = NULL;
        s1_reflection_original_ioctl = NULL;
        filp_close(s1_reflection_siedrv_file, NULL);
        s1_reflection_siedrv_file = NULL;
    }
    return result;
}

static void s1_reflection_remove_hook(void)
{
    WRITE_ONCE(s1_reflection_armed, false);
    smp_wmb();
    if (s1_reflection_siedrv_fops) {
        if (READ_ONCE(s1_reflection_siedrv_fops->unlocked_ioctl) ==
            s1_reflection_ioctl)
            s1_reflection_write_fops(s1_reflection_original_ioctl);
    }
    if (s1_reflection_siedrv_file) {
        filp_close(s1_reflection_siedrv_file, NULL);
        s1_reflection_siedrv_file = NULL;
    }
    s1_reflection_siedrv_fops = NULL;
    s1_reflection_original_ioctl = NULL;
    if (s1_reflection_module_held) {
        if (s1_reflection_siedrv_module)
            module_put(s1_reflection_siedrv_module);
        s1_reflection_siedrv_module = NULL;
        s1_reflection_module_held = false;
        module_put(THIS_MODULE);
    }
}

static bool s1_reflection_vrhmd_running(void)
{
    struct task_struct *task;
    bool running = false;

    rcu_read_lock();
    for_each_process(task) {
        if (!strcmp(task->comm, "VrhmdMain") &&
            READ_ONCE(task->exit_state) == 0) {
            running = true;
            break;
        }
    }
    rcu_read_unlock();
    return running;
}

static int s1_reflection_launch_vrhmd(void)
{
    const char *busybox = "/data/modules/busybox";
    struct file *probe;
    char *argv[] = {
        NULL, "setsid", "/tmp/vrhmd_main.elf", NULL
    };
    char *envp[] = {
        "HOME=/",
        "PATH=/sie/bin:/bin",
        "LD_LIBRARY_PATH=/sie/lib",
        "TERM=linux",
        NULL
    };
    int result;

    if (s1_reflection_vrhmd_running()) {
        pr_err("stage1: refusing reflection launch while VrhmdMain runs\n");
        return -EBUSY;
    }
    probe = filp_open(busybox, O_RDONLY, 0);
    if (IS_ERR(probe)) {
        busybox = "/tmp/busybox";
        probe = filp_open(busybox, O_RDONLY, 0);
    }
    if (IS_ERR(probe)) {
        pr_err("stage1: BusyBox is unavailable for VrhmdMain setsid\n");
        return -ENOENT;
    }
    filp_close(probe, NULL);
    argv[0] = (char *)busybox;
    result = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_EXEC);
    if (result) {
        pr_err("stage1: VrhmdMain launch failed: %d\n", result);
        return result;
    }
    pr_info("stage1: controlled VrhmdMain launch accepted\n");
    return 0;
}

static void s1_reflection_complete(bool repair_auth)
{
    mutex_lock(&s1_reflection_lock);
    s1_reflection_pending = false;
    s1_reflection_remove_hook();
    mutex_unlock(&s1_reflection_lock);
    if (repair_auth)
        s1_force_auth_ok();
}

static void s1_reflection_arm_workfn(struct work_struct *work)
{
    int result;

    (void)work;
    mutex_lock(&s1_reflection_lock);
    if (!s1_reflection_pending) {
        mutex_unlock(&s1_reflection_lock);
        return;
    }
    result = s1_reflection_install_hook();
    if (result)
        pr_err("stage1: reflection hook install failed: %d\n", result);
    if (!result)
        result = s1_reflection_restore_stock_auth();
    if (result)
        pr_err("stage1: stock authentication restore failed: %d\n",
               result);
    if (result) {
        s1_reflection_publish_locked(0, result, NULL, 0);
        s1_reflection_publish_locked(1, result, NULL, 0);
        s1_reflection_publish_locked(2, result, NULL, 0);
        s1_reflection_pending = false;
        s1_reflection_remove_hook();
        mutex_unlock(&s1_reflection_lock);
        s1_force_auth_ok();
        return;
    }
    s1_reflection_armed = true;
    s1_reflection_pending = false;
    if (s1_reflection_launch_daemon)
        result = s1_reflection_launch_vrhmd();
    if (result) {
        s1_reflection_publish_locked(0, result, NULL, 0);
        s1_reflection_publish_locked(1, result, NULL, 0);
        s1_reflection_publish_locked(2, result, NULL, 0);
        s1_reflection_remove_hook();
        mutex_unlock(&s1_reflection_lock);
        s1_force_auth_ok();
        return;
    }
    s1_reflection_start_deadline =
        jiffies + msecs_to_jiffies(S1_REFLECTION_START_TIMEOUT_MS);
    mod_delayed_work(system_wq, &s1_reflection_state_work,
                     msecs_to_jiffies(S1_REFLECTION_STATE_POLL_MS));
    mutex_unlock(&s1_reflection_lock);
    pr_info("stage1: waiting for authentication state 0x11\n");
}

static void s1_reflection_state_workfn(struct work_struct *work)
{
    unsigned char certificate[160];
    unsigned char *auth;
    unsigned char state;
    bool repair_auth = false;
    int result = 0;

    (void)work;
    mutex_lock(&s1_reflection_lock);
    if (!s1_reflection_armed || s1_reflection_ready != 0) {
        mutex_unlock(&s1_reflection_lock);
        return;
    }

    auth = s1_reflection_auth_context();
    state = auth ? READ_ONCE(auth[0xEF]) : 0;
    if (state != 0x11 &&
        time_before(jiffies, s1_reflection_start_deadline)) {
        mutex_unlock(&s1_reflection_lock);
        mod_delayed_work(system_wq, &s1_reflection_state_work,
                         msecs_to_jiffies(S1_REFLECTION_STATE_POLL_MS));
        return;
    }

    memset(certificate, 0, sizeof(certificate));
    if (state != 0x11) {
        result = -ETIMEDOUT;
        pr_err("stage1: authentication readiness timed out in state 0x%02x\n",
               state);
    } else {
        result = s1_reflection_kernel_ioctl(
            S1_SIEDRV_GET_PERI_CERT, certificate);
        if (result)
            pr_err("stage1: peripheral certificate ioctl failed: %d\n",
                   result);
    }

    if (result) {
        s1_reflection_publish_locked(0, result, NULL, 0);
        s1_reflection_publish_locked(1, result, NULL, 0);
        s1_reflection_publish_locked(2, result, NULL, 0);
        s1_reflection_remove_hook();
        repair_auth = true;
    } else {
        s1_reflection_publish_locked(
            0, 0, certificate, sizeof(certificate));
        mod_delayed_work(system_wq, &s1_reflection_timeout_work,
                         msecs_to_jiffies(S1_REFLECTION_TIMEOUT_MS));
    }
    mutex_unlock(&s1_reflection_lock);
    memset(certificate, 0, sizeof(certificate));
    if (repair_auth)
        s1_force_auth_ok();
    else
        pr_info("stage1: reflected authentication window armed at state 0x11\n");
}

static void s1_reflection_timeout_workfn(struct work_struct *work)
{
    (void)work;
    mutex_lock(&s1_reflection_lock);
    if (!s1_reflection_pending && !s1_reflection_armed) {
        mutex_unlock(&s1_reflection_lock);
        return;
    }
    if (s1_reflection_ready < 2)
        s1_reflection_publish_locked(1, -ETIMEDOUT, NULL, 0);
    if (s1_reflection_ready < 3)
        s1_reflection_publish_locked(2, -ETIMEDOUT, NULL, 0);
    mutex_unlock(&s1_reflection_lock);
    s1_reflection_complete(true);
    pr_warn("stage1: reflected authentication window timed out\n");
}

static void s1_reflection_finish_workfn(struct work_struct *work)
{
    unsigned char *auth;
    unsigned char state;

    (void)work;
    mutex_lock(&s1_reflection_lock);
    if (!s1_reflection_armed || s1_reflection_ready < 2) {
        mutex_unlock(&s1_reflection_lock);
        return;
    }
    auth = s1_reflection_auth_context();
    state = auth ? READ_ONCE(auth[0xEF]) : 0;
    if (state != 0x40 && state != 0x80) {
        mutex_unlock(&s1_reflection_lock);
        mod_delayed_work(system_wq, &s1_reflection_finish_work,
                         msecs_to_jiffies(100));
        return;
    }
    s1_reflection_publish_locked(
        2, state == 0x40 ? 0 : -EACCES, &state, sizeof(state));
    mutex_unlock(&s1_reflection_lock);
    cancel_delayed_work(&s1_reflection_timeout_work);
    s1_reflection_complete(true);
    pr_info("stage1: reflected authentication ended in state 0x%02x\n",
            state);
}

static bool s1_auth_reflection_is_armed(void)
{
    return READ_ONCE(s1_reflection_pending) ||
           READ_ONCE(s1_reflection_armed);
}

static int s1_auth_reflection_request(bool launch_daemon)
{
    int result = 0;

    BUILD_BUG_ON(sizeof(struct s1_reflection_event) !=
                 S1_REFLECTION_EVENT_SIZE);
    BUILD_BUG_ON(sizeof(struct s1_auth_challenge_request) != 244);
    BUILD_BUG_ON(sizeof(struct s1_cmac_request) != 104);

    /* stage1_init schedules a five-second fail-safe authentication repair.
     * If reflection is armed before that timer expires, the queued worker
     * otherwise races the stock-state restore below and forces state 0x80
     * during the live authentication window.  Mailbox completion suppresses
     * new repairs while pending/armed; cancel the preexisting one as well. */
    s1_cancel_auth_repair();
    cancel_delayed_work_sync(&s1_reflection_arm_work);
    cancel_delayed_work_sync(&s1_reflection_state_work);
    cancel_delayed_work_sync(&s1_reflection_timeout_work);
    cancel_delayed_work_sync(&s1_reflection_finish_work);
    if (s1_auth_reflection_is_armed())
        s1_reflection_complete(true);

    mutex_lock(&s1_reflection_lock);
    ++s1_reflection_generation;
    memset(s1_reflection_events, 0, sizeof(s1_reflection_events));
    WRITE_ONCE(s1_reflection_ready, 0);
    s1_reflection_launch_daemon = launch_daemon;
    s1_reflection_pending = true;
    if (!mod_delayed_work(system_wq, &s1_reflection_arm_work,
                          msecs_to_jiffies(
                              S1_REFLECTION_ARM_DELAY_MS)))
        result = 0;
    mutex_unlock(&s1_reflection_lock);
    return result;
}

static int s1_auth_reflection_copy_event(unsigned int index,
                                         void *output, size_t length)
{
    long waited;

    if (!output || length != S1_REFLECTION_EVENT_SIZE || index >= 3)
        return -EINVAL;
    waited = wait_event_interruptible_timeout(
        s1_reflection_wq,
        READ_ONCE(s1_reflection_ready) > index,
        msecs_to_jiffies(
            (index == 0 ? S1_REFLECTION_START_TIMEOUT_MS :
                          S1_REFLECTION_TIMEOUT_MS) + 2000));
    if (waited <= 0)
        return waited ? (int)waited : -ETIMEDOUT;
    smp_rmb();
    memcpy(output, &s1_reflection_events[index], length);
    if (index == 1)
        memset(s1_reflection_events[index].payload, 0,
               sizeof(s1_reflection_events[index].payload));
    return 0;
}

static void s1_auth_reflection_stop(void)
{
    cancel_delayed_work_sync(&s1_reflection_arm_work);
    cancel_delayed_work_sync(&s1_reflection_state_work);
    cancel_delayed_work_sync(&s1_reflection_timeout_work);
    cancel_delayed_work_sync(&s1_reflection_finish_work);
    if (s1_auth_reflection_is_armed())
        s1_reflection_complete(true);
    wake_up_all(&s1_reflection_wq);
    memset(s1_reflection_events, 0, sizeof(s1_reflection_events));
}
