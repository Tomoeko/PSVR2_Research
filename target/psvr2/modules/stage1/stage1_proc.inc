/* /proc/stage1 write handler */
static ssize_t stage1_write(struct file *file, const char __user *ubuf,
                            size_t count, loff_t *ppos)
{
    char cmd[512];
    size_t len = min(count, sizeof(cmd) - 1);

    (void)file;
    (void)ppos;

    if (copy_from_user(cmd, ubuf, len))
        return -EFAULT;
    cmd[len] = '\0';

    if (len > 0 && cmd[len-1] == '\n')
        cmd[--len] = '\0';

    if (strncmp(cmd, "recv_stop", 9) == 0) {
        recv_stop();
        snprintf(status_buf, STATUS_SIZE, "OK recv_stop %llu",
                 (unsigned long long)recv_total);

    } else if (strncmp(cmd, "recv_multi ", 11) == 0) {
        /* Multi-file: recv_multi <path1> <size1> <path2> <size2> ... */
        char args_buf[512];
        char *p, *tok;
        unsigned long total_size = 0;
        int fc = 0;

        send_stop();
        recv_stop();

        strncpy(args_buf, cmd + 11, sizeof(args_buf) - 1);
        args_buf[sizeof(args_buf) - 1] = '\0';

        p = args_buf;
        while (fc < RECV_MULTI_MAX) {
            unsigned long sz = 0;
            while (*p == ' ') p++;
            if (!*p) break;
            tok = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
            strncpy(recv_multi_paths[fc], tok, MAX_PATH_LEN - 1);
            recv_multi_paths[fc][MAX_PATH_LEN - 1] = '\0';
            while (*p == ' ') p++;
            if (!*p) break;
            tok = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
            if (kstrtoul(tok, 10, &sz) != 0 || sz == 0) {
                snprintf(status_buf, STATUS_SIZE,
                    "ERR recv_multi bad size[%d]", fc);
                return count;
            }
            recv_multi_sizes[fc] = sz;
            if (total_size > ULONG_MAX - sz) {
                snprintf(status_buf, STATUS_SIZE,
                    "ERR recv_multi size overflow");
                return count;
            }
            total_size += sz;
            fc++;
        }

        if (fc == 0) {
            snprintf(status_buf, STATUS_SIZE,
                "ERR recv_multi no files");
            return count;
        }

        recv_multi_count = fc;
        recv_multi_cur = 0;
        recv_total = 0;
        recv_expected = recv_multi_sizes[0];
        recv_grand_total = 0;
        recv_grand_expected = total_size;
        recv_error = 0;
        recv_out_fp = NULL;

        {
            ssize_t ret = recv_setup_usb(count);
            if (ret) return ret;
        }

    } else if (strncmp(cmd, "recv ", 5) == 0) {
        /* Single file: recv <path> <size> */
        char path[MAX_PATH_LEN];
        unsigned long size = 0;
        char *space;

        send_stop();
        recv_stop();

        strncpy(path, cmd + 5, MAX_PATH_LEN - 1);
        path[MAX_PATH_LEN - 1] = '\0';
        space = strchr(path, ' ');
        if (!space) {
            snprintf(status_buf, STATUS_SIZE,
                     "ERR recv usage: recv <path> <size>");
            return count;
        }
        *space = '\0';
        if (kstrtoul(space + 1, 10, &size) != 0 || size == 0) {
            snprintf(status_buf, STATUS_SIZE, "ERR recv bad size");
            return count;
        }

        strncpy(recv_path, path, MAX_PATH_LEN - 1);
        recv_path[MAX_PATH_LEN - 1] = '\0';
        recv_multi_count = 0;
        recv_total = 0;
        recv_expected = size;
        recv_grand_total = 0;
        recv_grand_expected = size;
        recv_error = 0;
        recv_out_fp = NULL;

        {
            ssize_t ret = recv_setup_usb(count);
            if (ret) return ret;
        }

    } else if (strcmp(cmd, "send_prepare") == 0) {
        int error = send_prepare();

        if (error)
            return error;

    } else if (strncmp(cmd, "send_stop", 9) == 0) {
        send_stop();
        snprintf(status_buf, STATUS_SIZE, "OK send_stop %llu",
                 (unsigned long long)send_total);

    } else if (strncmp(cmd, "send ", 5) == 0) {
        /* send <path> [size] - read file and push via USB IN */
        char path[MAX_PATH_LEN];
        struct file *tmp_fp;
        loff_t fsize = 0;
        char *space;

        recv_stop();
        send_stop();

        strncpy(path, cmd + 5, MAX_PATH_LEN - 1);
        path[MAX_PATH_LEN - 1] = '\0';

        /* Look for optional size argument: send <path> <size> */
        space = strchr(path, ' ');
        if (space) {
            unsigned long sarg = 0;
            *space = '\0';
            if (kstrtoul(space + 1, 10, &sarg) == 0) {
                fsize = sarg;
            }
        } else {
            /* Trim trailing whitespace if no size provided */
            int plen = strlen(path);
            while (plen > 0 && (path[plen-1] == ' ' || path[plen-1] == '\n'))
                path[--plen] = '\0';
        }

        /* Open file to check existence and get size if not provided */
        tmp_fp = filp_open(path, O_RDONLY, 0);
        if (IS_ERR(tmp_fp)) {
            snprintf(status_buf, STATUS_SIZE,
                     "ERR send open %ld: %s",
                     PTR_ERR(tmp_fp), path);
            return count;
        }

        if (fsize == 0) {
            fsize = i_size_read(file_inode(tmp_fp));
        }
        filp_close(tmp_fp, NULL);

        if (fsize == 0) {
            snprintf(status_buf, STATUS_SIZE,
                     "ERR send empty file: %s", path);
            return count;
        }

        strncpy(send_path, path, MAX_PATH_LEN - 1);
        send_path[MAX_PATH_LEN - 1] = '\0';
        send_total = 0;
        send_expected = fsize;
        send_error = 0;
        send_in_fp = NULL;

        {
            ssize_t ret = send_setup_usb(count);
            if (ret) return ret;
        }

        snprintf(status_buf, STATUS_SIZE, "OK send started %s %llu",
                 send_path, (unsigned long long)send_expected);

    } else if (strncmp(cmd, "send_probe", 10) == 0) {
        /* Probe gbuf->in (USB bulk IN endpoint) for fast_download.
         * Reports structure contents so we can verify IN is usable. */
        unsigned long cdev_addr_local = s1_usb_data3_cdev("probe");

        if (!cdev_addr_local) {
            return count;
        } else {
            struct s1_gbuf *gbuf_ptr = s1_usb_gbuf(cdev_addr_local);
            void *gin = gbuf_ptr->in;
            void *gout = gbuf_ptr->out;

            if (!gin) {
                snprintf(status_buf, STATUS_SIZE,
                    "ERR probe gbuf_in=NULL gbuf_out=%px cdev=%lx",
                    gout, cdev_addr_local);
            } else {
                int items = *(int *)((char *)gin + GO_ITEMS);
                size_t pktlen = GO_PKTLEN(gin);
                struct usb_ep *ep = GO_EPPTR(gin);
                char *dmabufs = *(char **)((char *)gin + GO_DMABUFS);
                int busy = atomic_read(&GO_BUSYCNT(gin));

                /* Peek at first request if available */
                if (items > 0 && dmabufs) {
                    char *dmabuf0 = dmabufs;  /* first entry, 80 bytes each */
                    struct usb_request *req0 =
                        *(struct usb_request **)(dmabuf0 + DMA_REQ);
                    void *buf0 = req0 ? req0->buf : NULL;
                    void (*cb0)(struct usb_ep *, struct usb_request *) =
                        req0 ? req0->complete : NULL;

                    snprintf(status_buf, STATUS_SIZE,
                        "OK probe in=%px items=%d pkt=%zu ep=%px "
                        "dma=%px busy=%d req0=%px buf0=%px cb0=%px",
                        gin, items, pktlen, ep,
                        dmabufs, busy, req0, buf0, cb0);
                } else {
                    snprintf(status_buf, STATUS_SIZE,
                        "OK probe in=%px items=%d pkt=%zu ep=%px "
                        "dma=%px busy=%d (no reqs)",
                        gin, items, pktlen, ep, dmabufs, busy);
                }
            }
        }

    } else if (strncmp(cmd, "cmd ", 4) == 0) {
        /* Fallback: write command to mailbox via /proc/stage1 write.
         * Preferred path is direct BSS write from host. */
        size_t cmdlen = len - 4;
        if (cmdlen >= CMD_INBUF_SIZE)
            cmdlen = CMD_INBUF_SIZE - 1;
        memcpy(cmd_in_buf, cmd + 4, cmdlen);
        cmd_in_buf[cmdlen] = '\0';
        smp_wmb();
        WRITE_ONCE(cmd_in_len, cmdlen);
        snprintf(status_buf, STATUS_SIZE, "OK cmd queued len=%zu", cmdlen);

    } else if (strcmp(cmd, "auth_reflect_arm") == 0) {
        ssize_t ret;

        recv_stop();
        send_stop();
        ret = s1_auth_reflection_request(false);
        if (ret) {
            snprintf(status_buf, STATUS_SIZE,
                     "ERR auth_reflect arm %d", (int)ret);
            return count;
        }
        send_total = 0;
        send_expected = 3 * S1_REFLECTION_EVENT_SIZE;
        send_error = 0;
        send_in_fp = NULL;
        send_reflection = 1;
        strcpy(send_path, "[auth-reflection]");
        ret = send_setup_usb(count);
        if (ret) {
            send_reflection = 0;
            s1_auth_reflection_stop();
            snprintf(status_buf, STATUS_SIZE,
                     "ERR auth_reflect send %d", (int)ret);
            return count;
        }
        snprintf(status_buf, STATUS_SIZE,
                 "OK auth_reflect pending generation=%lu",
                 s1_reflection_generation);

    } else if (strcmp(cmd, "auth_reflect_launch") == 0) {
        ssize_t ret;

        recv_stop();
        send_stop();
        ret = s1_auth_reflection_request(true);
        if (ret) {
            snprintf(status_buf, STATUS_SIZE,
                     "ERR auth_reflect_launch arm %d", (int)ret);
            return count;
        }
        send_total = 0;
        send_expected = 3 * S1_REFLECTION_EVENT_SIZE;
        send_error = 0;
        send_in_fp = NULL;
        send_reflection = 1;
        strcpy(send_path, "[auth-reflection-launch]");
        ret = send_setup_usb(count);
        if (ret) {
            send_reflection = 0;
            s1_auth_reflection_stop();
            snprintf(status_buf, STATUS_SIZE,
                     "ERR auth_reflect_launch send %d", (int)ret);
            return count;
        }
        snprintf(status_buf, STATUS_SIZE,
                 "OK auth_reflect_launch pending generation=%lu",
                 s1_reflection_generation);

    } else if (strcmp(cmd, "auth_reflect_disarm") == 0) {
        send_stop();
        s1_auth_reflection_stop();
        snprintf(status_buf, STATUS_SIZE, "OK auth_reflect disarmed");

    } else if (strncmp(cmd, "auth_ok", 7) == 0) {
        s1_force_auth_ok();
        snprintf(status_buf, STATUS_SIZE, "OK auth forced");

    } else {
        snprintf(status_buf, STATUS_SIZE, "ERR unknown cmd");
    }

    return count;
}

