/* Receive state */
static struct task_struct *recv_task;
static struct file *recv_out_fp;
static volatile u64 recv_total;
static volatile u64 recv_expected;
static volatile int recv_running;
static volatile int recv_error;
static void *recv_gbuf_out;
static char recv_path[MAX_PATH_LEN];

/* Multi-file receive state */
#define RECV_MULTI_MAX 8
static char   recv_multi_paths[RECV_MULTI_MAX][MAX_PATH_LEN];
static u64    recv_multi_sizes[RECV_MULTI_MAX];
static int    recv_multi_count;
static volatile int recv_multi_cur;
static volatile u64 recv_grand_total;
static volatile u64 recv_grand_expected;

/* Ring buffer: 32 slots x 16KB = 512KB (IRQ callback -> thread)
 * Needs to be large enough so the writer thread can drain to file
 * without the IRQ callback dropping data at ~50 MB/s USB speed. */
#define RECV_RING_SLOTS 32
#define RECV_RING_BUFSZ 16384
static char recv_ring_data[RECV_RING_SLOTS][RECV_RING_BUFSZ];
static size_t recv_ring_len[RECV_RING_SLOTS];
static volatile int recv_ring_head;
static volatile int recv_ring_tail;
static DECLARE_WAIT_QUEUE_HEAD(recv_ring_wq);

/* Callback state */
static void (*recv_orig_complete)(struct usb_ep *, struct usb_request *);
static atomic_t recv_cb_count = ATOMIC_INIT(0);
static atomic_t recv_total_cb = ATOMIC_INIT(0);
static atomic_t recv_queue_errs = ATOMIC_INIT(0);
static atomic_t recv_ring_drops = ATOMIC_INIT(0);
static atomic_t recv_ring_full_bp = ATOMIC_INIT(0);  /* ring-full backpressure events */
static volatile int recv_stop_requeue;

/* Receive completion callback */
static void recv_complete_cb(struct usb_ep *ep, struct usb_request *req)
{
    atomic_inc(&recv_total_cb);

    if (!recv_running) {
        s1_usb_complete_fn original;

        /* Pair with original-handler publication before callback install. */
        smp_rmb();
        original = READ_ONCE(recv_orig_complete);
        if (original)
            original(ep, req);
        return;
    }

    {
        int status = req->status;
        unsigned int actual = req->actual;
        int head, next;

        atomic_dec(&GO_BUSYCNT(recv_gbuf_out));

        if (!status && actual > 0) {
            head = recv_ring_head;
            next = (head + 1) % RECV_RING_SLOTS;

            if (next != recv_ring_tail) {
                size_t copy = actual;
                if (copy > RECV_RING_BUFSZ)
                    copy = RECV_RING_BUFSZ;
                memcpy(recv_ring_data[head], req->buf, copy);
                recv_ring_len[head] = copy;
                smp_wmb();
                recv_ring_head = next;
            } else {
                /* Ring full - data dropped. */
                atomic_inc(&recv_ring_drops);
                atomic_inc(&recv_ring_full_bp);
            }
        }

        if (!recv_stop_requeue) {
            req->length = GO_PKTLEN(recv_gbuf_out);
            atomic_inc(&GO_BUSYCNT(recv_gbuf_out));
            {
                int qret = usb_ep_queue(ep, req, GFP_ATOMIC);
                if (qret < 0) {
                    atomic_dec(&GO_BUSYCNT(recv_gbuf_out));
                    atomic_inc(&recv_queue_errs);
                }
            }
        }

        atomic_inc(&recv_cb_count);
        wake_up(&recv_ring_wq);
    }
}

