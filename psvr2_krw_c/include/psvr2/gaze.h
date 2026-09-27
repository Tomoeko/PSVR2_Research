#ifndef PSVR2_GAZE_H
#define PSVR2_GAZE_H

#include "psvr2/common.h"

#define PSVR2_GAZE_CALIBRATION_HEADER_SIZE 32U
#define PSVR2_GAZE_STATUS_REPORT_SIZE 16U
#define PSVR2_GAZE_FIELD_STRUCTURE_VERSION 0x0300U
#define PSVR2_GAZE_DATA_VERSION 0x0101U
#define PSVR2_GAZE_CHANNEL_CALIBRATION 3U
#define PSVR2_GAZE_CHANNEL_IMAGE 4U
#define PSVR2_GAZE_GENERIC_HEADER_SIZE 0x200U
#define PSVR2_GAZE_GENERIC_DESCRIPTOR_SIZE 12U
#define PSVR2_GAZE_GENERIC_MAX_RECORDS 40U
#define PSVR2_GAZE_GENERIC_MAX_PACKET_SIZE 0x40000U
#define PSVR2_GAZE_PROBE_TRANSFER_SIZE 20000U
#define PSVR2_GAZE_RELOCALIZER_PACKET_SIZE 0xc8780U
#define PSVR2_GAZE_RELOCALIZER_FEATURE_CAPACITY 324U
#define PSVR2_GAZE_RELOCALIZER_ARRAY_COUNT 6U
#define PSVR2_GAZE_RELOCALIZER_FIXED_TAIL_COUNT 3U
#define PSVR2_GAZE_RELOCALIZER_MAX_STALE_RANGES \
    (PSVR2_GAZE_RELOCALIZER_ARRAY_COUNT + \
     PSVR2_GAZE_RELOCALIZER_FIXED_TAIL_COUNT)

typedef enum {
    PSVR2_GAZE_PROBE_TRANSFER_REJECTED,
    PSVR2_GAZE_PROBE_TRANSFER_SILENT,
    PSVR2_GAZE_PROBE_TRANSFER_DEVICE_DATA
} psvr2_gaze_probe_transfer_classification;

typedef struct {
    uint16_t channel;
    uint16_t bank;
    uint32_t size;
    uint32_t offset;
    uint32_t padded_size;
} psvr2_gaze_generic_record;

typedef struct {
    uint16_t channel;
    uint32_t payload_size;
    uint32_t calibration_id;
    uint8_t calibrated_eye;
} psvr2_gaze_calibration_header;

typedef struct {
    uint32_t sequence;
    uint32_t feature_count;
} psvr2_gaze_relocalizer_header;

typedef struct {
    size_t offset;
    size_t length;
} psvr2_gaze_relocalizer_stale_range;

typedef struct {
    size_t range_count;
    size_t array_range_count;
    size_t array_bytes;
    size_t fixed_tail_bytes;
    size_t total_bytes;
    bool fixed_tails_included;
} psvr2_gaze_relocalizer_stale_layout;

bool psvr2_gaze_build_calibration_header(
    void *out, size_t capacity, uint16_t channel, uint32_t payload_size);
bool psvr2_gaze_parse_calibration_header(
    const void *data, size_t length, uint16_t expected_channel,
    psvr2_gaze_calibration_header *header);
bool psvr2_gaze_parse_stream_status(
    const void *data, size_t length, uint16_t *status);
bool psvr2_gaze_parse_generic_packet(
    const void *data, size_t length, psvr2_gaze_generic_record *records,
    size_t record_capacity, size_t *record_count);
bool psvr2_gaze_parse_relocalizer_header(
    const void *data, size_t length,
    psvr2_gaze_relocalizer_header *header);

/*
 * Calculate count-dependent stale spans. include_fixed_tails is deliberately
 * explicit: only the packet-derived helper below may infer it from wire data.
 */
bool psvr2_gaze_calculate_relocalizer_stale_ranges(
    uint32_t feature_count, bool include_fixed_tails,
    psvr2_gaze_relocalizer_stale_range *ranges, size_t range_capacity,
    psvr2_gaze_relocalizer_stale_layout *layout);

/* Validate an exact RP packet and include fixed tails only for its fallback. */
bool psvr2_gaze_relocalizer_packet_stale_ranges(
    const void *data, size_t length,
    psvr2_gaze_relocalizer_stale_range *ranges, size_t range_capacity,
    psvr2_gaze_relocalizer_header *header,
    psvr2_gaze_relocalizer_stale_layout *layout);
bool psvr2_gaze_fill_probe_sentinel(
    void *data, size_t length, uint32_t nonce);
psvr2_gaze_probe_transfer_classification
psvr2_gaze_classify_probe_transfer(
    bool host_is_darwin, bool transfer_succeeded, bool transfer_timed_out,
    int transferred, const void *data, size_t request_size,
    uint32_t sentinel_nonce);

#endif
