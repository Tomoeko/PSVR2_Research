#ifndef PSVR2_DEVICE_H
#define PSVR2_DEVICE_H

#include "psvr2/common.h"
#include <libusb.h>

#define PSVR2_DEVICE_STARTUP_TIMEOUT_MS 20000U
#define PSVR2_FIRMWARE_REPORT_ID 0x81U
#define PSVR2_FIRMWARE_REPORT_SIZE 92U
#define PSVR2_PC_TIME_REPORT_ID 0xdeU
#define PSVR2_PC_TIME_REPORT_SIZE 32U

typedef struct psvr2_device {
    libusb_context *ctx;
    libusb_device_handle *handle;
    bool serial_safe;
    double last_keep_alive;
    int configuration;
    int last_usb_error;
    uint64_t connection_generation;
} psvr2_device;

typedef struct {
    int interface_number;
    int alternate_setting;
    uint8_t direction;
    uint8_t transfer_type;
    uint8_t address;
} psvr2_endpoint_query;

typedef struct {
    uint8_t address;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint16_t packet_size;
} psvr2_endpoint;

typedef struct {
    uint8_t report_id;
    uint16_t subcommand;
    uint16_t length;
    uint8_t data[8];
} psvr2_vendor_command;

enum {
    PSVR2_DEVICE_CONNECT_SEND_KEEPALIVE = 1U << 0,
    PSVR2_DEVICE_CONNECT_DETACH_KERNEL_DRIVERS = 1U << 1,
    PSVR2_DEVICE_CONNECT_CONFIGURE = 1U << 2,
    PSVR2_DEVICE_CONNECT_DEFAULT =
        PSVR2_DEVICE_CONNECT_SEND_KEEPALIVE |
        PSVR2_DEVICE_CONNECT_DETACH_KERNEL_DRIVERS |
        PSVR2_DEVICE_CONNECT_CONFIGURE
};

int psvr2_device_init(psvr2_device *dev);
void psvr2_device_close(psvr2_device *dev);
bool psvr2_device_connect_ex(psvr2_device *dev, unsigned flags);
bool psvr2_device_connect(psvr2_device *dev);
bool psvr2_device_reconnect_ex(psvr2_device *dev, unsigned retries,
                               unsigned flags);
bool psvr2_device_reconnect(psvr2_device *dev, unsigned retries);
bool psvr2_device_wait_ready_ex(psvr2_device *dev, unsigned timeout_ms,
                                unsigned flags);
bool psvr2_device_wait_ready(psvr2_device *dev, unsigned timeout_ms);
bool psvr2_device_present(psvr2_device *dev);
bool psvr2_device_get_firmware(psvr2_device *dev, uint32_t *version);
bool psvr2_parse_firmware_response(const void *data, size_t length,
                                   uint32_t *version);
bool psvr2_parse_pc_time_response(const void *data, size_t length,
                                  uint32_t *vts, uint32_t *dp_counter,
                                  uint64_t *stc);
size_t psvr2_build_vendor_report(void *out, size_t out_capacity,
                                 uint8_t report_id, uint16_t subcmd,
                                 const void *data, uint16_t length);
int psvr2_device_control(psvr2_device *dev, uint8_t request_type,
                         uint8_t request, uint16_t value, uint16_t index,
                         void *data, uint16_t length, unsigned timeout_ms);
int psvr2_device_hid_get(psvr2_device *dev, uint8_t report_id,
                         uint8_t sub_id, void *data, uint16_t length);
int psvr2_device_hid_set(psvr2_device *dev, uint8_t report_id,
                         uint8_t sub_id, const void *data, uint16_t length);
bool psvr2_device_vendor_set(psvr2_device *dev, uint8_t report_id,
                             uint16_t subcmd, const void *data, uint16_t length,
                             unsigned timeout_ms);
int psvr2_device_get_config_desc(psvr2_device *dev, void *data, uint16_t length);
size_t psvr2_usb_find_endpoints(
    const struct libusb_config_descriptor *config,
    const psvr2_endpoint_query *query,
    psvr2_endpoint *out, size_t capacity);
size_t psvr2_device_find_endpoints(
    psvr2_device *dev, const psvr2_endpoint_query *query,
    psvr2_endpoint *out, size_t capacity);
bool psvr2_device_claim_interface(psvr2_device *dev, int interface_number);
void psvr2_device_release_interface(psvr2_device *dev, int interface_number);
size_t psvr2_eye_tracking_activation_commands(
    psvr2_vendor_command *out, size_t capacity);
bool psvr2_device_activate_eye_tracking(psvr2_device *dev);
void psvr2_device_send_keep_alive(psvr2_device *dev);
void psvr2_device_check_keep_alive(psvr2_device *dev);

#endif