/* Receive worker */
static int recv_thread_fn(void *data)
{
    loff_t out_pos = 0;
    int file_idx = 0;
    int total_files = recv_multi_count > 0 ? recv_multi_count : 1;
    int wait_count = 0;

    (void)data;

    /* Open first output file */
    {
        const char *path = recv_multi_count > 0
            ? recv_multi_paths[0] : recv_path;
        recv_out_fp = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
        if (IS_ERR(recv_out_fp)) {
            snprintf(status_buf, STATUS_SIZE, "ERR recv open[0] %ld",
                     PTR_ERR(recv_out_fp));
            recv_out_fp = NULL;
            recv_error = -ENOENT;
            recv_running = 0;
            recv_task = NULL;
            return 0;
        }
        recv_multi_cur = 0;
        recv_expected = recv_multi_count > 0
            ? recv_multi_sizes[0] : recv_expected;
        recv_total = 0;
        out_pos = 0;
    }

    while (!kthread_should_stop() && recv_running &&
           recv_grand_total < recv_grand_expected) {
        int tail;
        size_t n;

        /* Advance to next file if current is done */
        if (recv_total >= recv_expected && file_idx < total_files - 1) {
            if (recv_out_fp) {
                filp_close(recv_out_fp, NULL);
                recv_out_fp = NULL;
            }
            file_idx++;
            recv_multi_cur = file_idx;

            {
                const char *next = recv_multi_paths[file_idx];
                recv_out_fp = filp_open(next,
                    O_WRONLY | O_CREAT | O_TRUNC, 0755);
                if (IS_ERR(recv_out_fp)) {
                    snprintf(status_buf, STATUS_SIZE,
                        "ERR recv open[%d] %ld",
                        file_idx, PTR_ERR(recv_out_fp));
                    recv_out_fp = NULL;
                    recv_error = -ENOENT;
                    break;
                }
            }
            recv_expected = recv_multi_sizes[file_idx];
            recv_total = 0;
            out_pos = 0;
            wait_count = 0;

            snprintf(status_buf, STATUS_SIZE,
                     "OK recv file[%d/%d] %llu/%llu",
                     file_idx + 1, total_files,
                     (unsigned long long)recv_grand_total,
                     (unsigned long long)recv_grand_expected);
            continue;
        }

        if (recv_total >= recv_expected && file_idx >= total_files - 1)
            break;

        /* Increased from 500ms to 2000ms:
         * During stage3_serial insmod, the recv thread may not get CPU
         * for >500ms due to USB gadget configuration spinlocks and DWC3
         * controller re-initialization.  2s is generous enough to avoid
         * false timeouts during multi-file recv with concurrent insmod. */
        wait_event_interruptible_timeout(recv_ring_wq,
            recv_ring_head != recv_ring_tail || kthread_should_stop(),
            msecs_to_jiffies(2000));

        tail = recv_ring_tail;
        if (tail == recv_ring_head) {
            if (recv_grand_total == 0) {
                if (++wait_count > 40) {
                    recv_error = -ETIMEDOUT;
                    break;
                }
                snprintf(status_buf, STATUS_SIZE,
                    "OK recv waiting=%d", wait_count);
            }
            continue;
        }

        smp_rmb();
        n = recv_ring_len[tail];

        /* Clamp to current file's remaining bytes */
        {
            u64 file_remaining = recv_expected - recv_total;
            if (n > file_remaining)
                n = (size_t)file_remaining;
        }

        /* kernel_write may complete partially; retain data until all of the
         * selected ring chunk has been persisted. */
        {
            typedef ssize_t (*kw_fn_t)(struct file *, const char *,
                                       size_t, loff_t);
            size_t written = 0;

            while (written < n) {
                ssize_t w = ((kw_fn_t)kernel_write)(
                    recv_out_fp, recv_ring_data[tail] + written,
                    n - written, out_pos);
                if (w <= 0) {
                    recv_error = w < 0 ? (int)w : -EIO;
                    snprintf(status_buf, STATUS_SIZE,
                        "ERR recv write[%d] %d", file_idx,
                        recv_error);
                    break;
                }
                written += (size_t)w;
                recv_total += w;
                recv_grand_total += w;
                out_pos += w;
            }
            if (recv_error)
                break;
        }

        /* Handle leftovers spanning file boundaries */
        {
            size_t orig_n = recv_ring_len[tail];
            if (n < orig_n) {
                size_t leftover = orig_n - n;
                memmove(recv_ring_data[tail],
                        recv_ring_data[tail] + n, leftover);
                recv_ring_len[tail] = leftover;
            } else {
                recv_ring_tail = (tail + 1) % RECV_RING_SLOTS;
            }
        }

        if ((recv_grand_total & 0xFFFF) == 0 ||
            recv_grand_total >= recv_grand_expected) {
            snprintf(status_buf, STATUS_SIZE,
                     "OK recv %llu/%llu file[%d/%d]",
                     (unsigned long long)recv_grand_total,
                     (unsigned long long)recv_grand_expected,
                     file_idx + 1, total_files);
        }

        cond_resched();
    }

    recv_stop_requeue = 1;
    smp_wmb();

    /* Close the file before updating status to prevent an ETXTBSY race
     * where host sees "done" and tries to execute while file is
     * still open for writing. */
    recv_running = 0;

    if (recv_out_fp) {
        filp_close(recv_out_fp, NULL);
        recv_out_fp = NULL;
    }

    if (recv_error) {
        snprintf(status_buf, STATUS_SIZE,
                 "ERR recv %d at %llu/%llu files=%d/%d cb=%d qe=%d rd=%d bp=%d",
                 recv_error,
                 (unsigned long long)recv_grand_total,
                 (unsigned long long)recv_grand_expected,
                 file_idx + 1, total_files,
                 atomic_read(&recv_cb_count),
                 atomic_read(&recv_queue_errs),
                 atomic_read(&recv_ring_drops),
                 atomic_read(&recv_ring_full_bp));
    } else {
        snprintf(status_buf, STATUS_SIZE,
                 "OK recv done %llu files=%d cb=%d tcb=%d qe=%d rd=%d bp=%d",
                 (unsigned long long)recv_grand_total,
                 total_files,
                 atomic_read(&recv_cb_count),
                 atomic_read(&recv_total_cb),
                 atomic_read(&recv_queue_errs),
                 atomic_read(&recv_ring_drops),
                 atomic_read(&recv_ring_full_bp));
    }

    recv_task = NULL;
    return 0;
}

