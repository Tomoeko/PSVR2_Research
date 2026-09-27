#include "psvr2/device.h"
#include "psvr2/constants.h"

#include <stdlib.h>
#include <string.h>

static bool active_config_has_serial(psvr2_device *dev) {
    libusb_device *usbdev = libusb_get_device(dev->handle);
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(usbdev, &cfg) != 0) return false;
    bool found = false;
    for (uint8_t i = 0; i < cfg->bNumInterfaces && !found; ++i) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting; ++a) {
            uint8_t cls = iface->altsetting[a].bInterfaceClass;
            if (cls == LIBUSB_CLASS_COMM || cls == LIBUSB_CLASS_DATA) {
                found = true;
                break;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

int psvr2_device_init(psvr2_device *dev) {
    if (!dev) return LIBUSB_ERROR_INVALID_PARAM;
    memset(dev, 0, sizeof(*dev));
    int rc = libusb_init(&dev->ctx);
    dev->last_usb_error = rc;
#if defined(LIBUSB_OPTION_LOG_LEVEL)
    if (rc == 0) libusb_set_option(dev->ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
#endif
    return rc;
}

void psvr2_device_close(psvr2_device *dev) {
    if (!dev) return;
    if (dev->handle) libusb_close(dev->handle);
    if (dev->ctx) libusb_exit(dev->ctx);
    memset(dev, 0, sizeof(*dev));
}

bool psvr2_device_connect_ex(psvr2_device *dev, unsigned flags) {
    if (!dev || !dev->ctx) return false;
    if (dev->handle) {
        libusb_close(dev->handle);
        dev->handle = NULL;
    }
    dev->last_usb_error = LIBUSB_ERROR_NO_DEVICE;
    libusb_device **devices = NULL;
    ssize_t count = libusb_get_device_list(dev->ctx, &devices);
    if (count < 0) {
        dev->last_usb_error = (int)count;
        return false;
    }
    for (ssize_t index = 0; index < count; ++index) {
        struct libusb_device_descriptor descriptor;
        if (libusb_get_device_descriptor(
                devices[index], &descriptor) != 0 ||
            descriptor.idVendor != PSVR2_VID ||
            descriptor.idProduct != PSVR2_PID)
            continue;
        int rc = libusb_open(devices[index], &dev->handle);
        dev->last_usb_error = rc;
        if (rc == 0) break;
    }
    libusb_free_device_list(devices, 1);
    if (!dev->handle) return false;

    dev->serial_safe = active_config_has_serial(dev);
    struct libusb_config_descriptor *cfg = NULL;
    bool has_active_configuration =
        libusb_get_active_config_descriptor(
            libusb_get_device(dev->handle), &cfg) == 0;
    if (has_active_configuration) {
        dev->configuration = cfg->bConfigurationValue;
        libusb_free_config_descriptor(cfg);
    } else {
        dev->configuration = 1;
    }

    if (!dev->serial_safe &&
        (flags & PSVR2_DEVICE_CONNECT_DETACH_KERNEL_DRIVERS)) {
        struct libusb_config_descriptor *active = NULL;
        if (libusb_get_active_config_descriptor(libusb_get_device(dev->handle), &active) == 0) {
            for (uint8_t i = 0; i < active->bNumInterfaces; ++i) {
                int iface = active->interface[i].altsetting[0].bInterfaceNumber;
                if (libusb_kernel_driver_active(dev->handle, iface) == 1)
                    (void)libusb_detach_kernel_driver(dev->handle, iface);
            }
            libusb_free_config_descriptor(active);
        }
    }
    if (!dev->serial_safe &&
        (flags & PSVR2_DEVICE_CONNECT_CONFIGURE)) {
        /*
         * An active descriptor already proves that the device is configured.
         * In particular, do not turn an unsupported get-configuration query
         * into a set-configuration request: that would needlessly reset a
         * live composite device.
         */
        int current = 0;
        int configuration_rc =
            libusb_get_configuration(dev->handle, &current);
        if (!has_active_configuration &&
            configuration_rc == 0 &&
            current != dev->configuration) {
            int rc =
                libusb_set_configuration(
                    dev->handle, dev->configuration);
            if (rc != 0) {
                dev->last_usb_error = rc;
                libusb_close(dev->handle);
                dev->handle = NULL;
                return false;
            }
        }
    }
    if (flags & PSVR2_DEVICE_CONNECT_SEND_KEEPALIVE)
        psvr2_device_send_keep_alive(dev);
    dev->last_usb_error = LIBUSB_SUCCESS;
    ++dev->connection_generation;
    return true;
}

bool psvr2_device_connect(psvr2_device *dev) {
    return psvr2_device_connect_ex(dev, PSVR2_DEVICE_CONNECT_DEFAULT);
}

bool psvr2_device_reconnect_ex(psvr2_device *dev, unsigned retries,
                               unsigned flags) {
    for (unsigned i = 0; i < retries; ++i) {
        if (psvr2_device_connect_ex(dev, flags)) return true;
        if (i + 1U < retries) psvr2_sleep_ms(1000);
    }
    return false;
}

bool psvr2_device_reconnect(psvr2_device *dev, unsigned retries) {
    return psvr2_device_reconnect_ex(
        dev, retries, PSVR2_DEVICE_CONNECT_DEFAULT);
}

bool psvr2_device_wait_ready_ex(psvr2_device *dev, unsigned timeout_ms,
                                unsigned flags) {
    if (!dev || !dev->ctx) return false;
    const unsigned interval_ms = 250U;
    double deadline =
        psvr2_now() + (double)timeout_ms / 1000.0;
    do {
        if (psvr2_device_connect_ex(dev, flags)) return true;
        double remaining = deadline - psvr2_now();
        if (remaining <= 0.0) break;
        unsigned delay = interval_ms;
        if (remaining * 1000.0 < (double)delay)
            delay = (unsigned)(remaining * 1000.0);
        if (delay) psvr2_sleep_ms(delay);
    } while (psvr2_now() < deadline);
    return false;
}

bool psvr2_device_wait_ready(psvr2_device *dev, unsigned timeout_ms) {
    return psvr2_device_wait_ready_ex(
        dev, timeout_ms, PSVR2_DEVICE_CONNECT_DEFAULT);
}

bool psvr2_device_present(psvr2_device *dev) {
    if (!dev || !dev->ctx) return false;
    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(dev->ctx, &list);
    bool found = false;
    for (ssize_t i = 0; i < count; ++i) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) == 0 &&
            desc.idVendor == PSVR2_VID && desc.idProduct == PSVR2_PID) {
            found = true;
            break;
        }
    }
    if (list) libusb_free_device_list(list, 1);
    return found;
}

