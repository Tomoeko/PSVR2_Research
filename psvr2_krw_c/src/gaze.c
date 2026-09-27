#include "psvr2/gaze.h"

#include <string.h>

static uint8_t gaze_probe_sentinel_byte(uint32_t nonce, size_t offset) {
    uint32_t value = nonce ^ (uint32_t)offset;
    value ^= value >> 16;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15;
    value *= UINT32_C(0x846ca68b);
    value ^= value >> 16;
    return (uint8_t)(value >> 24);
}

static bool gaze_probe_sentinel_unchanged(
    const uint8_t *data, size_t start, size_t length, uint32_t nonce) {
    for (size_t offset = start; offset < length; ++offset) {
        if (data[offset] != gaze_probe_sentinel_byte(nonce, offset))
            return false;
    }
    return true;
}

static bool gaze_byte_span_is(
    const uint8_t *data, size_t length, uint8_t value) {
    if (!data) return false;
    for (size_t index = 0; index < length; ++index) {
        if (data[index] != value) return false;
    }
    return true;
}

bool psvr2_gaze_build_calibration_header(
    void *out, size_t capacity, uint16_t channel, uint32_t payload_size) {
    if (!out || capacity < PSVR2_GAZE_CALIBRATION_HEADER_SIZE ||
        (channel != PSVR2_GAZE_CHANNEL_CALIBRATION &&
         channel != PSVR2_GAZE_CHANNEL_IMAGE))
        return false;
    uint8_t *bytes = out;
    memset(bytes, 0, PSVR2_GAZE_CALIBRATION_HEADER_SIZE);
    memcpy(bytes, "GC", 2);
    psvr2_store_le16(
        bytes + 2, PSVR2_GAZE_FIELD_STRUCTURE_VERSION);
    psvr2_store_le16(bytes + 4, PSVR2_GAZE_DATA_VERSION);
    psvr2_store_le16(bytes + 6, channel);
    psvr2_store_le32(bytes + 8, payload_size);
    return true;
}

bool psvr2_gaze_parse_calibration_header(
    const void *data, size_t length, uint16_t expected_channel,
    psvr2_gaze_calibration_header *header) {
    if (!data || length != PSVR2_GAZE_CALIBRATION_HEADER_SIZE ||
        (expected_channel != PSVR2_GAZE_CHANNEL_CALIBRATION &&
         expected_channel != PSVR2_GAZE_CHANNEL_IMAGE))
        return false;
    const uint8_t *bytes = data;
    if (memcmp(bytes, "GC", 2) != 0 ||
        psvr2_load_le16(bytes + 2) !=
            PSVR2_GAZE_FIELD_STRUCTURE_VERSION ||
        psvr2_load_le16(bytes + 4) != PSVR2_GAZE_DATA_VERSION ||
        psvr2_load_le16(bytes + 6) != expected_channel)
        return false;
    if (header) {
        header->channel = expected_channel;
        header->payload_size = psvr2_load_le32(bytes + 8);
        header->calibration_id = psvr2_load_le32(bytes + 12);
        header->calibrated_eye = bytes[16];
    }
    return true;
}

bool psvr2_gaze_parse_stream_status(
    const void *data, size_t length, uint16_t *status) {
    if (!data || !status || length != PSVR2_GAZE_STATUS_REPORT_SIZE)
        return false;
    const uint8_t *bytes = data;
    if (bytes[0] != UINT8_C(0x8c) || bytes[1] != 0)
        return false;
    *status = psvr2_load_le16(bytes + 2);
    return true;
}

