#include "test_cases.h"

#include "kernel_list_internal.h"
#include "shell_internal.h"
#include "vfs_internal.h"

#include "psvr2/device.h"
#include "psvr2/gaze.h"
#include "psvr2/runtime.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t address[8];
    uint64_t next[8];
    size_t count;
    bool fail;
} fake_list;

static bool fake_read_pointer(
    void *opaque, uint64_t address, uint64_t *value) {
    fake_list *list = opaque;
    if (list->fail) return false;
    for (size_t i = 0; i < list->count; ++i) {
        if (list->address[i] == address) {
            *value = list->next[i];
            return true;
        }
    }
    return false;
}

static bool count_node(void *opaque, uint64_t node) {
    (void)node;
    ++*(size_t *)opaque;
    return true;
}

static void test_kernel_list_results(void) {
    fake_list list = {
        .address = {0x1000, 0x2000, 0x3000},
        .next = {0x2000, 0x3000, 0x9000},
        .count = 3
    };
    size_t count = 0;
    assert(psvr2_kernel_list_walk_with_reader(
               fake_read_pointer, &list, 0x1000, 0x9000,
               false, 8, count_node, &count) ==
           PSVR2_WALK_COMPLETE);
    assert(count == 3);

    list.next[2] = 0x2000;
    count = 0;
    assert(psvr2_kernel_list_walk_with_reader(
               fake_read_pointer, &list, 0x1000, 0x9000,
               false, 8, count_node, &count) ==
           PSVR2_WALK_CYCLE);
    assert(count == 3);

    list.next[2] = 0x9000;
    count = 0;
    assert(psvr2_kernel_list_walk_with_reader(
               fake_read_pointer, &list, 0x1000, 0x9000,
               false, 2, count_node, &count) ==
           PSVR2_WALK_LIMIT);
    list.fail = true;
    count = 0;
    assert(psvr2_kernel_list_walk_with_reader(
               fake_read_pointer, &list, 0x1000, 0x9000,
               false, 8, count_node, &count) ==
           PSVR2_WALK_READ_ERROR);
}

static void test_eye_tracking_commands(void) {
    psvr2_vendor_command commands[4];
    assert(psvr2_eye_tracking_activation_commands(
               commands, PSVR2_ARRAY_LEN(commands)) == 4);
    assert(commands[0].report_id == 0x0b);
    assert(commands[0].subcommand == 1);
    assert(commands[0].length == 8);
    assert(psvr2_load_le32(commands[0].data) == 1);
    assert(psvr2_load_le32(commands[0].data + 4) == 5);
    assert(commands[1].report_id == 0x17);
    assert(commands[1].length == 2);
    assert(commands[1].data[0] == 1 && commands[1].data[1] == 3);
    assert(commands[2].report_id == 0x12 &&
           commands[2].data[0] == 5);
    assert(commands[3].report_id == 0x0c &&
           commands[3].length == 0);
}

static void test_endpoint_filtering(void) {
    const struct libusb_endpoint_descriptor endpoints[] = {
        {
            .bEndpointAddress = 0x81,
            .bmAttributes = LIBUSB_TRANSFER_TYPE_INTERRUPT,
            .wMaxPacketSize = 64
        },
        {
            .bEndpointAddress = 0x82,
            .bmAttributes = LIBUSB_TRANSFER_TYPE_BULK,
            .wMaxPacketSize = 512
        },
        {
            .bEndpointAddress = 0x03,
            .bmAttributes = LIBUSB_TRANSFER_TYPE_BULK,
            .wMaxPacketSize = 512
        }
    };
    const struct libusb_interface_descriptor alternate = {
        .bInterfaceNumber = 5,
        .bAlternateSetting = 0,
        .bNumEndpoints = 3,
        .endpoint = endpoints
    };
    const struct libusb_endpoint_descriptor et_endpoint = {
        .bEndpointAddress = 0x87,
        .bmAttributes = LIBUSB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 1024
    };
    const struct libusb_interface_descriptor et_alternate = {
        .bInterfaceNumber = 6,
        .bAlternateSetting = 0,
        .bNumEndpoints = 1,
        .endpoint = &et_endpoint
    };
    const struct libusb_interface interfaces[] = {
        {
            .altsetting = &alternate,
            .num_altsetting = 1
        },
        {
            .altsetting = &et_alternate,
            .num_altsetting = 1
        }
    };
    const struct libusb_config_descriptor config = {
        .bNumInterfaces = 2,
        .interface = interfaces
    };
    const psvr2_endpoint_query query = {
        .interface_number = 5,
        .alternate_setting = 0,
        .direction = LIBUSB_ENDPOINT_IN,
        .transfer_type = LIBUSB_TRANSFER_TYPE_BULK
    };
    psvr2_endpoint found;
    assert(psvr2_usb_find_endpoints(
               &config, &query, &found, 1) == 1);
    assert(found.address == 0x82);
    assert(found.interface_number == 5);
    assert(found.packet_size == 512);

    const psvr2_endpoint_query et_query = {
        .interface_number = -1,
        .alternate_setting = -1,
        .direction = LIBUSB_ENDPOINT_IN,
        .transfer_type = LIBUSB_TRANSFER_TYPE_BULK,
        .address = 0x87
    };
    assert(psvr2_usb_find_endpoints(
               &config, &et_query, &found, 1) == 1);
    assert(found.address == 0x87);
    assert(found.interface_number == 6);
    assert(found.packet_size == 1024);
}

