#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define loff_t int64_t
#define __user
#define GFP_KERNEL 0
#define THIS_MODULE NULL
#define MISC_DYNAMIC_MINOR 0
#define KERN_INFO ""
#define KERN_ERR ""
#define KERN_WARNING ""
#define ERESTARTSYS 512
#define printk(...) ((void)0)
#define DEFINE_MUTEX(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define DEFINE_SPINLOCK(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define mutex_lock(mutex) assert(pthread_mutex_lock(mutex) == 0)
#define mutex_unlock(mutex) assert(pthread_mutex_unlock(mutex) == 0)
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; mutex_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); mutex_unlock(lock); } while (0)
#define spin_lock_irq(lock) mutex_lock(lock)
#define spin_unlock_irq(lock) mutex_unlock(lock)
#define smp_load_acquire(value) __atomic_load_n(value, __ATOMIC_ACQUIRE)
#define smp_store_release(value, input) __atomic_store_n(value, input, __ATOMIC_RELEASE)
#define wait_event_interruptible(queue, condition) ((void)(queue), (condition) ? 0 : 1)

typedef uint64_t u64;
typedef uint8_t u8;
typedef int wait_queue_head_t;
struct inode { int unused; };
struct file { int f_flags; bool hung_up; };
static int tty_hung_up_p(struct file *file) { return file->hung_up; }
struct usb_ep {
    const char *name;
    bool enabled;
    unsigned int maxpacket;
};
struct usb_function_instance { unsigned int identity; };
struct usb_function { const struct usb_function_instance *fi; };
struct f_serial_opts {
    struct usb_function_instance func_inst;
    u8 port_num;
};
struct gserial {
    struct usb_function func;
    void *ioport;
    struct usb_ep *in;
    struct usb_ep *out;
};
#define MAX_U_SERIAL_PORTS 4
#define container_of(pointer, type, member) ((type *)((char *)(pointer) - offsetof(type, member)))
static struct usb_function_instance *acm_inst[2];
static struct usb_function *acm_func[2];
static int probe_kernel_read(void *destination, const void *source, size_t size)
{
    if (!source) return -EFAULT;
    memcpy(destination, source, size);
    return 0;
}
struct usb_request {
    void *buf;
    void (*complete)(struct usb_ep *, struct usb_request *);
    void *context;
    unsigned int actual;
    unsigned int length;
    int status;
};
struct file_operations {
    void *owner;
    int (*open)(struct inode *, struct file *);
    ssize_t (*read)(struct file *, char *, size_t, loff_t *);
};
struct miscdevice {
    int minor;
    const char *name;
    const struct file_operations *fops;
};

static struct usb_request *usb_ep_alloc_request(struct usb_ep *ep, int flags);
static void usb_ep_free_request(struct usb_ep *ep, struct usb_request *request);
static int usb_ep_queue(struct usb_ep *ep, struct usb_request *request, int flags);
static int usb_ep_dequeue(struct usb_ep *ep, struct usb_request *request);
static int misc_register(struct miscdevice *device);
static void misc_deregister(struct miscdevice *device);
static bool s3_input_bridge_latch(void);
static int buffer_allocation_count;
static int buffer_release_count;
static int fail_buffer_allocate = -1;
static void *kmalloc(size_t size, int flags)
{
    (void)flags;
    int index = buffer_allocation_count++;
    if (index == fail_buffer_allocate) return NULL;
    return malloc(size);
}
static void kfree(void *buffer)
{
    if (buffer) ++buffer_release_count;
    free(buffer);
}
struct task_struct { int references; bool alive; };
static DEFINE_MUTEX(worker_lock);
static bool double_evict;
static int active_acm_ports;
static struct task_struct *bridge_thread;
static void init_waitqueue_head(wait_queue_head_t *queue) { *queue = 1; }
static void wake_up_interruptible(wait_queue_head_t *queue) { assert(*queue == 1); }
static int copy_to_user(char *destination, const void *source, size_t size)
{
    memcpy(destination, source, size);
    return 0;
}

static char status_text[512];
static void s3_set_status(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(status_text, sizeof(status_text), format, arguments);
    va_end(arguments);
    assert(length >= 0 && (size_t)length < sizeof(status_text));
}
#include "../../target/psvr2/modules/stage3_serial/stage3_input_diagnostics.inc"
#include "../../target/psvr2/modules/stage3_serial/stage3_fast_input.inc"