/* /proc/stage1 read handler */
static ssize_t stage1_read(struct file *file, char __user *ubuf,
                           size_t count, loff_t *ppos)
{
    char snapshot[STATUS_SIZE];
    size_t len;

    (void)file;

    if (recv_running) {
        if (recv_multi_count > 0) {
            snprintf(snapshot, sizeof(snapshot),
                     "OK recv %llu/%llu file[%d/%d]",
                     (unsigned long long)recv_grand_total,
                     (unsigned long long)recv_grand_expected,
                     recv_multi_cur + 1, recv_multi_count);
        } else {
            snprintf(snapshot, sizeof(snapshot),
                     "OK recv %llu/%llu",
                     (unsigned long long)recv_grand_total,
                     (unsigned long long)recv_grand_expected);
        }
    } else if (send_running) {
        snprintf(snapshot, sizeof(snapshot),
                 "OK send %llu/%llu",
                 (unsigned long long)send_total,
                 (unsigned long long)send_expected);
    } else {
        memcpy(snapshot, status_buf, sizeof(snapshot));
        snapshot[sizeof(snapshot) - 1] = '\0';
    }

    len = strnlen(snapshot, sizeof(snapshot));

    if (*ppos >= len)
        return 0;

    if (count > len - *ppos)
        count = len - *ppos;

    if (copy_to_user(ubuf, snapshot + *ppos, count))
        return -EFAULT;

    *ppos += count;
    return count;
}

