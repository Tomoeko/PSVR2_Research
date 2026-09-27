#define FAST_INPUT_BUFSZ 1024
#define FAST_INPUT_NBUFS 16

static struct usb_ep *fast_in_ep;
static struct usb_request *fast_in_reqs[FAST_INPUT_NBUFS];
/* Ring storage is owned independently from optional USB hardware requests. */
static unsigned char *fast_in_buffers[FAST_INPUT_NBUFS];
static size_t fast_in_lengths[FAST_INPUT_NBUFS];
static wait_queue_head_t fast_in_wait;
static bool fast_in_wait_initialized;
static int fast_in_head, fast_in_tail;
static bool fast_in_ready;
static bool fast_in_registered;
static bool fast_in_stopping;
static DEFINE_MUTEX(fast_in_io_mutex);
static bool fast_in_bridge_mode; /* true = tty ring, optional HW requests dequeued */
static bool fast_in_done[FAST_INPUT_NBUFS];
static DEFINE_SPINLOCK(fast_in_lock);

static bool fast_input_has_data(void) {
  bool ready;
  unsigned long flags;

  spin_lock_irqsave(&fast_in_lock, flags);
  ready = fast_in_bridge_mode
      ? fast_in_tail != fast_in_head
      : fast_in_done[fast_in_tail];
  spin_unlock_irqrestore(&fast_in_lock, flags);
  return ready;
}

static void fast_input_complete_cb(struct usb_ep *ep,
                                   struct usb_request *req) {
  int idx = (int)(long)req->context;
  unsigned long flags;

  if (idx < 0 || idx >= FAST_INPUT_NBUFS)
    return;

  /* A queued request may finish while setup is still unpublished. Retain
   * that completion so its slot is readable after the final publication. */
  spin_lock_irqsave(&fast_in_lock, flags);
  if (fast_in_ep == ep && fast_in_reqs[idx] == req &&
      !fast_in_bridge_mode && req->status == 0 &&
      req->actual <= FAST_INPUT_BUFSZ) {
    fast_in_lengths[idx] = req->actual;
    fast_in_done[idx] = true;
  }
  spin_unlock_irqrestore(&fast_in_lock, flags);
  wake_up_interruptible(&fast_in_wait);
}

static int fast_in_open(struct inode *inode, struct file *file) {
  if (!smp_load_acquire(&fast_in_ready)) return -ENODEV;
  return 0;
}

static ssize_t fast_in_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
  int idx;
  int ret;
  size_t len;
  unsigned long flags;

  if (!smp_load_acquire(&fast_in_ready)) return -ENODEV;

  if (!fast_input_has_data()) {
    if (file->f_flags & O_NONBLOCK) return -EAGAIN;
    if (wait_event_interruptible(
            fast_in_wait, !smp_load_acquire(&fast_in_ready) || fast_input_has_data()))
      return -ERESTARTSYS;
    if (!smp_load_acquire(&fast_in_ready))
      return -ENODEV;
  }

  mutex_lock(&fast_in_io_mutex);
  if (!smp_load_acquire(&fast_in_ready)) {
    ret = -ENODEV;
    goto unlock;
  }
  spin_lock_irqsave(&fast_in_lock, flags);
  if ((fast_in_bridge_mode && fast_in_tail == fast_in_head) ||
      (!fast_in_bridge_mode && !fast_in_done[fast_in_tail])) {
    spin_unlock_irqrestore(&fast_in_lock, flags);
    ret = -EAGAIN;
    goto unlock;
  }
  idx = fast_in_tail;
  len = fast_in_lengths[idx];
  spin_unlock_irqrestore(&fast_in_lock, flags);

  if (count < len) {
    ret = -EMSGSIZE;
    goto unlock;
  }
  if (copy_to_user(buf, fast_in_buffers[idx], len)) {
    ret = -EFAULT;
    goto unlock;
  }

  spin_lock_irqsave(&fast_in_lock, flags);
  fast_in_tail = (fast_in_tail + 1) % FAST_INPUT_NBUFS;
  fast_in_done[idx] = false;
  spin_unlock_irqrestore(&fast_in_lock, flags);

  if (!fast_in_bridge_mode) {
    fast_in_reqs[idx]->length = FAST_INPUT_BUFSZ;
    ret = usb_ep_queue(fast_in_ep, fast_in_reqs[idx], GFP_KERNEL);
    if (ret)
      printk(KERN_WARNING
             "stage3: fast_input: requeue %d failed: %d\n", idx, ret);
  }
  mutex_unlock(&fast_in_io_mutex);
  return len;