static void recv_stop(void)
{
    recv_running = 0;
    smp_wmb();
    if (recv_task) {
        kthread_stop(recv_task);
        recv_task = NULL;
    }
}

static void recv_restore_callbacks(void)
{
    int restored;

    if (!recv_gbuf_out || !recv_orig_complete)
        return;
    restored = s1_usb_restore_callbacks(
        recv_gbuf_out, recv_complete_cb, recv_orig_complete);
    pr_info("stage1: restored %d recv callbacks\n", restored);
    recv_gbuf_out = NULL;
    recv_orig_complete = NULL;
}

/* Shared USB endpoint setup for recv and recv_multi */
static ssize_t recv_setup_usb(size_t count)
{
    unsigned long cdev_addr_local = s1_usb_data3_cdev("recv");

    (void)count;

    if (!cdev_addr_local)
        return -ENODEV;

    if (!recv_gbuf_out) {
        /* First upload: patch completion callbacks */
        struct s1_gbuf *gbuf_ptr = s1_usb_gbuf(cdev_addr_local);
        int replaced;

        recv_gbuf_out = s1_usb_direction(gbuf_ptr, false, "recv");
        if (!recv_gbuf_out)
            return -ENODEV;

        recv_orig_complete = NULL;
        recv_ring_head = 0;
        recv_ring_tail = 0;
        recv_stop_requeue = 0;
        atomic_set(&recv_cb_count, 0);
        replaced = s1_usb_replace_callbacks(
            recv_gbuf_out, "recv", recv_complete_cb,
            &recv_orig_complete);
        if (replaced < 0) {
            recv_gbuf_out = NULL;
            recv_orig_complete = NULL;
            return replaced;
        }
    } else {
        /* Second+ upload: dequeue stale + re-queue */
        struct s1_usb_requests requests;
        int queued = 0;
        int qi;

        if (!s1_usb_requests(recv_gbuf_out, "recv", &requests)) {
            recv_restore_callbacks();
            return -EINVAL;
        }

        recv_ring_head = 0;
        recv_ring_tail = 0;
        atomic_set(&recv_cb_count, 0);
        atomic_set(&recv_total_cb, 0);
        atomic_set(&recv_queue_errs, 0);
        atomic_set(&recv_ring_drops, 0);

        recv_running = 1;
        recv_task = kthread_run(recv_thread_fn, NULL, "stage1_recv");
        if (IS_ERR(recv_task)) {
            snprintf(status_buf, STATUS_SIZE, "ERR recv kthread %ld",
                     PTR_ERR(recv_task));
            recv_running = 0;
            recv_task = NULL;
            recv_restore_callbacks();
            return -ECHILD;
        }

        for (qi = 0; qi < requests.count; qi++) {
            struct usb_request *request =
                s1_usb_request_at(&requests, qi);

            if (request)
                usb_ep_dequeue(requests.ep, request);
        }

        recv_stop_requeue = 0;
        smp_wmb();

        for (qi = 0; qi < requests.count; qi++) {
            struct usb_request *ureq =
                s1_usb_request_at(&requests, qi);
            int ret;

            if (!ureq)
                continue;
            ureq->length = requests.packet_size;
            atomic_inc(&GO_BUSYCNT(recv_gbuf_out));
            ret = usb_ep_queue(requests.ep, ureq, GFP_KERNEL);
            if (ret < 0) {
                atomic_dec(&GO_BUSYCNT(recv_gbuf_out));
                atomic_inc(&recv_queue_errs);
            } else {
                ++queued;
            }
        }
        if (!queued) {
            snprintf(status_buf, STATUS_SIZE,
                     "ERR recv could not queue USB requests");
            recv_stop();
            recv_restore_callbacks();
            return -EIO;
        }
    }

    /* First upload: start thread after callback setup */
    if (!recv_running) {
        recv_running = 1;
        recv_task = kthread_run(recv_thread_fn, NULL, "stage1_recv");
        if (IS_ERR(recv_task)) {
            snprintf(status_buf, STATUS_SIZE, "ERR recv kthread %ld",
                     PTR_ERR(recv_task));
            recv_running = 0;
            recv_task = NULL;
            recv_restore_callbacks();
            return -ECHILD;
        }
    }

    if (recv_multi_count > 0) {
        snprintf(status_buf, STATUS_SIZE,
                 "OK recv_multi started %d files %llu bytes",
                 recv_multi_count,
                 (unsigned long long)recv_grand_expected);
    } else {
        snprintf(status_buf, STATUS_SIZE, "OK recv started %s %llu",
                 recv_path,
                 (unsigned long long)recv_grand_expected);
    }

    return 0;  /* 0 = success, caller returns count */
}