int psvr2_device_control(psvr2_device *dev, uint8_t request_type,
                         uint8_t request, uint16_t value, uint16_t index,
                         void *data, uint16_t length, unsigned timeout_ms) {
    if (!dev) return LIBUSB_ERROR_INVALID_PARAM;
    if (!dev->handle) {
        dev->last_usb_error = LIBUSB_ERROR_NO_DEVICE;
        return dev->last_usb_error;
    }
    int result =
        libusb_control_transfer(
            dev->handle, request_type, request, value,
            index, data, length, timeout_ms);
    dev->last_usb_error = result < 0 ? result : LIBUSB_SUCCESS;
    if (result == LIBUSB_ERROR_NO_DEVICE && dev->handle) {
        libusb_close(dev->handle);
        dev->handle = NULL;
    }
    return result;
}

bool psvr2_device_get_firmware(psvr2_device *dev, uint32_t *version) {
    uint8_t data[PSVR2_FIRMWARE_REPORT_SIZE] = {0};
    int rc = psvr2_device_control(dev, 0xc2, 0x01,
                                  PSVR2_FIRMWARE_REPORT_ID, 0,
                                  data, sizeof(data), 1000);
    return rc >= 0 && psvr2_parse_firmware_response(data, (size_t)rc, version);
}

bool psvr2_parse_firmware_response(const void *data, size_t length,
                                   uint32_t *version) {
    enum {
        command = 0x01,
        payload_size = 0x54
    };
    if (!data || !version || length != PSVR2_FIRMWARE_REPORT_SIZE)
        return false;
    const uint8_t *bytes = data;
    if (bytes[0] != PSVR2_FIRMWARE_REPORT_ID || bytes[1] != 0 ||
        psvr2_load_le16(bytes + 2) != command ||
        psvr2_load_le16(bytes + 4) != payload_size ||
        bytes[6] != 0 || bytes[7] != 0)
        return false;
    *version = psvr2_load_le32(bytes + 8);
    return true;
}