bool psvr2_gaze_parse_generic_packet(
    const void *data, size_t length, psvr2_gaze_generic_record *records,
    size_t record_capacity, size_t *record_count) {
    if (!data || !records || !record_count ||
        length < PSVR2_GAZE_GENERIC_HEADER_SIZE ||
        length > PSVR2_GAZE_GENERIC_MAX_PACKET_SIZE)
        return false;

    const uint8_t *bytes = data;
    size_t count = psvr2_load_le16(bytes + 8);
    if (memcmp(bytes, "VD", 2) != 0 ||
        psvr2_load_le16(bytes + 2) != PSVR2_GAZE_GENERIC_HEADER_SIZE ||
        psvr2_load_le32(bytes + 4) != length || count == 0 ||
        count > PSVR2_GAZE_GENERIC_MAX_RECORDS || count > record_capacity)
        return false;

    size_t expected_offset = PSVR2_GAZE_GENERIC_HEADER_SIZE;
    uint16_t bank = 0;
    for (size_t index = 0; index < count; ++index) {
        size_t descriptor_offset = 0x20U +
            index * PSVR2_GAZE_GENERIC_DESCRIPTOR_SIZE;
        const uint8_t *descriptor = bytes + descriptor_offset;
        uint32_t size = psvr2_load_le32(descriptor + 4);
        uint32_t offset = psvr2_load_le32(descriptor + 8);
        if (size > UINT32_MAX - 31U) return false;
        uint32_t padded_size = (size + 31U) & ~UINT32_C(31);
        uint16_t record_bank = psvr2_load_le16(descriptor + 2);
        if (size == 0 || offset != expected_offset ||
            padded_size > length - expected_offset ||
            record_bank >= 8 ||
            (index != 0 && record_bank != bank))
            return false;
        if (index == 0) bank = record_bank;
        records[index] = (psvr2_gaze_generic_record){
            .channel = psvr2_load_le16(descriptor),
            .bank = record_bank,
            .size = size,
            .offset = offset,
            .padded_size = padded_size
        };
        expected_offset += padded_size;
    }
    if (expected_offset != length) return false;
    *record_count = count;
    return true;
}

bool psvr2_gaze_parse_relocalizer_header(
    const void *data, size_t length,
    psvr2_gaze_relocalizer_header *header) {
    if (!data || length != PSVR2_GAZE_RELOCALIZER_PACKET_SIZE)
        return false;

    const uint8_t *bytes = data;
    if (psvr2_load_le16(bytes) != UINT16_C(0x5052) ||
        psvr2_load_le16(bytes + 2) != 1 ||
        psvr2_load_le32(bytes + 4) !=
            PSVR2_GAZE_RELOCALIZER_PACKET_SIZE ||
        psvr2_load_le32(bytes + 8) != 1 ||
        !gaze_byte_span_is(bytes + 0x0c, 0x1c, 0))
        return false;

    uint32_t feature_count = psvr2_load_le32(bytes + 0x30);
    if (feature_count > PSVR2_GAZE_RELOCALIZER_FEATURE_CAPACITY)
        return false;
    if (header) {
        header->sequence = psvr2_load_le32(bytes + 0x28);
        header->feature_count = feature_count;
    }
    return true;
}

bool psvr2_gaze_calculate_relocalizer_stale_ranges(
    uint32_t feature_count, bool include_fixed_tails,
    psvr2_gaze_relocalizer_stale_range *ranges, size_t range_capacity,
    psvr2_gaze_relocalizer_stale_layout *layout) {
    static const size_t array_bases[PSVR2_GAZE_RELOCALIZER_ARRAY_COUNT] = {
        0x34, 0x544, 0x5644, 0x58cc, 0x67fc, 0x958c
    };
    static const size_t array_strides[PSVR2_GAZE_RELOCALIZER_ARRAY_COUNT] = {
        4, 64, 2, 12, 36, 576
    };
    static const psvr2_gaze_relocalizer_stale_range fixed_tails[
        PSVR2_GAZE_RELOCALIZER_FIXED_TAIL_COUNT] = {
        {0x36e90, 4}, {0x9ae94, 44}, {0x9b020, 76}
    };

    if (!layout || (ranges == NULL && range_capacity != 0) ||
        feature_count > PSVR2_GAZE_RELOCALIZER_FEATURE_CAPACITY)
        return false;

    psvr2_gaze_relocalizer_stale_range calculated[
        PSVR2_GAZE_RELOCALIZER_MAX_STALE_RANGES];
    psvr2_gaze_relocalizer_stale_layout result = {
        .fixed_tails_included = include_fixed_tails
    };
    size_t remaining =
        PSVR2_GAZE_RELOCALIZER_FEATURE_CAPACITY - feature_count;
    for (size_t index = 0;
         index < PSVR2_GAZE_RELOCALIZER_ARRAY_COUNT; ++index) {
        if (remaining == 0) break;
        size_t initialized = (size_t)feature_count * array_strides[index];
        size_t length = remaining * array_strides[index];
        calculated[result.range_count++] =
            (psvr2_gaze_relocalizer_stale_range){
                .offset = array_bases[index] + initialized,
                .length = length
            };
        ++result.array_range_count;
        result.array_bytes += length;
    }

    if (include_fixed_tails) {
        for (size_t index = 0;
             index < PSVR2_GAZE_RELOCALIZER_FIXED_TAIL_COUNT; ++index) {
            calculated[result.range_count++] = fixed_tails[index];
            result.fixed_tail_bytes += fixed_tails[index].length;
        }
    }
    result.total_bytes = result.array_bytes + result.fixed_tail_bytes;

    if (ranges && range_capacity < result.range_count) return false;
    if (ranges) {
        memcpy(ranges, calculated,
               result.range_count * sizeof(calculated[0]));
    }
    *layout = result;
    return true;
}