unlock:
  mutex_unlock(&fast_in_io_mutex);
  return ret;
}

static const struct file_operations fast_in_fops = {
  .owner = THIS_MODULE,
  .open = fast_in_open,
  .read = fast_in_read,
};

static struct miscdevice fast_in_misc = {
  .minor = MISC_DYNAMIC_MINOR,
  .name = "fast_input",
  .fops = &fast_in_fops,
};

int s3_fast_input_setup(struct usb_ep *ep) {
  int i, ret = -ENOMEM;
  mutex_lock(&fast_in_io_mutex);
  if (fast_in_stopping) {
    mutex_unlock(&fast_in_io_mutex);
    return -ENODEV;
  }
  if (smp_load_acquire(&fast_in_ready)) {
    ret = ep == fast_in_ep ? 0 : -EBUSY;
    mutex_unlock(&fast_in_io_mutex);
    return ret;
  }

  fast_in_ep = ep;
  init_waitqueue_head(&fast_in_wait);
  fast_in_wait_initialized = true;
  fast_in_head = 0;
  fast_in_tail = 0;
  memset(fast_in_done, 0, sizeof(fast_in_done));
  fast_in_bridge_mode = ep == NULL;
  memset(fast_in_lengths, 0, sizeof(fast_in_lengths));

  for (i = 0; i < FAST_INPUT_NBUFS; i++) {
    fast_in_buffers[i] = kmalloc(FAST_INPUT_BUFSZ, GFP_KERNEL);
    if (!fast_in_buffers[i]) goto fail;
  }
  if (ep) {
    for (i = 0; i < FAST_INPUT_NBUFS; i++) {
      fast_in_reqs[i] = usb_ep_alloc_request(ep, GFP_KERNEL);
      if (!fast_in_reqs[i]) goto fail;
      fast_in_reqs[i]->buf = fast_in_buffers[i];
      fast_in_reqs[i]->complete = fast_input_complete_cb;
      fast_in_reqs[i]->context = (void *)(long)i;
      fast_in_reqs[i]->length = FAST_INPUT_BUFSZ;
      ret = usb_ep_queue(ep, fast_in_reqs[i], GFP_KERNEL);
      if (ret) {
        printk(KERN_ERR "stage3: fast_input: queue %d failed: %d\n", i, ret);
        goto fail;
      }
    }
  }

  ret = misc_register(&fast_in_misc);
  if (ret) {
    printk(KERN_ERR "stage3: fast_input: misc_register failed: %d\n", ret);
    goto fail;
  }
  fast_in_registered = true;
  /* Publish only after every buffer and optional request is complete,
   * hardware queues succeeded, and misc registration is visible. The tty
   * worker pairs this release with acquire before accessing ring storage. */
  smp_store_release(&fast_in_ready, true);
  mutex_unlock(&fast_in_io_mutex);
  printk(KERN_INFO "stage3: /dev/fast_input ready (ep=%s, receiving %d×%dB)\n",
         ep ? ep->name : "ttyGS1 software ring", FAST_INPUT_NBUFS, FAST_INPUT_BUFSZ);
  return 0;

fail:
  smp_store_release(&fast_in_ready, false);
  for (i = 0; i < FAST_INPUT_NBUFS; i++) {
    if (fast_in_reqs[i]) {
      usb_ep_dequeue(ep, fast_in_reqs[i]);
      fast_in_reqs[i]->buf = NULL;
      usb_ep_free_request(ep, fast_in_reqs[i]);
      fast_in_reqs[i] = NULL;
    }
    kfree(fast_in_buffers[i]);
    fast_in_buffers[i] = NULL;
    fast_in_lengths[i] = 0;
  }
  fast_in_bridge_mode = false;
  fast_in_ep = NULL;
  mutex_unlock(&fast_in_io_mutex);
  return ret < 0 ? ret : -ENOMEM;
}