/* The neighboring stream cleanup shares the teardown include but is idle. */
#define FAST_NBUFS 1
static bool fast_ready;
static bool fast_registered;
static DEFINE_MUTEX(fast_write_mutex);
static struct miscdevice fast_misc;
static struct usb_ep *fast_ep;
static struct usb_request *fast_reqs[FAST_NBUFS];
#include "../../target/psvr2/modules/stage3_serial/stage3_stream_teardown.inc"

static int queue_count;
static int dequeue_count;
static int allocation_count;
static int release_count;
static int fail_queue = -1;
static int fail_allocate = -1;
static bool fail_misc;
static bool emit_early_completion;
static bool registered;

static struct usb_request *usb_ep_alloc_request(struct usb_ep *ep, int flags)
{
    assert(ep); (void)flags;
    int index = allocation_count++;
    assert(!smp_load_acquire(&fast_in_ready));
    assert(!s3_input_bridge_latch());
    if (index == fail_allocate) return NULL;
    return calloc(1, sizeof(struct usb_request));
}

static void usb_ep_free_request(struct usb_ep *ep, struct usb_request *request)
{
    assert(ep && request && !request->buf);
    ++release_count;
    free(request);
}

static int usb_ep_queue(struct usb_ep *ep, struct usb_request *request, int flags)
{
    assert(ep && request && request->buf); (void)flags;
    if (!registered) {
        assert(!smp_load_acquire(&fast_in_ready));
        assert(!s3_input_bridge_latch());
    }
    int index = queue_count++;
    if (index == fail_queue) return -EIO;
    if (emit_early_completion && index == 0) {
        memset(request->buf, 0xA5, 16);
        request->actual = 16;
        request->status = 0;
        request->complete(ep, request);
    }
    return 0;
}

static int usb_ep_dequeue(struct usb_ep *ep, struct usb_request *request)
{
    assert(ep && request && request->buf);
    ++dequeue_count;
    request->status = -ECONNRESET;
    request->complete(ep, request);
    return 0;
}

static int misc_register(struct miscdevice *device)
{
    assert(device == &fast_in_misc);
    assert(!smp_load_acquire(&fast_in_ready));
    assert(!s3_input_bridge_latch());
    assert(buffer_allocation_count == FAST_INPUT_NBUFS);
    assert(allocation_count == (fast_in_ep ? FAST_INPUT_NBUFS : 0));
    assert(queue_count == (fast_in_ep ? FAST_INPUT_NBUFS : 0));
    if (fail_misc) return -EIO;
    registered = true;
    return 0;
}

static void misc_deregister(struct miscdevice *device)
{
    assert(device == &fast_in_misc);
    assert(!smp_load_acquire(&fast_in_ready));
    registered = false;
}

static void packet(unsigned char bytes[16], unsigned char value)
{
    memset(bytes, value, 16);
    bytes[0] = 'C';
    bytes[1] = 'T';
}

static void test_input_diagnostics(void)
{
    s3_input_diagnostics_reset();
    s3_input_diagnostics_open(false, -ENOENT, -EINPROGRESS);
    struct s3_input_diagnostics state = s3_input_diagnostics_snapshot();
    assert(!state.opened && state.open_error == -ENOENT && state.raw_ioctl == -EINPROGRESS);
    s3_input_diagnostics_open(true, 0, 0);
    s3_input_diagnostics_read_begin();
    state = s3_input_diagnostics_snapshot();
    assert(state.opened && state.reading && state.read_calls == 1 && !state.read_returns);
    s3_input_diagnostics_read_end(16);
    s3_input_diagnostics_read_begin();
    s3_input_diagnostics_read_end(-EAGAIN);
    s3_input_diagnostics_read_begin();
    s3_input_diagnostics_read_end(0);
    s3_input_diagnostics_read_begin();
    s3_input_diagnostics_read_end(-EIO);
    s3_input_diagnostics_ring(true);
    s3_input_diagnostics_ring(false);
    state = s3_input_diagnostics_snapshot();
    assert(!state.reading && state.read_calls == 4 && state.read_returns == 4);
    assert(state.read_bytes == 16 && state.read_again == 1 && state.read_eof == 1);
    assert(state.fatal == -EIO && state.last_read == -EIO);
    assert(state.ring_accepted == 1 && state.ring_dropped == 1);
    s3_input_bridge_status();
    assert(strstr(status_text, "opened=1 open_err=0 raw=0 reading=0 read=4/4 bytes=16 again=1 eof=1"));
    assert(strstr(status_text, "accepted=1 dropped=1 ready=0 pending=0"));
    s3_input_diagnostics_closed();
    state = s3_input_diagnostics_snapshot();
    assert(!state.opened && state.fatal == -EIO);
    s3_input_diagnostics_reset();
}