static void initialize_relocalizer_packet(
    uint8_t *packet, uint32_t sequence, uint32_t feature_count) {
    memset(packet, 0, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE);
    memcpy(packet, "RP", 2);
    psvr2_store_le16(packet + 2, 1);
    psvr2_store_le32(
        packet + 4, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE);
    psvr2_store_le32(packet + 8, 1);
    psvr2_store_le32(packet + 0x28, sequence);
    psvr2_store_le32(packet + 0x30, feature_count);
}

static void test_relocalizer_stale_ranges(void) {
    psvr2_gaze_relocalizer_stale_range ranges[
        PSVR2_GAZE_RELOCALIZER_MAX_STALE_RANGES];
    psvr2_gaze_relocalizer_stale_layout layout;

    assert(psvr2_gaze_calculate_relocalizer_stale_ranges(
        0, false, ranges, PSVR2_ARRAY_LEN(ranges), &layout));
    assert(layout.range_count == 6 && layout.array_range_count == 6);
    assert(layout.array_bytes == 224856 && layout.fixed_tail_bytes == 0);
    assert(layout.total_bytes == 224856 &&
           !layout.fixed_tails_included);
    assert(ranges[0].offset == 0x34 && ranges[0].length == 324U * 4U);
    assert(ranges[5].offset == 0x958c &&
           ranges[5].length == 324U * 576U);

    assert(psvr2_gaze_calculate_relocalizer_stale_ranges(
        0, true, ranges, PSVR2_ARRAY_LEN(ranges), &layout));
    assert(layout.range_count == 9 && layout.array_range_count == 6);
    assert(layout.array_bytes == 224856 && layout.fixed_tail_bytes == 124);
    assert(layout.total_bytes == 224980 && layout.fixed_tails_included);
    assert(ranges[6].offset == 0x36e90 && ranges[6].length == 4);
    assert(ranges[7].offset == 0x9ae94 && ranges[7].length == 44);
    assert(ranges[8].offset == 0x9b020 && ranges[8].length == 76);

    assert(psvr2_gaze_calculate_relocalizer_stale_ranges(
        1, false, ranges, PSVR2_ARRAY_LEN(ranges), &layout));
    assert(layout.range_count == 6 && layout.array_range_count == 6);
    assert(layout.array_bytes == 323U * 694U);
    assert(layout.fixed_tail_bytes == 0 &&
           layout.total_bytes == 323U * 694U);
    assert(ranges[0].offset == 0x38 && ranges[0].length == 323U * 4U);
    assert(ranges[5].offset == 0x97cc &&
           ranges[5].length == 323U * 576U);

    assert(psvr2_gaze_calculate_relocalizer_stale_ranges(
        PSVR2_GAZE_RELOCALIZER_FEATURE_CAPACITY, false,
        ranges, PSVR2_ARRAY_LEN(ranges), &layout));
    assert(layout.range_count == 0 && layout.array_range_count == 0);
    assert(layout.array_bytes == 0 && layout.fixed_tail_bytes == 0 &&
           layout.total_bytes == 0);

    assert(psvr2_gaze_calculate_relocalizer_stale_ranges(
        PSVR2_GAZE_RELOCALIZER_FEATURE_CAPACITY, true,
        ranges, PSVR2_ARRAY_LEN(ranges), &layout));
    assert(layout.range_count == 3 && layout.array_range_count == 0);
    assert(layout.array_bytes == 0 && layout.fixed_tail_bytes == 124 &&
           layout.total_bytes == 124);
    assert(!psvr2_gaze_calculate_relocalizer_stale_ranges(
        PSVR2_GAZE_RELOCALIZER_FEATURE_CAPACITY + 1U, false,
        ranges, PSVR2_ARRAY_LEN(ranges), &layout));
    assert(!psvr2_gaze_calculate_relocalizer_stale_ranges(
        0, true, ranges, PSVR2_ARRAY_LEN(ranges) - 1U, &layout));
    assert(!psvr2_gaze_calculate_relocalizer_stale_ranges(
        0, false, ranges, PSVR2_ARRAY_LEN(ranges), NULL));
}