/* The software route is meaningful only with the explicit second ACM port
 * and a published bridge worker. Check under its lifetime publication lock. */
static int s3_fast_input_bridge_setup(void) {
  bool available;

  mutex_lock(&worker_lock);
  available = double_evict && active_acm_ports >= 2 && bridge_thread != NULL;
  mutex_unlock(&worker_lock);
  if (!available)
    return -ENODEV;
  return s3_fast_input_setup(NULL);
}

/* Called on every tty read iteration: ttyGS1 can predate `input N` setup. */
static bool s3_input_bridge_latch(void) {
  int i;
  unsigned long flags;

  if (!smp_load_acquire(&fast_in_ready))
    return false;
  mutex_lock(&fast_in_io_mutex);
  if (!smp_load_acquire(&fast_in_ready)) {
    mutex_unlock(&fast_in_io_mutex);
    return false;
  }
  if (!fast_in_bridge_mode) {
    if (!fast_in_ep) {
      mutex_unlock(&fast_in_io_mutex);
      return false;
    }
    /* Block readers/requeue with the I/O mutex and suppress IRQ completions
     * before canceling hardware requests. The ring is used only after all
     * dequeues have returned. Cleanup stops this worker before freeing it. */
    spin_lock_irqsave(&fast_in_lock, flags);
    fast_in_bridge_mode = true;
    spin_unlock_irqrestore(&fast_in_lock, flags);
    for (i = 0; i < FAST_INPUT_NBUFS; ++i)
      usb_ep_dequeue(fast_in_ep, fast_in_reqs[i]);
    spin_lock_irqsave(&fast_in_lock, flags);
    fast_in_head = 0;
    fast_in_tail = 0;
    memset(fast_in_done, 0, sizeof(fast_in_done));
    spin_unlock_irqrestore(&fast_in_lock, flags);
    printk(KERN_INFO "stage3: bridge mode ON — HW USB requests dequeued\n");
  }
  mutex_unlock(&fast_in_io_mutex);
  return true;
}

static bool s3_input_bridge_push(const unsigned char *packet) {
  unsigned long flags;
  int next_head;
  bool accepted = false;

  if (!smp_load_acquire(&fast_in_ready)) {
    s3_input_diagnostics_ring(false);
    return false;
  }
  spin_lock_irqsave(&fast_in_lock, flags);
  next_head = (fast_in_head + 1) % FAST_INPUT_NBUFS;
  if (fast_in_bridge_mode && next_head != fast_in_tail) {
    int idx = fast_in_head;
    memcpy(fast_in_buffers[idx], packet, 16);
    fast_in_lengths[idx] = 16;
    fast_in_head = next_head;
    accepted = true;
  }
  spin_unlock_irqrestore(&fast_in_lock, flags);
  s3_input_diagnostics_ring(accepted);
  if (accepted)
    wake_up_interruptible(&fast_in_wait);
  return accepted;
}

/* Drain complete CT16 frames and retain a possible one-byte header/split
 * packet. Every two-byte header probe requires two bytes in the buffer. */