static void test_hung_up_tty_recovery(void)
{
    struct file tty = {.f_flags = O_NONBLOCK};
    int residue = 7;
    s3_input_diagnostics_reset();
    assert(!s3_input_read_should_reopen(&tty, -EAGAIN, &residue));
    assert(!s3_input_read_should_reopen(&tty, 0, &residue));
    assert(residue == 7 && !s3_input_diagnostics_snapshot().tty_hangups);
    tty.hung_up = true;
    assert(!s3_input_read_should_reopen(&tty, 16, &residue));
    assert(residue == 7);
    assert(s3_input_read_should_reopen(&tty, 0, &residue));
    assert(!residue && s3_input_diagnostics_snapshot().tty_hangups == 1);
    residue = 9;
    assert(s3_input_read_should_reopen(&tty, -EIO, &residue));
    assert(!residue && s3_input_diagnostics_snapshot().tty_hangups == 2);
    /* A freshly opened descriptor resumes partial-frame assembly; old
     * connection residue cannot be prefixed to the new stream. */
    tty.hung_up = false;
    residue = 4;
    assert(!s3_input_read_should_reopen(&tty, 0, &residue) && residue == 4);
    s3_input_diagnostics_reset();
}

static void test_serial_diagnostics(void)
{
    struct f_serial_opts options = {{23}, 2};
    struct usb_ep input = {.enabled = true, .maxpacket = 1024};
    struct usb_ep output = {.enabled = true, .maxpacket = 512};
    struct gserial serial = {{&options.func_inst}, &options, &input, &output};
    active_acm_ports = 2;
    acm_inst[1] = &options.func_inst;
    acm_func[1] = &serial.func;
    struct s3_input_serial_snapshot state = s3_input_serial_snapshot();
    assert(state.line == 2 && state.connected && state.in_enabled && state.out_enabled);
    assert(state.out_maxpacket == 512);
    s3_input_bridge_status();
    assert(strstr(status_text, "line=2 connected=1 in=1 out=1 maxpacket=512"));
    serial.ioport = NULL;
    output.enabled = false;
    state = s3_input_serial_snapshot();
    assert(state.line == 2 && !state.connected && !state.out_enabled);
    options.port_num = MAX_U_SERIAL_PORTS;
    assert(s3_input_serial_snapshot().line == -1);
    options.port_num = 2;
    serial.func.fi = NULL;
    state = s3_input_serial_snapshot();
    assert(state.line == 2 && !state.connected && !state.in_enabled && !state.out_enabled);
    acm_inst[1] = NULL;
    acm_func[1] = NULL;
    active_acm_ports = 0;
}

static void test_delayed_setup(void)
{
    struct usb_ep endpoint = {.name = "mock-input"};
    struct file reader = {.f_flags = O_NONBLOCK};
    char output[1024];
    loff_t position = 0;
    assert(!s3_input_bridge_latch());
    assert(fast_in_open(NULL, &reader) == -ENODEV);
    assert(fast_in_read(&reader, output, sizeof(output), &position) == -ENODEV);
    fail_queue = 3;
    assert(s3_fast_input_setup(&endpoint) == -EIO);
    assert(!smp_load_acquire(&fast_in_ready));
    assert(!s3_input_bridge_latch());
    assert(release_count == allocation_count);
    for (int slot = 0; slot < FAST_INPUT_NBUFS; ++slot) assert(!fast_in_reqs[slot]);
    assert(buffer_allocation_count == buffer_release_count);
    buffer_allocation_count = buffer_release_count = 0;
    allocation_count = release_count = queue_count = dequeue_count = 0;
    fail_queue = -1;
    fail_allocate = 2;
    assert(s3_fast_input_setup(&endpoint) == -ENOMEM);
    assert(!smp_load_acquire(&fast_in_ready));
    assert(!s3_input_bridge_latch());
    assert(release_count == allocation_count - 1);
    for (int slot = 0; slot < FAST_INPUT_NBUFS; ++slot) assert(!fast_in_reqs[slot]);
    assert(buffer_allocation_count == buffer_release_count);
    buffer_allocation_count = buffer_release_count = 0;
    allocation_count = release_count = queue_count = dequeue_count = 0;
    fail_allocate = -1;
    fail_misc = true;
    assert(s3_fast_input_setup(&endpoint) == -EIO);
    assert(!smp_load_acquire(&fast_in_ready) && !registered);
    assert(!s3_input_bridge_latch());
    assert(release_count == allocation_count);
    for (int slot = 0; slot < FAST_INPUT_NBUFS; ++slot) assert(!fast_in_reqs[slot]);
    assert(buffer_allocation_count == buffer_release_count);
    buffer_allocation_count = buffer_release_count = 0;
    allocation_count = release_count = queue_count = dequeue_count = 0;
    fail_misc = false;
    emit_early_completion = true;
    assert(s3_fast_input_setup(&endpoint) == 0);
    assert(smp_load_acquire(&fast_in_ready) && registered);
    assert(fast_in_done[0]); /* Completion before publication was retained. */
    assert(fast_in_open(NULL, &reader) == 0);
    assert(fast_in_lengths[0] == 16);
    assert(fast_in_read(&reader, output, sizeof(output), &position) == 16);
    for (int byte = 0; byte < 16; ++byte) assert((unsigned char)output[byte] == 0xA5);
    assert(queue_count == FAST_INPUT_NBUFS + 1);
    assert(s3_input_bridge_latch());
    assert(dequeue_count == FAST_INPUT_NBUFS);
    assert(s3_input_bridge_latch());
    assert(dequeue_count == FAST_INPUT_NBUFS); /* Persistent latch is idempotent. */
    assert(!fast_input_has_data());
    emit_early_completion = false;
}