bool psvr2_parse_pc_time_response(const void *data, size_t length,
                                  uint32_t *vts, uint32_t *dp_counter,
                                  uint64_t *stc) {
    if (!data || length != PSVR2_PC_TIME_REPORT_SIZE)
        return false;
    const uint8_t *bytes = data;
    if (bytes[0] != PSVR2_PC_TIME_REPORT_ID || bytes[1] != 0 ||
        psvr2_load_le16(bytes + 2) != UINT16_C(1) ||
        psvr2_load_le16(bytes + 4) != UINT16_C(24) ||
        bytes[6] != 0 ||
        psvr2_load_le32(bytes + 8) != 0)
        return false;
    /* Exact 06.00 leaves header byte 7 uninitialized, so it is
     * intentionally excluded from validation. */
    if (vts) *vts = psvr2_load_le32(bytes + 12);
    if (dp_counter) *dp_counter = psvr2_load_le32(bytes + 16);
    if (stc) *stc = psvr2_load_le64(bytes + 24);
    return true;
}

size_t psvr2_build_vendor_report(void *out, size_t out_capacity,
                                 uint8_t report_id, uint16_t subcmd,
                                 const void *data, uint16_t length) {
    size_t total = (size_t)length + 8U;
    if (!out || out_capacity < total || (length && !data)) return 0;
    uint8_t *buf = out;
    memset(buf, 0, total);
    buf[0] = report_id;
    psvr2_store_le16(buf + 2, subcmd);
    psvr2_store_le16(buf + 4, length);
    if (length) memcpy(buf + 8, data, length);
    return total;
}

int psvr2_device_hid_get(psvr2_device *dev, uint8_t report_id,
                         uint8_t sub_id, void *data, uint16_t length) {
    return psvr2_device_control(dev, 0xa1, 0x01,
                                (uint16_t)(((uint16_t)sub_id << 8) | report_id),
                                0, data, length, 500);
}

int psvr2_device_hid_set(psvr2_device *dev, uint8_t report_id,
                         uint8_t sub_id, const void *data, uint16_t length) {
    return psvr2_device_control(dev, 0x21, 0x09,
                                (uint16_t)(((uint16_t)sub_id << 8) | report_id),
                                0, (void *)data, length, 500);
}

bool psvr2_device_vendor_set(psvr2_device *dev, uint8_t report_id,
                             uint16_t subcmd, const void *data, uint16_t length,
                             unsigned timeout_ms) {
    if ((size_t)length + 8U > UINT16_MAX) return false;
    uint16_t total = (uint16_t)(length + 8U);
    uint8_t *buf = malloc(total);
    if (!buf) return false;
    if (!psvr2_build_vendor_report(buf, total, report_id, subcmd,
                                   data, length)) {
        free(buf);
        return false;
    }
    int rc = psvr2_device_control(dev, 0x42, 0x09, 0, 0,
                                  buf, total, timeout_ms);
    free(buf);
    return rc == total;
}

int psvr2_device_get_config_desc(psvr2_device *dev, void *data, uint16_t length) {
    return psvr2_device_control(dev, 0x80, 0x06, 0x0200, 1,
                                data, length, 300);
}

size_t psvr2_usb_find_endpoints(
    const struct libusb_config_descriptor *config,
    const psvr2_endpoint_query *query,
    psvr2_endpoint *out, size_t capacity) {
    if (!config || !query || (capacity && !out)) return 0;
    size_t count = 0;
    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const struct libusb_interface *interface = &config->interface[i];
        for (int a = 0; a < interface->num_altsetting; ++a) {
            const struct libusb_interface_descriptor *alternate =
                &interface->altsetting[a];
            if ((query->interface_number >= 0 &&
                 alternate->bInterfaceNumber != query->interface_number) ||
                (query->alternate_setting >= 0 &&
                 alternate->bAlternateSetting != query->alternate_setting))
                continue;
            for (uint8_t e = 0; e < alternate->bNumEndpoints; ++e) {
                const struct libusb_endpoint_descriptor *endpoint =
                    &alternate->endpoint[e];
                if ((endpoint->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) !=
                        query->direction ||
                    (endpoint->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
                        query->transfer_type ||
                    (query->address &&
                     endpoint->bEndpointAddress != query->address))
                    continue;
                if (count < capacity) {
                    out[count] = (psvr2_endpoint){
                        .address = endpoint->bEndpointAddress,
                        .interface_number = alternate->bInterfaceNumber,
                        .alternate_setting = alternate->bAlternateSetting,
                        .packet_size = endpoint->wMaxPacketSize
                    };
                }
                ++count;
            }
        }
    }
    return count < capacity ? count : capacity;
}

