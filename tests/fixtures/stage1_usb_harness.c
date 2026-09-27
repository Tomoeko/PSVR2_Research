/* Execute the real Stage1 USB helpers against bounded fake native requests. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define STATUS_SIZE 256
#define GO_BUSYCOUNT 96
#define GO_PACKETLEN 128
#define GO_EP 160
#define GO_ITEMS 136
#define GO_DMABUFS 144
#define DMA_REQ 16
typedef struct { int value; } atomic_t;
#define GO_PKTLEN(p) (*(size_t *)((char *)(p) + GO_PACKETLEN))
#define GO_EPPTR(p) (*(struct usb_ep **)((char *)(p) + GO_EP))
#define GO_BUSYCNT(p) (*(atomic_t *)((char *)(p) + GO_BUSYCOUNT))
#define atomic_read(p) ((p)->value)
#define READ_ONCE(x) (x)
static void observe_write(void *slot);
#define WRITE_ONCE(x, value) do { (x) = (value); observe_write(&(x)); } while (0)
static int write_barriers, grace_periods, sleeps, completions, dequeue_calls;
static int dequeue_error_index = -1;
static bool refill, installing, awaiting_old_call;
#define smp_wmb() (++write_barriers)
static char status_buf[STATUS_SIZE];
struct usb_endpoint_descriptor { bool input; };
struct usb_ep { struct usb_endpoint_descriptor *desc; };
struct usb_request {
    void (*complete)(struct usb_ep *, struct usb_request *);
    bool queued, account;
};
struct s1_gbuf { void *in, *out; };
struct inode { void *i_cdev; };
struct dentry { struct inode *d_inode; };
struct path { struct dentry *dentry; };
struct file { int unused; };
#define LOOKUP_FOLLOW 0
#define IS_ERR(p) (!(p))
static int kern_path(const char *p, int flags, struct path *out)
{ (void)p; (void)flags; (void)out; return -ENOENT; }
static bool is_autoboot_disabled(void) { return false; }
static struct file *filp_open(const char *p, int flags, int mode)
{ (void)p; (void)flags; (void)mode; return NULL; }
static void filp_close(struct file *file, void *unused)
{ (void)file; (void)unused; }
static void path_put(struct path *path) { (void)path; }
static bool usb_endpoint_dir_in(const struct usb_endpoint_descriptor *desc)
{ return desc->input; }
static union { max_align_t alignment; char data[256]; } fake_gbuf;
static union { max_align_t alignment; char data[4 * 80]; } fake_dmabufs;
static struct usb_request requests[4];
static struct usb_endpoint_descriptor descriptor;
static struct usb_ep endpoint = {&descriptor};
static void (*original)(struct usb_ep *, struct usb_request *);

static void native_complete(struct usb_ep *ep, struct usb_request *request)
{
    assert(ep == &endpoint);
    ++completions;
    if (request->account) {
        --GO_BUSYCNT(fake_gbuf.data).value;
        request->account = false;
    }
}
static void replacement(struct usb_ep *ep, struct usb_request *request)
{
    assert(original == native_complete);
    original(ep, request);
}
static void observe_write(void *slot)
{
    for (size_t index = 0; index < 4; ++index) {
        if (slot == &requests[index].complete && installing) {
            assert(write_barriers > 0);
            /* Simulate an IRQ at the first possible installation instant. */
            requests[index].complete(&endpoint, &requests[index]);
        }
    }
}
static void synchronize_sched(void)
{
    ++grace_periods;
    if (awaiting_old_call) {
        for (size_t index = 0; index < 4; ++index)
            assert(requests[index].complete == native_complete);
        /* An old IRQ can still be forwarding before this grace completes. */
        replacement(&endpoint, &requests[0]);
        awaiting_old_call = false;
    }
}
static void msleep(unsigned int milliseconds)
{
    assert(milliseconds == 200);
    ++sleeps;
    if (refill)
        ++GO_BUSYCNT(fake_gbuf.data).value;
}
static void usb_ep_fifo_flush(struct usb_ep *ep) { assert(ep == &endpoint); }
static int usb_ep_dequeue(struct usb_ep *ep, struct usb_request *request)
{
    assert(ep == &endpoint && request >= requests && request < requests + 4);
    ++dequeue_calls;
    if (request - requests == dequeue_error_index)
        return -EIO;
    if (!request->queued)
        return -EINVAL;
    request->queued = false;
    request->account = true;
    request->complete(ep, request);
    return 0;
}