static void test_relocalizer_packet_validation(void) {
    uint8_t *packet = malloc(PSVR2_GAZE_RELOCALIZER_PACKET_SIZE);
    assert(packet != NULL);
    initialize_relocalizer_packet(packet, UINT32_C(0x12345678), 0);

    psvr2_gaze_relocalizer_header header;
    psvr2_gaze_relocalizer_stale_range ranges[
        PSVR2_GAZE_RELOCALIZER_MAX_STALE_RANGES];
    psvr2_gaze_relocalizer_stale_layout layout;
    assert(psvr2_gaze_parse_relocalizer_header(
        packet, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE, &header));
    assert(header.sequence == UINT32_C(0x12345678));
    assert(header.feature_count == 0);
    assert(psvr2_gaze_relocalizer_packet_stale_ranges(
        packet, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE,
        ranges, PSVR2_ARRAY_LEN(ranges), &header, &layout));
    assert(layout.total_bytes == 224856 &&
           !layout.fixed_tails_included);

    memset(packet + 0x36e94, 0x7f, 0x64000);
    assert(psvr2_gaze_relocalizer_packet_stale_ranges(
        packet, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE,
        ranges, PSVR2_ARRAY_LEN(ranges), &header, &layout));
    assert(layout.array_bytes == 224856 && layout.fixed_tail_bytes == 124);
    assert(layout.total_bytes == 224980 && layout.fixed_tails_included);

    psvr2_store_le32(packet + 0x36e8c, 1);
    assert(psvr2_gaze_relocalizer_packet_stale_ranges(
        packet, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE,
        ranges, PSVR2_ARRAY_LEN(ranges), &header, &layout));
    assert(layout.total_bytes == 224856 &&
           !layout.fixed_tails_included);
    psvr2_store_le32(packet + 0x36e8c, 0);

    psvr2_store_le32(packet + 0x30, 1);
    assert(psvr2_gaze_relocalizer_packet_stale_ranges(
        packet, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE,
        ranges, PSVR2_ARRAY_LEN(ranges), &header, &layout));
    assert(layout.array_bytes == 323U * 694U &&
           layout.fixed_tail_bytes == 0 && !layout.fixed_tails_included);

    psvr2_store_le32(
        packet + 0x30, PSVR2_GAZE_RELOCALIZER_FEATURE_CAPACITY);
    assert(psvr2_gaze_relocalizer_packet_stale_ranges(
        packet, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE,
        ranges, PSVR2_ARRAY_LEN(ranges), &header, &layout));
    assert(layout.range_count == 0 && layout.total_bytes == 0);

    assert(!psvr2_gaze_parse_relocalizer_header(
        packet, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE - 1U, &header));
    packet[0] = 'X';
    assert(!psvr2_gaze_parse_relocalizer_header(
        packet, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE, &header));
    packet[0] = 'R';
    psvr2_store_le32(packet + 4,
                     PSVR2_GAZE_RELOCALIZER_PACKET_SIZE - 1U);
    assert(!psvr2_gaze_parse_relocalizer_header(
        packet, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE, &header));
    psvr2_store_le32(
        packet + 4, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE);
    psvr2_store_le32(packet + 8, 2);
    assert(!psvr2_gaze_parse_relocalizer_header(
        packet, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE, &header));
    psvr2_store_le32(packet + 8, 1);
    packet[0x0c] = 1;
    assert(!psvr2_gaze_parse_relocalizer_header(
        packet, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE, &header));
    packet[0x0c] = 0;
    psvr2_store_le32(
        packet + 0x30, PSVR2_GAZE_RELOCALIZER_FEATURE_CAPACITY + 1U);
    assert(!psvr2_gaze_parse_relocalizer_header(
        packet, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE, &header));
    assert(!psvr2_gaze_relocalizer_packet_stale_ranges(
        packet, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE,
        ranges, PSVR2_ARRAY_LEN(ranges), &header, &layout));
    assert(!psvr2_gaze_parse_relocalizer_header(
        NULL, PSVR2_GAZE_RELOCALIZER_PACKET_SIZE, &header));

    free(packet);
}

