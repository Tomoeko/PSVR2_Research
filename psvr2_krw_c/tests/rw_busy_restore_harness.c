/* Exercise the actual byte-write sequence with a scripted USB endpoint. */
#include "psvr2/rw.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

enum { MAX_TRANSFERS = 8 };

static uint8_t transfers[MAX_TRANSFERS][PSVR2_WRITE_PAYLOAD_SIZE];
static int responses[MAX_TRANSFERS];
static size_t transfer_count;

void psvr2_constants_invalidate_live_state(psvr2_constants *constants) {
    constants->live_profile_certified = false;
}

int psvr2_device_control(
    psvr2_device *device, uint8_t request_type,
    uint8_t request, uint16_t value, uint16_t index,
    void *data, uint16_t length, unsigned timeout_ms) {
    (void)device;
    assert(request_type == 0x21 && request == 0x09);
    assert(value == 0x01f0 && index == 0);
    assert(length == PSVR2_WRITE_PAYLOAD_SIZE && timeout_ms == 2500);
    assert(transfer_count < MAX_TRANSFERS);
    memcpy(transfers[transfer_count], data, length);
    return responses[transfer_count++];
}

static void prepare(
    psvr2_krw *krw, psvr2_exploit *exploit,
    psvr2_device *device, psvr2_constants *constants) {
    memset(device, 0, sizeof(*device));
    memset(constants, 0, sizeof(*constants));
    memset(exploit, 0, sizeof(*exploit));
    memset(krw, 0, sizeof(*krw));
    memset(responses, 0, sizeof(responses));
    transfer_count = 0;
    device->handle = (libusb_device_handle *)(uintptr_t)1;
    device->connection_generation = 1;
    constants->live_profile_certified = true;
    constants->certified_connection_generation = 1;
    constants->fw.stack_cookie = UINT64_C(0x1122334455667788);
    constants->fw.clean_return = UINT64_C(0xffffffc000305778);
    exploit->usb = device;
    exploit->constants = constants;
    exploit->direct_read_ready = true;
    krw->ex = exploit;
    krw->connection_generation = 1;
    krw->spinlock = UINT64_C(0xffffffc000303000);
    krw->regs.mep = UINT64_C(0xffffffc0391f5c00);
    krw->regs.valid = true;
}

static void assert_write(size_t index, uint64_t target, uint8_t value) {
    assert(psvr2_load_le64(transfers[index] + 96) ==
           target - PSVR2_MTU3_ENDPOINT_STATE_OFFSET);
    assert(psvr2_load_le64(transfers[index] + 112) == value);
}

int main(void) {
    psvr2_krw krw;
    psvr2_exploit exploit;
    psvr2_device device;
    psvr2_constants constants;
    const uint64_t target = UINT64_C(0xffffffc000123456);

    prepare(&krw, &exploit, &device, &constants);
    responses[0] = responses[1] = PSVR2_WRITE_PAYLOAD_SIZE;
    assert(psvr2_krw_write_byte_blind(&krw, target, 0x5a));
    assert(transfer_count == 2);
    assert_write(0, target, 0x5a);
    assert_write(1, krw.regs.mep + PSVR2_MTU3_ENDPOINT_STATE_OFFSET, 0);

    /* A USB error may follow execution, so clear busy before retrying. */
    prepare(&krw, &exploit, &device, &constants);
    responses[0] = -1;
    responses[1] = responses[2] = responses[3] =
        PSVR2_WRITE_PAYLOAD_SIZE;
    assert(psvr2_krw_write_byte_blind(&krw, target, 0xa5));
    assert(transfer_count == 4);
    assert_write(0, target, 0xa5);
    assert_write(1, krw.regs.mep + PSVR2_MTU3_ENDPOINT_STATE_OFFSET, 0);
    assert_write(2, target, 0xa5);
    assert_write(3, krw.regs.mep + PSVR2_MTU3_ENDPOINT_STATE_OFFSET, 0);

    prepare(&krw, &exploit, &device, &constants);
    responses[0] = PSVR2_WRITE_PAYLOAD_SIZE;
    assert(!psvr2_krw_write_byte_blind(&krw, target, 0x5a));
    assert(transfer_count == 4);
    assert(!krw.regs.valid);
    assert(krw.last_result.status == PSVR2_STATUS_RECOVERY_REQUIRED);

    prepare(&krw, &exploit, &device, &constants);
    responses[0] = PSVR2_WRITE_PAYLOAD_SIZE;
    assert(psvr2_krw_write_byte_blind(
        &krw, krw.regs.mep + PSVR2_MTU3_ENDPOINT_STATE_OFFSET, 0));
    assert(transfer_count == 1);

    prepare(&krw, &exploit, &device, &constants);
    krw.regs.mep = 0;
    assert(!psvr2_krw_write_byte_blind(&krw, target, 0x5a));
    assert(transfer_count == 0);
    return 0;
}
