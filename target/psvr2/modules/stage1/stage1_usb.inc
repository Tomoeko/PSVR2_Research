/* Shared discovery for sieusb's /dev/usb_data3 backing object. */
static unsigned long s1_usb_data3_cdev(const char *operation)
{
    struct path usb_path;
    struct inode *inode;
    unsigned long address = 0;

    if (kern_path("/dev/usb_data3", LOOKUP_FOLLOW, &usb_path)) {
        snprintf(status_buf, STATUS_SIZE, "ERR %s kern_path", operation);
        return 0;
    }

    inode = usb_path.dentry->d_inode;
    if (inode && !inode->i_cdev && is_autoboot_disabled()) {
        struct file *dummy =
            filp_open("/dev/usb_data3", O_RDONLY | O_NONBLOCK, 0);

        if (!IS_ERR(dummy))
            filp_close(dummy, NULL);
    }
    if (inode && inode->i_cdev)
        address = (unsigned long)inode->i_cdev;

    path_put(&usb_path);
    if (!address)
        snprintf(status_buf, STATUS_SIZE, "ERR %s no cdev", operation);
    return address;
}

static struct s1_gbuf *s1_usb_gbuf(unsigned long cdev_address)
{
    /*
     * Both exact supported sieusb builds embed gbuf at private+0xf8 and
     * struct cdev at private+0x178. inode->i_cdev points at the latter.
     */
    struct s1_gbuf *gbuf =
        (struct s1_gbuf *)((char *)cdev_address - 0x80);

    /* gbuf_init() names this exact instance "bulkintr" on both families. */
    if (strncmp((char *)gbuf + 0x50, "bulkintr", 8) != 0)
        return NULL;
    return gbuf;
}

#define S1_USB_MAX_ITEMS 128
#define S1_USB_DMABUF_STRIDE 80

typedef void (*s1_usb_complete_fn)(struct usb_ep *,
                                   struct usb_request *);

struct s1_usb_requests {
    int count;
    char *dmabufs;
    struct usb_ep *ep;
    size_t packet_size;
};

static bool s1_usb_requests(void *gbuf, const char *operation,
                            struct s1_usb_requests *requests)
{
    int count;
    char *dmabufs;
    struct usb_ep *ep;
    size_t packet_size;

    if (!gbuf || !requests)
        return false;
    count = *(int *)((char *)gbuf + GO_ITEMS);
    dmabufs = *(char **)((char *)gbuf + GO_DMABUFS);
    ep = GO_EPPTR(gbuf);
    packet_size = GO_PKTLEN(gbuf);
    if (count <= 0 || count > S1_USB_MAX_ITEMS || !dmabufs || !ep ||
        !packet_size || packet_size > (1U << 20)) {
        snprintf(status_buf, STATUS_SIZE,
                 "ERR %s invalid USB request set", operation);
        return false;
    }
    requests->count = count;
    requests->dmabufs = dmabufs;
    requests->ep = ep;
    requests->packet_size = packet_size;
    return true;
}

static void *s1_usb_direction(struct s1_gbuf *gbuf, bool want_in,
                              const char *operation)
{
    void *candidate;
    struct usb_ep *ep;

    if (!gbuf) {
        snprintf(status_buf, STATUS_SIZE,
                 "ERR %s invalid bulkintr gbuf", operation);
        return NULL;
    }

    /* gbuf_init stores source/IN at +0 and sink/OUT at +8. */
    candidate = want_in ? gbuf->in : gbuf->out;
    if (!candidate) {
        snprintf(status_buf, STATUS_SIZE,
                 "ERR %s missing %s gbuf", operation,
                 want_in ? "IN" : "OUT");
        return NULL;
    }
    ep = GO_EPPTR(candidate);
    if (!ep || !ep->desc ||
        !!usb_endpoint_dir_in(ep->desc) != want_in) {
        snprintf(status_buf, STATUS_SIZE,
                 "ERR %s unexpected endpoint direction", operation);
        return NULL;
    }
    return candidate;
}

static struct usb_request *s1_usb_request_at(
    const struct s1_usb_requests *requests, int index)
{
    char *entry;

    if (!requests || index < 0 || index >= requests->count)
        return NULL;
    entry = requests->dmabufs + S1_USB_DMABUF_STRIDE * index;
    return *(struct usb_request **)(entry + DMA_REQ);
}