static void test_runtime_reconnect_reset(void) {
    psvr2_constants constants;
    assert(psvr2_constants_set_firmware(
        &constants, PSVR2_FW_0600, false));
    constants.live_profile_certified = true;
    constants.certified_connection_generation = 1;
    constants.runtime_work = UINT64_C(0xffffffc012340000);
    constants.runtime_data =
        constants.runtime_work + PSVR2_RUNTIME_DATA_OFFSET;
    psvr2_device device = {.connection_generation = 1};
    psvr2_exploit exploit = {
        .usb = &device,
        .constants = &constants,
        .request_buffer = 0x1234,
        .connection_generation = 1,
        .direct_read_ready = true
    };
    psvr2_krw krw = {
        .ex = &exploit,
        .connection_generation = 1,
        .spinlock = 0x5678,
        .regs = {.valid = true}
    };
    psvr2_runtime runtime;
    psvr2_runtime_init(&runtime, &krw);
    runtime.stage1_loaded = true;
    runtime.mailbox.valid = true;
    runtime.phase = PSVR2_RUNTIME_STAGE1_READY;
    ++device.connection_generation;
    assert(!psvr2_runtime_sync_connection(&runtime));
    assert(runtime.phase == PSVR2_RUNTIME_KERNEL_READY);
    assert(!runtime.stage1_loaded && !runtime.mailbox.valid);
    assert(!krw.regs.valid && krw.spinlock == 0);
    assert(!exploit.direct_read_ready &&
           exploit.request_buffer == 0);
    assert(exploit.connection_generation == 2);
    assert(krw.connection_generation == 2);
    assert(!constants.live_profile_certified);
    assert(constants.certified_connection_generation == 0);
    assert(constants.runtime_work == 0 && constants.runtime_data == 0);
    assert(psvr2_runtime_sync_connection(&runtime));
}

static void test_guarded_overflow_generation(void) {
    psvr2_constants constants;
    assert(psvr2_constants_set_firmware(
        &constants, PSVR2_FW_0600, false));
    constants.live_profile_certified = true;
    constants.certified_connection_generation = 1;
    constants.runtime_work = UINT64_C(0xffffffc012340000);
    constants.runtime_data =
        constants.runtime_work + PSVR2_RUNTIME_DATA_OFFSET;
    psvr2_device device = {.connection_generation = 2};
    psvr2_exploit exploit = {
        .usb = &device,
        .constants = &constants,
        .connection_generation = 2,
        .direct_read_ready = true
    };
    psvr2_krw krw = {
        .ex = &exploit,
        .connection_generation = 2,
        .spinlock = UINT64_C(0xffffffc012345000),
        .regs = {.valid = true}
    };
    const uint8_t payload[] = {0xf0, 0x01};
    bool attempted = true;
    assert(!psvr2_krw_trigger_overflow_ex(
        &krw, payload, sizeof(payload), 1, &attempted));
    assert(!attempted);
    assert(!constants.live_profile_certified);
    assert(constants.certified_connection_generation == 0);
    assert(constants.runtime_work == 0 && constants.runtime_data == 0);
    exploit.constants = NULL;
    attempted = true;
    assert(!psvr2_krw_trigger_overflow_ex(
        &krw, payload, sizeof(payload), 1, &attempted));
    assert(!attempted);

    exploit.constants = &constants;
    constants.live_profile_certified = true;
    constants.certified_connection_generation = 2;
    attempted = true;
    assert(!psvr2_krw_trigger_overflow_ex(
        &krw, payload, sizeof(payload), 1, &attempted));
    assert(!attempted);
    assert(!constants.live_profile_certified);
}