static unsigned int s3_input_bridge_drain(unsigned char *stream, int *length) {
  int offset = 0;
  unsigned int accepted = 0;

  while (offset + 2 <= *length) {
    if (stream[offset] == 'C' && stream[offset + 1] == 'T') {
      if (offset + 16 > *length)
        break;
      if (s3_input_bridge_push(stream + offset))
        ++accepted;
      offset += 16;
    } else {
      ++offset;
    }
  }
  if (offset > 0) {
    *length -= offset;
    memmove(stream, stream + offset, *length);
  }
  return accepted;
}

struct s3_input_serial_snapshot {
    int line;
    bool connected;
    bool in_enabled;
    bool out_enabled;
    unsigned int out_maxpacket;
};

/* Official f_acm embeds gserial as its first field. Read only the published
 * u_serial.h structures, and reject absent or mismatched function instances.
 * Private gs_port fields and notification endpoint offsets are not inferred. */
static struct s3_input_serial_snapshot s3_input_serial_snapshot(void)
{
    struct s3_input_serial_snapshot snapshot = {.line = -1};
    struct f_serial_opts *options;
    struct gserial *serial;
    struct usb_function function;
    struct gserial view;
    struct usb_ep endpoint;
    u8 line;

    if (active_acm_ports < 2 || !acm_inst[1] || !acm_func[1])
        return snapshot;
    options = container_of(acm_inst[1], struct f_serial_opts, func_inst);
    if (probe_kernel_read(&line, &options->port_num, sizeof(line)) ||
        line >= MAX_U_SERIAL_PORTS)
        return snapshot;
    snapshot.line = line;
    serial = container_of(acm_func[1], struct gserial, func);
    if (probe_kernel_read(&function, &serial->func, sizeof(function)) ||
        function.fi != acm_inst[1] ||
        probe_kernel_read(&view, serial, sizeof(view)))
        return snapshot;
    snapshot.connected = view.ioport != NULL;
    if (view.in && !probe_kernel_read(&endpoint, view.in, sizeof(endpoint)))
        snapshot.in_enabled = endpoint.enabled;
    if (view.out && !probe_kernel_read(&endpoint, view.out, sizeof(endpoint))) {
        snapshot.out_enabled = endpoint.enabled;
        snapshot.out_maxpacket = endpoint.maxpacket;
    }
    return snapshot;
}

/* A diagnostic command never waits for tty reads or the reader I/O mutex. */
static void s3_input_bridge_status(void)
{
    struct s3_input_diagnostics snapshot = s3_input_diagnostics_snapshot();
    struct s3_input_serial_snapshot serial = s3_input_serial_snapshot();
    unsigned long flags;
    unsigned int pending;
    bool ready = smp_load_acquire(&fast_in_ready);
    spin_lock_irqsave(&fast_in_lock, flags);
    pending = (fast_in_head + FAST_INPUT_NBUFS - fast_in_tail) % FAST_INPUT_NBUFS;
    spin_unlock_irqrestore(&fast_in_lock, flags);
    s3_set_status("OK input opened=%u open_err=%d raw=%d reading=%u "
                  "read=%llu/%llu bytes=%llu again=%llu eof=%llu hangups=%llu "
                  "last=%d fatal=%d accepted=%llu dropped=%llu ready=%u pending=%u "
                  "line=%d connected=%u in=%u out=%u maxpacket=%u",
                  snapshot.opened, snapshot.open_error, snapshot.raw_ioctl, snapshot.reading,
                  (unsigned long long)snapshot.read_calls,
                  (unsigned long long)snapshot.read_returns,
                  (unsigned long long)snapshot.read_bytes,
                  (unsigned long long)snapshot.read_again,
                  (unsigned long long)snapshot.read_eof,
                  (unsigned long long)snapshot.tty_hangups,
                  snapshot.last_read, snapshot.fatal,
                  (unsigned long long)snapshot.ring_accepted,
                  (unsigned long long)snapshot.ring_dropped, ready, pending,
                  serial.line, serial.connected, serial.in_enabled,
                  serial.out_enabled, serial.out_maxpacket);
}