static int s1_usb_validate_callbacks(
    const struct s1_usb_requests *requests, const char *operation,
    s1_usb_complete_fn *original)
{
    s1_usb_complete_fn observed = NULL;
    int index;

    /* Validate every request before changing any callback. */
    for (index = 0; index < requests->count; ++index) {
        struct usb_request *request =
            s1_usb_request_at(requests, index);
        s1_usb_complete_fn callback =
            request ? READ_ONCE(request->complete) : NULL;

        if (!callback) {
            snprintf(status_buf, STATUS_SIZE,
                     "ERR %s invalid request[%d]", operation, index);
            return -EINVAL;
        }
        if (!observed)
            observed = callback;
        else if (callback != observed) {
            snprintf(status_buf, STATUS_SIZE,
                     "ERR %s mixed callbacks", operation);
            return -EINVAL;
        }
    }
    *original = observed;
    return 0;
}

static int s1_usb_replace_callbacks(
    void *gbuf, const char *operation, s1_usb_complete_fn replacement,
    s1_usb_complete_fn *original)
{
    struct s1_usb_requests requests;
    s1_usb_complete_fn observed;
    int index;

    if (!replacement || !original ||
        !s1_usb_requests(gbuf, operation, &requests) ||
        s1_usb_validate_callbacks(&requests, operation, &observed))
        return -EINVAL;

    /* An endpoint can complete as soon as its first callback is replaced. */
    WRITE_ONCE(*original, observed);
    smp_wmb();
    for (index = 0; index < requests.count; ++index)
        WRITE_ONCE(s1_usb_request_at(&requests, index)->complete,
                   replacement);
    return requests.count;
}

static int s1_usb_restore_callbacks(
    void *gbuf, s1_usb_complete_fn replacement,
    s1_usb_complete_fn original)
{
    struct s1_usb_requests requests;
    int restored = 0;
    int index;

    if (!replacement || !original ||
        !s1_usb_requests(gbuf, "restore", &requests))
        return 0;
    for (index = 0; index < requests.count; ++index) {
        struct usb_request *request =
            s1_usb_request_at(&requests, index);

        if (request && READ_ONCE(request->complete) == replacement) {
            WRITE_ONCE(request->complete, original);
            ++restored;
        }
    }
    /* MTU3 invokes native completions with IRQs disabled, including dequeue.
     * Keep forwarding handlers and gbuf state alive until old calls finish. */
    synchronize_sched();
    return restored;
}

static int s1_usb_prepare_in(void *gbuf, const char *operation)
{
    struct s1_usb_requests requests;
    s1_usb_complete_fn original;
    int index;
    int dequeued = 0;
    int busy;

    if (!s1_usb_requests(gbuf, operation, &requests) ||
        s1_usb_validate_callbacks(&requests, operation, &original))
        return -EINVAL;
    if (!requests.ep->desc ||
        !usb_endpoint_dir_in(requests.ep->desc)) {
        snprintf(status_buf, STATUS_SIZE,
                 "ERR %s unexpected endpoint direction", operation);
        return -EINVAL;
    }

    for (index = 0; index < requests.count; ++index) {
        struct usb_request *request = s1_usb_request_at(&requests, index);
        int error = usb_ep_dequeue(requests.ep, request);

        if (!error)
            ++dequeued;
        else if (error != -EINVAL) {
            snprintf(status_buf, STATUS_SIZE,
                     "ERR %s dequeue[%d] %d", operation, index, error);
            return error;
        }
    }
    synchronize_sched();
    msleep(200);
    /* Optional controller operation; the host still drains stale payload. */
    usb_ep_fifo_flush(requests.ep);
    busy = atomic_read(&GO_BUSYCNT(gbuf));
    if (busy != 0) {
        snprintf(status_buf, STATUS_SIZE,
                 "ERR %s busy=%d after dequeued=%d/%d", operation,
                 busy, dequeued, requests.count);
        return -EBUSY;
    }

    /* This is a snapshot. Paused writers and a host timeout establish idle. */
    snprintf(status_buf, STATUS_SIZE,
             "OK %s requests=%d dequeued=%d busy=0", operation,
             requests.count, dequeued);
    return 0;
}