static void test_ring_and_stream(void)
{
    struct file reader = {.f_flags = O_NONBLOCK};
    char output[1024];
    loff_t position = 0;
    unsigned char bytes[16];
    for (int slot = 0; slot < FAST_INPUT_NBUFS - 1; ++slot) {
        packet(bytes, (unsigned char)slot);
        assert(s3_input_bridge_push(bytes));
    }
    assert(!s3_input_bridge_push(bytes)); /* Preserve unread frames when full. */
    assert(fast_in_read(&reader, output, 8, &position) == -EMSGSIZE);
    int queues_before_read = queue_count;
    for (int slot = 0; slot < FAST_INPUT_NBUFS - 1; ++slot) {
        assert(fast_in_read(&reader, output, sizeof(output), &position) == 16);
        assert(output[0] == 'C' && output[1] == 'T' && output[2] == slot);
    }
    assert(queue_count == queues_before_read); /* Bridge mode never requeues USB. */
    assert(fast_in_read(&reader, output, sizeof(output), &position) == -EAGAIN);

    unsigned char stream[64] = {0};
    stream[0] = 'C';
    int length = 1;
    assert(s3_input_bridge_drain(stream, &length) == 0 && length == 1);
    packet(bytes, 7);
    memcpy(stream + length, bytes + 1, 7);
    length += 7;
    assert(s3_input_bridge_drain(stream, &length) == 0 && length == 8);
    memcpy(stream + length, bytes + 8, 8);
    length += 8;
    assert(s3_input_bridge_drain(stream, &length) == 1 && length == 0);
    assert(fast_in_read(&reader, output, sizeof(output), &position) == 16);
    assert(output[2] == 7);

    /* Junk, split header, and two frames retain exactly the trailing byte. */
    memset(stream, 0xFF, 3);
    packet(stream + 3, 9);
    packet(stream + 19, 10);
    stream[35] = 'C';
    length = 36;
    assert(s3_input_bridge_drain(stream, &length) == 2);
    assert(length == 1 && stream[0] == 'C');
    assert(fast_in_read(&reader, output, sizeof(output), &position) == 16);
    assert(output[2] == 9);
    assert(fast_in_read(&reader, output, sizeof(output), &position) == 16);
    assert(output[2] == 10);

    /* Exactly one byte has no readable stream[1]; ASan checks the boundary. */
    unsigned char *single = malloc(1);
    assert(single);
    *single = 'C';
    length = 1;
    assert(s3_input_bridge_drain(single, &length) == 0 && length == 1);
    free(single);
}

static unsigned int stopped_workers;
static void get_task_struct(struct task_struct *task)
{
    assert(task->alive && task->references == 1);
    ++task->references;
}
static int kthread_stop(struct task_struct *task)
{
    /* Model natural worker exit after the snapshot lock is released. The
     * caller's pin must keep the task alive before stop acquires a reference. */
    assert(task->references == 2);
    --task->references;
    assert(task->alive && task->references == 1);
    ++stopped_workers;
    return 0;
}
static void put_task_struct(struct task_struct *task)
{
    assert(task->references == 1);
    --task->references;
    task->alive = false;
}
#include "../../target/psvr2/modules/stage3_serial/stage3_workers.inc"