#include "../../target/psvr2/modules/stage1/stage1_usb.inc"

static void reset(void)
{
    memset(fake_gbuf.data, 0, sizeof(fake_gbuf.data));
    memset(fake_dmabufs.data, 0, sizeof(fake_dmabufs.data));
    memset(status_buf, 0, sizeof(status_buf));
    descriptor.input = true;
    GO_PKTLEN(fake_gbuf.data) = 16384;
    GO_EPPTR(fake_gbuf.data) = &endpoint;
    *(int *)(fake_gbuf.data + GO_ITEMS) = 4;
    *(char **)(fake_gbuf.data + GO_DMABUFS) = fake_dmabufs.data;
    for (size_t index = 0; index < 4; ++index) {
        requests[index] = (struct usb_request){native_complete, false, false};
        *(struct usb_request **)(fake_dmabufs.data + 80 * index + DMA_REQ) =
            &requests[index];
    }
    write_barriers = grace_periods = sleeps = completions = dequeue_calls = 0;
    dequeue_error_index = -1;
    refill = installing = awaiting_old_call = false;
    original = NULL;
}

static void publication(void)
{
    reset();
    installing = true;
    assert(s1_usb_replace_callbacks(fake_gbuf.data, "recv", replacement,
                                     &original) == 4);
    installing = false;
    assert(completions == 4 && original == native_complete);
    awaiting_old_call = true;
    assert(s1_usb_restore_callbacks(fake_gbuf.data, replacement, original) == 4);
    assert(grace_periods == 1 && !awaiting_old_call && completions == 5);
    original = NULL;
}

static void preparation(void)
{
    reset();
    requests[0].queued = requests[2].queued = true;
    GO_BUSYCNT(fake_gbuf.data).value = 2;
    assert(s1_usb_prepare_in(fake_gbuf.data, "send_prepare") == 0);
    assert(!strcmp(status_buf,
                   "OK send_prepare requests=4 dequeued=2 busy=0"));
    assert(completions == 2 && dequeue_calls == 4 && grace_periods == 1);
    assert(sleeps == 1 && write_barriers == 0 && original == NULL);
    for (size_t index = 0; index < 4; ++index)
        assert(requests[index].complete == native_complete);
    assert(s1_usb_prepare_in(fake_gbuf.data, "send_prepare") == 0);
    assert(strstr(status_buf, "dequeued=0 busy=0"));
}

static void validation(void)
{
    reset();
    requests[3].complete = NULL;
    assert(s1_usb_prepare_in(fake_gbuf.data, "send_prepare") == -EINVAL);
    assert(!dequeue_calls && strstr(status_buf, "ERR send_prepare invalid"));
    reset();
    requests[3].complete = replacement;
    assert(s1_usb_prepare_in(fake_gbuf.data, "send_prepare") == -EINVAL);
    assert(!dequeue_calls && strstr(status_buf, "ERR send_prepare mixed"));
    reset();
    descriptor.input = false;
    assert(s1_usb_prepare_in(fake_gbuf.data, "send_prepare") == -EINVAL);
    assert(!dequeue_calls && strstr(status_buf, "unexpected endpoint direction"));
    reset();
    *(int *)(fake_gbuf.data + GO_ITEMS) = 129;
    assert(s1_usb_prepare_in(fake_gbuf.data, "send_prepare") == -EINVAL);
    assert(!dequeue_calls);
}

static void failures(void)
{
    reset();
    dequeue_error_index = 1;
    assert(s1_usb_prepare_in(fake_gbuf.data, "send_prepare") == -EIO);
    assert(dequeue_calls == 2 && !sleeps && !grace_periods);
    assert(!strcmp(status_buf, "ERR send_prepare dequeue[1] -5"));
    reset();
    refill = true;
    assert(s1_usb_prepare_in(fake_gbuf.data, "send_prepare") == -EBUSY);
    assert(strstr(status_buf, "ERR send_prepare busy=1"));
    reset();
    GO_BUSYCNT(fake_gbuf.data).value = -1;
    assert(s1_usb_prepare_in(fake_gbuf.data, "send_prepare") == -EBUSY);
    assert(strstr(status_buf, "ERR send_prepare busy=-1"));
}

int main(void)
{
    publication();
    preparation();
    validation();
    failures();
    puts("Stage1 USB lifecycle/preparation checks passed");
    return 0;
}