static void test_shell_bulk_reconnect_reset(void) {
    psvr2_device device = {.connection_generation = 7};
    psvr2_exploit exploit = {.usb = &device};
    psvr2_krw krw = {.ex = &exploit};
    psvr2_runtime runtime;
    psvr2_runtime_init(&runtime, &krw);
    psvr2_shell shell = {
        .runtime = &runtime,
        .bulk_interface = 5,
        .bulk_in = 0x82,
        .bulk_out = 0x03,
        .bulk_packet = 512,
        .bulk_claimed = true,
        .bulk_generation = 7
    };
    assert(psvr2_shell_sync_bulk_generation(&shell));
    assert(shell.bulk_claimed && shell.bulk_in == 0x82);

    ++device.connection_generation;
    assert(psvr2_shell_sync_bulk_generation(&shell));
    assert(shell.bulk_generation == 8);
    assert(!shell.bulk_claimed);
    assert(shell.bulk_interface == 5);
    assert(shell.bulk_in == 0 && shell.bulk_out == 0);
    assert(shell.bulk_packet == 0);
}

static void test_vfs_page_map(void) {
    psvr2_vfs_page_map map = {0};
    assert(psvr2_vfs_page_map_put(&map, 9, 0x9000));
    assert(psvr2_vfs_page_map_put(&map, 1, 0x1000));
    assert(psvr2_vfs_page_map_put(&map, 4, 0x4000));
    assert(psvr2_vfs_page_map_put(&map, 4, 0x4400));
    assert(map.count == 3);
    assert(map.entries[0].index == 1);
    assert(map.entries[1].index == 4);
    assert(map.entries[2].index == 9);
    assert(psvr2_vfs_page_map_get(&map, 1) == 0x1000);
    assert(psvr2_vfs_page_map_get(&map, 4) == 0x4400);
    assert(psvr2_vfs_page_map_get(&map, 9) == 0x9000);
    assert(psvr2_vfs_page_map_get(&map, 2) == 0);
    psvr2_vfs_page_map_free(&map);
    assert(!map.entries && map.count == 0 && map.capacity == 0);
}

static void test_structured_core_failures(void) {
    uint8_t byte = 0;
    psvr2_device device = {0};
    assert(psvr2_device_control(
               &device, 0, 0, 0, 0, NULL, 0, 0) ==
           LIBUSB_ERROR_NO_DEVICE);
    assert(device.last_usb_error == LIBUSB_ERROR_NO_DEVICE);
    psvr2_exploit exploit = {0};
    psvr2_result result =
        psvr2_exploit_initialize_result(&exploit, false);
    assert(result.status == PSVR2_STATUS_INVALID_ARGUMENT);
    assert(strstr(result.message, "device") != NULL);
    assert(!psvr2_exploit_initialize(&exploit, false, true));
    result = psvr2_exploit_read_result(
        &exploit, 0x1000, &byte, 1, false);
    assert(result.status == PSVR2_STATUS_INVALID_ARGUMENT);

    psvr2_krw krw = {0};
    result = psvr2_krw_setup_write_result(&krw, false, false);
    assert(result.status == PSVR2_STATUS_INVALID_ARGUMENT);

    psvr2_constants constants;
    assert(psvr2_constants_set_firmware(
        &constants, PSVR2_FW_0600, false));
    constants.fw.exact_image_baseline_present = false;
    exploit.constants = &constants;
    krw.ex = &exploit;
    krw.regs.valid = true;
    result =
        psvr2_krw_kernel_execution_probe_result(&krw, false);
    assert(result.status == PSVR2_STATUS_UNSUPPORTED);
    assert(strstr(result.message, "exact-binary-backed") != NULL);
}

void test_maintenance_helpers(void) {
    test_kernel_list_results();
    test_eye_tracking_commands();
    test_endpoint_filtering();
    test_relocalizer_stale_ranges();
    test_relocalizer_packet_validation();
    test_runtime_reconnect_reset();
    test_guarded_overflow_generation();
    test_shell_bulk_reconnect_reset();
    test_vfs_page_map();
    test_structured_core_failures();
}