static void test_worker_snapshot(void)
{
    for (unsigned int index = 0; index < 3; ++index) {
        struct task_struct worker = {1, true};
        struct task_struct *slot = &worker;
        s3_stop_worker(&slot);
        assert(!slot && !worker.alive && worker.references == 0);
    }
    struct task_struct *empty = NULL;
    s3_stop_worker(&empty);
    assert(stopped_workers == 3);
}

static void assert_cleaned_up(void)
{
    s3_fast_input_cleanup();
    assert(!smp_load_acquire(&fast_in_ready) && !registered);
    assert(allocation_count == release_count);
    assert(buffer_allocation_count == buffer_release_count);
    for (int slot = 0; slot < FAST_INPUT_NBUFS; ++slot) {
        assert(!fast_in_buffers[slot] && !fast_in_reqs[slot]);
        assert(!fast_in_lengths[slot]);
    }
    assert(!s3_input_bridge_latch());
    struct file reader = {.f_flags = O_NONBLOCK};
    char output[1024];
    loff_t position = 0;
    assert(fast_in_read(&reader, output, sizeof(output), &position) == -ENODEV);
    struct usb_ep endpoint = {.name = "after-stop"};
    assert(s3_fast_input_setup(&endpoint) == -ENODEV);
    assert(s3_fast_input_setup(NULL) == -ENODEV);
    int releases = buffer_release_count;
    s3_fast_input_cleanup(); /* Repeated teardown cannot free storage twice. */
    assert(buffer_release_count == releases);
}

static void assert_failed_software_setup(void)
{
    assert(!smp_load_acquire(&fast_in_ready) && !registered && !fast_in_ep);
    assert(!fast_in_bridge_mode && !s3_input_bridge_latch());
    assert(!allocation_count && !release_count && !queue_count && !dequeue_count);
    for (int slot = 0; slot < FAST_INPUT_NBUFS; ++slot)
        assert(!fast_in_buffers[slot] && !fast_in_reqs[slot] && !fast_in_lengths[slot]);
}

static void test_software_bridge(void)
{
    /* Model a new module instance after verifying the prior teardown. */
    fast_in_stopping = false;
    buffer_allocation_count = buffer_release_count = 0;
    allocation_count = release_count = queue_count = dequeue_count = 0;
    struct task_struct worker = {1, true};
    assert(s3_fast_input_bridge_setup() == -ENODEV);
    double_evict = true;
    assert(s3_fast_input_bridge_setup() == -ENODEV);
    active_acm_ports = 2;
    assert(s3_fast_input_bridge_setup() == -ENODEV);
    bridge_thread = &worker;

    fail_buffer_allocate = 3;
    assert(s3_fast_input_bridge_setup() == -ENOMEM);
    assert_failed_software_setup();
    assert(buffer_allocation_count == 4 && buffer_release_count == 3);
    buffer_allocation_count = buffer_release_count = 0;
    fail_buffer_allocate = -1;
    fail_misc = true;
    assert(s3_fast_input_bridge_setup() == -EIO);
    assert_failed_software_setup();
    assert(buffer_allocation_count == FAST_INPUT_NBUFS);
    assert(buffer_allocation_count == buffer_release_count);
    buffer_allocation_count = buffer_release_count = 0;
    fail_misc = false;

    assert(s3_fast_input_bridge_setup() == 0);
    assert(smp_load_acquire(&fast_in_ready) && registered);
    assert(!fast_in_ep && fast_in_bridge_mode);
    assert(s3_input_bridge_latch());
    assert(s3_fast_input_bridge_setup() == 0); /* Repeated command keeps the ring. */
    assert(buffer_allocation_count == FAST_INPUT_NBUFS);
    assert(!allocation_count && !queue_count && !dequeue_count);
    struct usb_ep endpoint = {.name = "cannot-replace-software-ring"};
    assert(s3_fast_input_setup(&endpoint) == -EBUSY);
    test_ring_and_stream();
    assert_cleaned_up();
    assert(!allocation_count && !queue_count && !dequeue_count);
    bridge_thread = NULL;
}

int main(void)
{
    test_input_diagnostics();
    test_hung_up_tty_recovery();
    test_serial_diagnostics();
    test_delayed_setup();
    test_ring_and_stream();
    test_worker_snapshot();
    assert_cleaned_up();
    test_software_bridge();
    s3_fast_stream_cleanup();
    puts("Stage3 delayed input setup, software bridge, readiness, ring, split packets, and teardown passed.");
    return 0;
}
