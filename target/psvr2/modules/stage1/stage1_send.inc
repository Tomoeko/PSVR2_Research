/*
 * Send (fast_download): device -> host via USB bulk IN
 */

/* Send state */
static struct task_struct *send_task;
static struct file *send_in_fp;
static volatile u64 send_total;
static volatile u64 send_expected;
static volatile int send_running;
static volatile int send_error;
static volatile int send_reflection;
static void *send_gbuf_in;
static char send_path[MAX_PATH_LEN];

/* Send uses single-request synchronous approach:
 * thread fills buffer -> queues -> waits for completion -> repeats */
static DECLARE_WAIT_QUEUE_HEAD(send_done_wq);
static volatile int send_req_done;

/* Callback state */
static void (*send_orig_complete)(struct usb_ep *, struct usb_request *);
static struct usb_request *send_my_req = NULL;

/* Send completion callback */
static void send_complete_cb(struct usb_ep *ep, struct usb_request *req)
{
    s1_usb_complete_fn original;

    /* Our request remains ours even after send_stop clears send_running.
     * Forwarding it into sieusb's native completion path corrupts its request
     * bookkeeping and can leave a callback targeting freed module text. */
    if (req == READ_ONCE(send_my_req)) {
        smp_wmb();
        WRITE_ONCE(send_req_done, 1);
        wake_up(&send_done_wq);
        return;
    }

    /* Forward sieusb's native telemetry requests to their original handler. */
    smp_rmb();
    original = READ_ONCE(send_orig_complete);
    if (original)
        original(ep, req);
}

/* Send worker: read the file and push it to USB IN */
static int send_thread_fn(void *data)
{
    struct s1_usb_requests requests;
    struct usb_ep *ep = NULL;
    size_t pktlen = 0;

    struct usb_request *(*alloc_req)(struct usb_ep *, gfp_t) = NULL;
    void (*free_req)(struct usb_ep *, struct usb_request *) = NULL;
    struct usb_request *my_req = NULL;
    void *my_buf = NULL;
    loff_t pos = 0;

    (void)data;
    send_total = 0;
    send_error = 0;
    if (!s1_usb_requests(send_gbuf_in, "send", &requests)) {
        send_error = -EINVAL;
        goto out;
    }
    ep = requests.ep;
    pktlen = requests.packet_size;
    alloc_req = *(void **)(*(void **)((char *)ep + 16) + 16);
    free_req = *(void **)(*(void **)((char *)ep + 16) + 24);

    /* Allocate our own request and buffer to avoid interfering with sieusb */
    my_req = alloc_req(ep, GFP_KERNEL);
    if (!my_req) {
        send_error = -ENOMEM;
        snprintf(status_buf, STATUS_SIZE, "ERR send alloc_req");
        goto out;
    }

    my_buf = kmalloc(pktlen, GFP_KERNEL);
    if (!my_buf) {
        send_error = -ENOMEM;
        snprintf(status_buf, STATUS_SIZE, "ERR send kmalloc");
        goto out;
    }

    my_req->buf = my_buf;
    my_req->complete = send_complete_cb;
    my_req->context = NULL;
    send_my_req = my_req;

    /* Recheck native IN requests before queuing the owned send request. */
    send_error = s1_usb_prepare_in(send_gbuf_in, "send");
    if (send_error) {
        goto out;
    }

    /* Reflection events are generated in-kernel and intentionally never
     * touch the HID exploit transport while stock authentication is active. */
    if (!send_reflection) {
        send_in_fp = filp_open(send_path, O_RDONLY, 0);
        if (IS_ERR(send_in_fp)) {
            send_error = PTR_ERR(send_in_fp);
            send_in_fp = NULL;
            /* Don't overwrite clr status if it's already an error, but here we must report open error */
            snprintf(status_buf, STATUS_SIZE,
                     "ERR send open %d %s", send_error, send_path);
            goto out;
        }
    }

    while (send_running && send_total < send_expected) {
        ssize_t nread;
        size_t chunk = send_expected - send_total;
        int ret;

        if (chunk > pktlen)
            chunk = pktlen;

        /* Read from file into request buffer (4.4 signature) */
        if (send_reflection) {
            unsigned int event =
                (unsigned int)(send_total / S1_REFLECTION_EVENT_SIZE);

            nread = s1_auth_reflection_copy_event(
                event, my_buf, S1_REFLECTION_EVENT_SIZE);
            if (!nread)
                nread = S1_REFLECTION_EVENT_SIZE;
        } else {
            typedef int (*kr_fn_t)(struct file *, loff_t,
                                   char *, unsigned long);
            nread = ((kr_fn_t)kernel_read)(send_in_fp, pos, my_buf, chunk);
        }
        if (nread <= 0) {
            if (nread < 0)
                send_error = nread;
            snprintf(status_buf, STATUS_SIZE,
                     "ERR send kread %d buf=%px chunk=%zu",
                     (int)nread, my_buf, chunk);
            break;
        }
        pos += nread;

        /* Set request length and queue to IN endpoint */
        my_req->status = -EINPROGRESS;
        my_req->actual = 0;
        my_req->length = nread;
        WRITE_ONCE(send_req_done, 0);
        smp_wmb();

        ret = usb_ep_queue(ep, my_req, GFP_KERNEL);
        if (ret) {
            send_error = ret;
            snprintf(status_buf, STATUS_SIZE,
                     "ERR send epq %d ep=%px req=%px len=%d",
                     ret, ep, my_req, (int)nread);
            break;
        }

        snprintf(status_buf, STATUS_SIZE,
                 "WAIT send %llu/%llu st=%d act=%u",
                 (unsigned long long)send_total,
                 (unsigned long long)send_expected,
                 my_req->status, my_req->actual);
        ret = wait_event_interruptible_timeout(
            send_done_wq,
            READ_ONCE(send_req_done) || !READ_ONCE(send_running) ||
                kthread_should_stop(),
            msecs_to_jiffies(60000));

        if (!READ_ONCE(send_req_done)) {
            int dq_ret = usb_ep_dequeue(ep, my_req);
            /*
             * A queued request must complete before its request, buffer, or
             * callback code can be released. usb_ep_dequeue completion is
             * asynchronous on this controller.
             */
            wait_event(send_done_wq, READ_ONCE(send_req_done));
            send_error = ret == 0 ? -ETIMEDOUT :
                         (ret < 0 ? ret : -ECANCELED);
            snprintf(status_buf, STATUS_SIZE,
                     "ERR send wait %d dq=%d st=%d act=%u",
                     send_error, dq_ret,
                     my_req->status, my_req->actual);
            break;
        }

        smp_rmb();
        if (my_req->status != 0 || my_req->actual != nread) {
            send_error = my_req->status ?
                my_req->status : -EREMOTEIO;
            snprintf(status_buf, STATUS_SIZE,
                     "ERR send complete %d act=%u expected=%d",
                     my_req->status, my_req->actual, (int)nread);
            break;
        }

        send_total += my_req->actual;
        cond_resched();
    }

out:
    if (send_error) {
        /* Only set generic if no specific diag was written above */
        if (strncmp(status_buf, "ERR send ", 9) != 0)
            snprintf(status_buf, STATUS_SIZE,
                     "ERR send %d at %llu/%llu",
                     send_error,
                     (unsigned long long)send_total,
                     (unsigned long long)send_expected);
    } else {
        snprintf(status_buf, STATUS_SIZE,
                 "OK send done %llu",
                 (unsigned long long)send_total);
    }

    if (send_in_fp) {
        filp_close(send_in_fp, NULL);
        send_in_fp = NULL;
    }

    if (my_req) {
        WRITE_ONCE(send_my_req, NULL);
        smp_wmb();
        if (free_req)
            free_req(ep, my_req);
    }
    if (my_buf)
        kfree(my_buf);

    send_running = 0;
    send_reflection = 0;
    send_task = NULL;
    return 0;
}