size_t psvr2_device_find_endpoints(
    psvr2_device *dev, const psvr2_endpoint_query *query,
    psvr2_endpoint *out, size_t capacity) {
    if (!dev || !dev->handle) return 0;
    struct libusb_config_descriptor *config = NULL;
    if (libusb_get_active_config_descriptor(
            libusb_get_device(dev->handle), &config) != 0)
        return 0;
    size_t count =
        psvr2_usb_find_endpoints(config, query, out, capacity);
    libusb_free_config_descriptor(config);
    return count;
}

bool psvr2_device_claim_interface(psvr2_device *dev, int interface_number) {
    if (!dev || !dev->handle || interface_number < 0) return false;
    if (libusb_kernel_driver_active(dev->handle, interface_number) == 1)
        (void)libusb_detach_kernel_driver(dev->handle, interface_number);
    int result =
        libusb_claim_interface(dev->handle, interface_number);
    dev->last_usb_error = result;
    return result == 0;
}

void psvr2_device_release_interface(psvr2_device *dev, int interface_number) {
    if (dev && dev->handle && interface_number >= 0)
        (void)libusb_release_interface(dev->handle, interface_number);
}

size_t psvr2_eye_tracking_activation_commands(
    psvr2_vendor_command *out, size_t capacity) {
    static const psvr2_vendor_command commands[] = {
        {
            .report_id = 0x0b, .subcommand = 1, .length = 8,
            .data = {1, 0, 0, 0, 5, 0, 0, 0}
        },
        {
            .report_id = 0x17, .subcommand = 1, .length = 2,
            .data = {1, 3}
        },
        {
            .report_id = 0x12, .subcommand = 1, .length = 1,
            .data = {5}
        },
        {.report_id = 0x0c, .subcommand = 1, .length = 0}
    };
    size_t count = PSVR2_ARRAY_LEN(commands);
    if (out && capacity) {
        size_t copied = capacity < count ? capacity : count;
        memcpy(out, commands, copied * sizeof(*out));
    }
    return count;
}

bool psvr2_device_activate_eye_tracking(psvr2_device *dev) {
    /*
     * Match Sony's Windows sequence.  Its driver uses the nonzero 1 MHz VTS
     * at report-0xde payload DWORD +4 as a weak PC-connection readiness
     * heuristic.  This is not a VrhmdMain gaze-start command.  A negative
     * response remains non-fatal so callers can test the activation reports.
     */
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        uint8_t response[PSVR2_PC_TIME_REPORT_SIZE] = {0};
        int received = psvr2_device_control(
            dev, 0xc2, 0x01, PSVR2_PC_TIME_REPORT_ID, 0,
            response, sizeof(response), 1000);
        uint32_t vts = 0;
        if (received >= 0 &&
            psvr2_parse_pc_time_response(
                response, (size_t)received, &vts, NULL, NULL) &&
            vts != 0)
            break;
        if (attempt + 1 < 3)
            psvr2_sleep_ms(500);
    }

    psvr2_vendor_command commands[4];
    size_t count = psvr2_eye_tracking_activation_commands(
        commands, PSVR2_ARRAY_LEN(commands));
    for (size_t i = 0; i < count; ++i) {
        const psvr2_vendor_command *command = &commands[i];
        if (!psvr2_device_vendor_set(
                dev, command->report_id, command->subcommand,
                command->length ? command->data : NULL,
                command->length, 1000))
            return false;
    }
    psvr2_sleep_ms(1000);
    return true;
}

void psvr2_device_send_keep_alive(psvr2_device *dev) {
    if (!dev || !dev->handle) return;
    uint8_t flush[64];
    (void)psvr2_device_hid_get(dev, 0xf2, 0, flush, sizeof(flush));
    uint8_t payload[] = {0x17, 0x00, 0x01, 0x00, 0x02,
                         0x00, 0x00, 0x00, 0x01, 0x03};
    if (psvr2_device_control(dev, 0x42, 0x09, 0, 0,
                             payload, sizeof(payload), 1000) >= 0)
        dev->last_keep_alive = psvr2_now();
}

void psvr2_device_check_keep_alive(psvr2_device *dev) {
    if (dev && psvr2_now() - dev->last_keep_alive > 30.0)
        psvr2_device_send_keep_alive(dev);
}
