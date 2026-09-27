/* Check bulk preparation ordering/error handling without USB or a headset. */
#include "shell_internal.h"

#include <assert.h>
#include <string.h>

enum drain_case { IDLE, FINITE_TAIL, BUSY, ZLP_BUSY, PARTIAL_TIMEOUT_BUSY,
                  USB_ERROR, USB_PARTIAL_ERROR };
static enum drain_case drain_case;
static bool execute_ok, status_ok, prepared, idle_seen, sending, finalized;
static int64_t prepare_result;
static const char *prepare_status;
static size_t exec_calls, status_calls, drain_reads, file_reads, send_calls, opens;

static void reset_case(void) {
    drain_case = IDLE;
    execute_ok = status_ok = true;
    prepared = idle_seen = sending = finalized = false;
    prepare_result = 0;
    prepare_status = "OK send_prepare requests=2 dequeued=2 busy=0";
    exec_calls = status_calls = drain_reads = file_reads = send_calls = opens = 0;
}

bool psvr2_stage1_discover(psvr2_runtime *runtime) {
    assert(runtime);
    return true;
}

bool psvr2_stage1_exec(psvr2_runtime *runtime, const char *command,
                       double timeout, int64_t *result, psvr2_buffer *output) {
    assert(runtime && timeout == 10.0 && !output);
    ++exec_calls;
    if (!strcmp(command, "echo 'send_prepare' > /proc/stage1")) {
        assert(!sending && !drain_reads && !status_calls);
        *result = prepare_result;
        return execute_ok;
    }
    if (!strcmp(command, "echo 'send /tmp/fixture 4' > /proc/stage1")) {
        assert(prepared && idle_seen);
        sending = true;
        ++send_calls;
    } else {
        assert(!strcmp(command, "echo 'send_stop' > /proc/stage1"));
        assert(sending);
    }
    *result = 0;
    return true;
}

bool psvr2_stage1_read_status(psvr2_runtime *runtime, char *output,
                              size_t capacity) {
    assert(runtime && exec_calls);
    ++status_calls;
    if (sending) {
        snprintf(output, capacity, "OK send started 4");
        return true;
    }
    if (!status_ok) return false;
    snprintf(output, capacity, "%s", prepare_status);
    prepared = !strncmp(prepare_status, "OK send_prepare ", 16U);
    return true;
}

bool psvr2_device_claim_interface(psvr2_device *device, int interface_number) {
    (void)device; (void)interface_number;
    assert(0 && "cached interface should already be claimed");
    return false;
}

size_t psvr2_device_find_endpoints(psvr2_device *device,
                                   const psvr2_endpoint_query *query,
                                   psvr2_endpoint *out, size_t capacity) {
    (void)device; (void)query; (void)out; (void)capacity;
    assert(0 && "cached endpoints should already be selected");
    return 0;
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *device,
    unsigned char endpoint, unsigned char *data, int length,
    int *transferred, unsigned int timeout) {
    assert(device && endpoint == 0x81U && timeout == 20U);
    assert(prepared && status_calls);
    if (sending) {
        assert(idle_seen && length >= 4);
        memcpy(data, "FILE", 4U);
        *transferred = 4;
        ++file_reads;
        return 0;
    }
    assert(length == 65536);
    ++drain_reads;
    *transferred = 0;
    switch (drain_case) {
        case FINITE_TAIL:
            if (drain_reads == 1U) { *transferred = 3; return 0; }
            if (drain_reads == 2U) {
                *transferred = 2;
                return LIBUSB_ERROR_TIMEOUT;
            }
            if (drain_reads == 3U) return 0; /* ZLP is not the idle boundary. */
            break;
        case BUSY: *transferred = 16; return 0;
        case ZLP_BUSY: return 0;
        case PARTIAL_TIMEOUT_BUSY:
            *transferred = 1;
            return LIBUSB_ERROR_TIMEOUT;
        case USB_PARTIAL_ERROR: *transferred = 3; return LIBUSB_ERROR_PIPE;
        case USB_ERROR: return LIBUSB_ERROR_NO_DEVICE;
        case IDLE: break;
    }
    idle_seen = true;
    return LIBUSB_ERROR_TIMEOUT;
}