static void send_stop(void)
{
    send_running = 0;
    smp_wmb();
    wake_up(&send_done_wq);
    if (send_task) {
        kthread_stop(send_task);
        send_task = NULL;
    }
}

static int send_prepare(void)
{
    unsigned long cdev_address;
    struct s1_gbuf *gbuf;
    void *input;

    send_stop();
    cdev_address = s1_usb_data3_cdev("send_prepare");
    if (!cdev_address)
        return -ENODEV;
    gbuf = s1_usb_gbuf(cdev_address);
    input = s1_usb_direction(gbuf, true, "send_prepare");
    if (!input)
        return -ENODEV;
    /* Keep native callbacks intact; no persistent hook is installed here. */
    return s1_usb_prepare_in(input, "send_prepare");
}

static void send_restore_callbacks(void)
{
    int restored;

    if (!send_gbuf_in || !send_orig_complete)
        return;
    restored = s1_usb_restore_callbacks(
        send_gbuf_in, send_complete_cb, send_orig_complete);
    pr_info("stage1: restored %d send callbacks\n", restored);
    send_gbuf_in = NULL;
    send_orig_complete = NULL;
}

/* Send USB setup: find gbuf->in and patch callbacks */
static ssize_t send_setup_usb(size_t count)
{
    unsigned long cdev_addr_local = s1_usb_data3_cdev("send");

    (void)count;

    if (!cdev_addr_local)
        return -ENODEV;

    if (!send_gbuf_in) {
        /* First send: patch IN-side completion callbacks */
        struct s1_gbuf *gbuf_ptr = s1_usb_gbuf(cdev_addr_local);
        int replaced;

        send_gbuf_in = s1_usb_direction(gbuf_ptr, true, "send");
        if (!send_gbuf_in)
            return -ENODEV;

        send_orig_complete = NULL;
        replaced = s1_usb_replace_callbacks(
            send_gbuf_in, "send", send_complete_cb,
            &send_orig_complete);
        if (replaced < 0) {
            send_gbuf_in = NULL;
            send_orig_complete = NULL;
            return replaced;
        }
    }

    /* Start send thread */
    send_running = 1;
    send_req_done = 0;
    send_task = kthread_run(send_thread_fn, NULL, "stage1_send");
    if (IS_ERR(send_task)) {
        snprintf(status_buf, STATUS_SIZE, "ERR send kthread %ld",
                 PTR_ERR(send_task));
        send_running = 0;
        send_task = NULL;
        send_restore_callbacks();
        return -ECHILD;
    }

    return 0;  /* 0 = success */
}