static const struct file_operations stage1_fops = {
    .owner = THIS_MODULE,
    .write = stage1_write,
    .read  = stage1_read,
};

/* /proc/stage1_out: return the command output buffer */
static ssize_t stage1_out_read(struct file *file, char __user *ubuf,
                                size_t count, loff_t *ppos)
{
    unsigned long len = READ_ONCE(cmd_out_len);

    (void)file;
    if (len == 0)
        return 0;
    return simple_read_from_buffer(ubuf, count, ppos, cmd_out_buf, len);
}

static const struct file_operations stage1_out_fops = {
    .owner = THIS_MODULE,
    .read  = stage1_out_read,
};

static struct proc_dir_entry *proc_entry;
static struct proc_dir_entry *proc_out_entry;

static int __init stage1_init(void)
{
    int error;

    /* Log boot mode detection status for diagnostics */
    is_autoboot_disabled();

    s1_force_auth_ok();

    proc_entry = proc_create("stage1", 0666, NULL, &stage1_fops);
    if (!proc_entry)
        return -ENOMEM;


    proc_out_entry = proc_create("stage1_out", 0444, NULL,
                                  &stage1_out_fops);
    if (!proc_out_entry) {
        proc_remove(proc_entry);
        proc_entry = NULL;
        return -ENOMEM;
    }

    error = s1_mailbox_start();
    if (error) {
        pr_err("stage1: cmd kthread failed %d\n", error);
        proc_remove(proc_out_entry);
        proc_out_entry = NULL;
        proc_remove(proc_entry);
        proc_entry = NULL;
        return error;
    }

    s1_mailbox_publish_ready();

    /* Catch the host's remaining module-discovery reads after init. */
    s1_schedule_auth_repair(5000);

    pr_info("stage1: loaded (cmd mailbox active)\n");
    return 0;
}

static void stage1_exit(void)
{
    /* Quiesce proc callers before stopping the workers they can start. */
    if (proc_out_entry) {
        proc_remove(proc_out_entry);
        proc_out_entry = NULL;
    }
    if (proc_entry) {
        proc_remove(proc_entry);
        proc_entry = NULL;
    }

    /* A completing mailbox command can arm reflection or schedule repair. */
    s1_mailbox_stop();
    s1_auth_reflection_stop();
    recv_stop();
    send_stop();
    /* No remaining producer may requeue this work after cancellation. */
    s1_cancel_auth_repair();

    /*
     * Restore every sieusb callback before module text is freed; otherwise a
     * later endpoint completion could jump into unloaded Stage1 code.
     */
    recv_restore_callbacks();
    send_restore_callbacks();

    pr_info("stage1: unloaded (callbacks restored)\n");
}

module_init(stage1_init);
module_exit(stage1_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("PSVR2 Stage1 - Fast File Transfer + Command Execution");