const char * LIBUSB_CALL libusb_error_name(int code) {
    (void)code;
    return "mock USB error";
}

FILE *psvr2_shell_open_output_exclusive(const char *path) {
    assert(!strcmp(path, "mock.out.partial"));
    assert(prepared && idle_seen && sending);
    ++opens;
    return tmpfile();
}

bool psvr2_shell_finalize_output_exclusive(const char *partial,
                                           const char *destination) {
    assert(!strcmp(partial, "mock.out.partial"));
    assert(!strcmp(destination, "mock.out"));
    assert(opens == 1U && file_reads == 1U);
    finalized = true;
    return true;
}

static void assert_no_reads(psvr2_shell *shell) {
    assert(!psvr2_shell_stop_and_drain_bulk_in(shell));
    assert(drain_reads == 0U && file_reads == 0U && send_calls == 0U);
}

int main(void) {
    psvr2_device device = {
        .handle = (libusb_device_handle *)(uintptr_t)1U,
        .connection_generation = 1U
    };
    psvr2_exploit exploit = {.usb = &device};
    psvr2_krw krw = {.ex = &exploit};
    psvr2_runtime runtime = {.krw = &krw};
    psvr2_shell shell = {.runtime = &runtime, .bulk_generation = 1U,
        .bulk_interface = 5, .bulk_in = 0x81U, .bulk_out = 0x02U,
        .bulk_packet = 512U, .bulk_claimed = true};

    reset_case(); execute_ok = false;
    assert_no_reads(&shell);
    assert(status_calls == 0U);
    reset_case(); prepare_result = 1;
    assert_no_reads(&shell);
    assert(status_calls == 0U);
    reset_case(); status_ok = false;
    assert_no_reads(&shell);
    const char *bad_status[] = {
        "ERR send_prepare settle failed", "OK send stopped",
        "OK send_prepare", "WAIT send_prepare requests=2", "OK recv done 4"
    };
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(bad_status); ++index) {
        reset_case(); prepare_status = bad_status[index];
        assert_no_reads(&shell);
    }

    reset_case();
    assert(psvr2_shell_stop_and_drain_bulk_in(&shell));
    assert(prepared && idle_seen && drain_reads == 1U && !send_calls);
    reset_case(); drain_case = FINITE_TAIL;
    assert(psvr2_shell_stop_and_drain_bulk_in(&shell));
    assert(idle_seen && drain_reads == 4U);
    const enum drain_case failures[] = {BUSY, ZLP_BUSY, PARTIAL_TIMEOUT_BUSY,
                                       USB_ERROR, USB_PARTIAL_ERROR};
    for (size_t index = 0; index < PSVR2_ARRAY_LEN(failures); ++index) {
        reset_case(); drain_case = failures[index];
        assert(!psvr2_shell_stop_and_drain_bulk_in(&shell));
        assert(!idle_seen && !send_calls);
        assert(drain_reads == (index < 3U ? 32U : 1U));
    }

    char *download[] = {"fast_download", "/tmp/fixture", "mock.out", "4"};
    reset_case(); drain_case = FINITE_TAIL;
    assert(psvr2_shell_fast_download(&shell, 4, download) == 0);
    assert(idle_seen && send_calls == 1U && opens == 1U && finalized);
    reset_case(); prepare_status = "ERR send_prepare endpoint busy";
    assert(psvr2_shell_fast_download(&shell, 4, download) == 1);
    assert(!drain_reads && !file_reads && !send_calls && !opens && !finalized);
    puts("bulk preparation/drain regressions passed");
    return 0;
}
