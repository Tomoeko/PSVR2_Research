/* Snapshot counters without taking the ring or tty I/O locks. */
struct s3_input_diagnostics {
    bool opened;
    bool reading;
    int open_error;
    int raw_ioctl;
    int last_read;
    int fatal;
    u64 read_calls;
    u64 read_returns;
    u64 read_bytes;
    u64 read_again;
    u64 read_eof;
    u64 tty_hangups;
    u64 ring_accepted;
    u64 ring_dropped;
};

static struct s3_input_diagnostics input_diagnostics;
static DEFINE_SPINLOCK(input_diagnostics_lock);

static void s3_input_diagnostics_reset(void)
{
    unsigned long flags;
    spin_lock_irqsave(&input_diagnostics_lock, flags);
    memset(&input_diagnostics, 0, sizeof(input_diagnostics));
    input_diagnostics.raw_ioctl = -EINPROGRESS;
    spin_unlock_irqrestore(&input_diagnostics_lock, flags);
}

static void s3_input_diagnostics_open(bool opened, int error, int raw_ioctl)
{
    unsigned long flags;
    spin_lock_irqsave(&input_diagnostics_lock, flags);
    input_diagnostics.opened = opened;
    input_diagnostics.open_error = error;
    input_diagnostics.raw_ioctl = raw_ioctl;
    spin_unlock_irqrestore(&input_diagnostics_lock, flags);
}

static void s3_input_diagnostics_closed(void)
{
    unsigned long flags;
    spin_lock_irqsave(&input_diagnostics_lock, flags);
    input_diagnostics.opened = false;
    input_diagnostics.reading = false;
    spin_unlock_irqrestore(&input_diagnostics_lock, flags);
}

static void s3_input_diagnostics_read_begin(void)
{
    unsigned long flags;
    spin_lock_irqsave(&input_diagnostics_lock, flags);
    ++input_diagnostics.read_calls;
    input_diagnostics.reading = true;
    spin_unlock_irqrestore(&input_diagnostics_lock, flags);
}

static void s3_input_diagnostics_read_end(int result)
{
    unsigned long flags;
    spin_lock_irqsave(&input_diagnostics_lock, flags);
    ++input_diagnostics.read_returns;
    input_diagnostics.reading = false;
    input_diagnostics.last_read = result;
    if (result > 0)
        input_diagnostics.read_bytes += (u64)result;
    else if (result == -EAGAIN || result == -EINTR)
        ++input_diagnostics.read_again;
    else if (!result)
        ++input_diagnostics.read_eof;
    else
        input_diagnostics.fatal = result;
    spin_unlock_irqrestore(&input_diagnostics_lock, flags);
}

/* Reconfiguration permanently replaces this descriptor's tty file ops.
 * Healthy nonblocking idle reads must not trigger close/reopen cycles. */
static bool s3_input_read_should_reopen(struct file *file, int result,
                                        int *stream_length)
{
    unsigned long flags;
    if (result > 0 || !tty_hung_up_p(file))
        return false;
    *stream_length = 0;
    spin_lock_irqsave(&input_diagnostics_lock, flags);
    ++input_diagnostics.tty_hangups;
    spin_unlock_irqrestore(&input_diagnostics_lock, flags);
    return true;
}

static void s3_input_diagnostics_ring(bool accepted)
{
    unsigned long flags;
    spin_lock_irqsave(&input_diagnostics_lock, flags);
    if (accepted)
        ++input_diagnostics.ring_accepted;
    else
        ++input_diagnostics.ring_dropped;
    spin_unlock_irqrestore(&input_diagnostics_lock, flags);
}

static struct s3_input_diagnostics s3_input_diagnostics_snapshot(void)
{
    struct s3_input_diagnostics snapshot;
    unsigned long flags;
    spin_lock_irqsave(&input_diagnostics_lock, flags);
    snapshot = input_diagnostics;
    spin_unlock_irqrestore(&input_diagnostics_lock, flags);
    return snapshot;
}