bool psvr2_gaze_relocalizer_packet_stale_ranges(
    const void *data, size_t length,
    psvr2_gaze_relocalizer_stale_range *ranges, size_t range_capacity,
    psvr2_gaze_relocalizer_header *header,
    psvr2_gaze_relocalizer_stale_layout *layout) {
    psvr2_gaze_relocalizer_header parsed;
    if (!psvr2_gaze_parse_relocalizer_header(data, length, &parsed))
        return false;

    const uint8_t *bytes = data;
    bool fallback = parsed.feature_count == 0 &&
        psvr2_load_le32(bytes + 0x36e8c) == 0 &&
        gaze_byte_span_is(bytes + 0x36e94, 0x64000, UINT8_C(0x7f));
    if (!psvr2_gaze_calculate_relocalizer_stale_ranges(
            parsed.feature_count, fallback, ranges, range_capacity,
            layout))
        return false;
    if (header) *header = parsed;
    return true;
}

bool psvr2_gaze_fill_probe_sentinel(
    void *data, size_t length, uint32_t nonce) {
    if (!data || length == 0) return false;
    uint8_t *bytes = data;
    for (size_t offset = 0; offset < length; ++offset)
        bytes[offset] = gaze_probe_sentinel_byte(nonce, offset);
    return true;
}

psvr2_gaze_probe_transfer_classification
psvr2_gaze_classify_probe_transfer(
    bool host_is_darwin, bool transfer_succeeded, bool transfer_timed_out,
    int transferred, const void *data, size_t request_size,
    uint32_t sentinel_nonce) {
    if (!data || request_size == 0 ||
        transferred < 0 || (size_t)transferred > request_size ||
        transfer_succeeded == transfer_timed_out)
        return PSVR2_GAZE_PROBE_TRANSFER_REJECTED;

    size_t transferred_size = (size_t)transferred;
    const uint8_t *bytes = data;
    if (transferred_size == 0) {
        if (!host_is_darwin || gaze_probe_sentinel_unchanged(
                                   bytes, 0, request_size, sentinel_nonce))
            return PSVR2_GAZE_PROBE_TRANSFER_SILENT;
        return PSVR2_GAZE_PROBE_TRANSFER_REJECTED;
    }
    if (host_is_darwin && transfer_timed_out &&
        transferred_size == request_size) {
        return gaze_probe_sentinel_unchanged(
                   bytes, 0, request_size, sentinel_nonce)
                   ? PSVR2_GAZE_PROBE_TRANSFER_SILENT
                   : PSVR2_GAZE_PROBE_TRANSFER_REJECTED;
    }
    if (host_is_darwin && transfer_succeeded &&
        transferred_size == request_size)
        return PSVR2_GAZE_PROBE_TRANSFER_REJECTED;
    if (!transfer_succeeded) return PSVR2_GAZE_PROBE_TRANSFER_REJECTED;
    if (!host_is_darwin)
        return PSVR2_GAZE_PROBE_TRANSFER_DEVICE_DATA;
    return gaze_probe_sentinel_unchanged(
               bytes, transferred_size, request_size, sentinel_nonce) &&
           !gaze_probe_sentinel_unchanged(
               bytes, 0, transferred_size, sentinel_nonce)
               ? PSVR2_GAZE_PROBE_TRANSFER_DEVICE_DATA
               : PSVR2_GAZE_PROBE_TRANSFER_REJECTED;
}
