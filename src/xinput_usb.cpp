// SPDX-License-Identifier: MIT
// TinyUSB XUSB transport; protocol descriptors follow the existing BL616
// ControllerBridge reference. No Bluetooth operation executes in a callback.
#include "xinput_usb.h"

#include <cstring>
#include "device/usbd_pvt.h"
#include "profiles/auto_usb.h"

namespace {
constexpr uint8_t ep_in = 0x81;
constexpr uint8_t ep_out = 0x02;
constexpr uint8_t ms_vendor_code = 0xcd;
constexpr size_t input_size = 20;
constexpr size_t output_size = 8;
bool opened;
uint8_t port;
alignas(4) uint8_t input_buffer[32];
alignas(4) uint8_t output_buffer[32];
uint8_t pending_output[output_size];
size_t pending_length;

constexpr uint16_t ms_string[] = {
    0x0312, 'M', 'S', 'F', 'T', '1', '0', '0', ms_vendor_code
};
constexpr uint8_t compat_id[] = {
    40, 0, 0, 0, 0, 1, 4, 0, 1, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 'X', 'U', 'S', 'B', '1', '0', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
constexpr uint8_t input_caps[] = {
    0, 20, 0xff, 0xf3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 1, 0
};
constexpr uint8_t output_caps[] = {0, 8, 0, 0xff, 0xff, 0, 0, 0};
constexpr uint8_t device_id[] = {0x23, 0x50, 0xcb, 0x12};

void reset(uint8_t rhport) {
    port = rhport;
    opened = false;
    pending_length = 0;
    std::memset(input_buffer, 0, sizeof(input_buffer));
    std::memset(output_buffer, 0, sizeof(output_buffer));
}
void init() { reset(0); }
bool deinit() { reset(0); return true; }

uint16_t open(uint8_t rhport, const tusb_desc_interface_t *interface,
              uint16_t max_len) {
    // Return zero for every other vendor interface; the Nintendo vendor driver
    // and ordinary HID/Audio class drivers continue to own their interfaces.
    if (auto_usb_identity() != BridgeUsbIdentity::XInput ||
        interface->bInterfaceClass != 0xff || interface->bInterfaceSubClass != 0x5d ||
        interface->bInterfaceProtocol != 1 || interface->bInterfaceNumber != 0 ||
        interface->bNumEndpoints != 2 || max_len < 40) return 0;
    const auto *base = reinterpret_cast<const uint8_t *>(interface);
    if (base[9] != 17 || base[10] != 0x21 ||
        base[26] != 7 || base[27] != TUSB_DESC_ENDPOINT || base[28] != ep_in ||
        base[33] != 7 || base[34] != TUSB_DESC_ENDPOINT || base[35] != ep_out) return 0;
    if (!usbd_edpt_open(rhport, reinterpret_cast<const tusb_desc_endpoint_t *>(base + 26)) ||
        !usbd_edpt_open(rhport, reinterpret_cast<const tusb_desc_endpoint_t *>(base + 33))) return 0;
    port = rhport;
    opened = true;
    pending_length = 0;
    if (!usbd_edpt_xfer(port, ep_out, output_buffer, sizeof(output_buffer))) {
        opened = false;
        return 0;
    }
    return 40;
}

bool transfer(uint8_t rhport, uint8_t endpoint, xfer_result_t result, uint32_t len) {
    if (endpoint == ep_out) {
        // LED messages use 01/03 and are deliberately not mistaken for rumble.
        // The latest rumble state wins if host packets outpace the main loop.
        if (result == XFER_RESULT_SUCCESS && len >= output_size &&
            output_buffer[0] == 0 && output_buffer[1] == output_size) {
            std::memcpy(pending_output, output_buffer, output_size);
            pending_length = output_size;
        }
        if (opened) usbd_edpt_xfer(rhport, ep_out, output_buffer, sizeof(output_buffer));
    }
    return endpoint == ep_out || endpoint == ep_in;
}

const usbd_class_driver_t driver = {
    .name = "ControllerBridge XUSB",
    .init = init,
    .deinit = deinit,
    .reset = reset,
    .open = open,
    .control_xfer_cb = xinput_usb_control_xfer,
    .xfer_cb = transfer,
    .xfer_isr = nullptr,
    .sof = nullptr,
};
} // namespace

extern "C" const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *count) {
    *count = 1;
    return &driver;
}

bool xinput_usb_ready() {
    return opened && auto_usb_identity() == BridgeUsbIdentity::XInput &&
           !auto_usb_switch_pending() && tud_mounted() && !tud_suspended() &&
           !usbd_edpt_busy(port, ep_in);
}

bool xinput_usb_send(const uint8_t *report, size_t len) {
    if (!report || len != input_size || !xinput_usb_ready()) return false;
    if (!usbd_edpt_claim(port, ep_in)) return false;
    std::memcpy(input_buffer, report, input_size);
    if (usbd_edpt_xfer(port, ep_in, input_buffer, input_size)) return true;
    usbd_edpt_release(port, ep_in);
    return false;
}

size_t xinput_usb_take_output(uint8_t *buffer, size_t capacity) {
    if (!buffer || capacity < pending_length || !pending_length) return 0;
    const size_t len = pending_length;
    std::memcpy(buffer, pending_output, len);
    pending_length = 0;
    return len;
}

const uint16_t *xinput_usb_msos_string() { return ms_string; }

bool xinput_usb_control_xfer(uint8_t rhport, uint8_t stage,
                            const tusb_control_request_t *request) {
    if (auto_usb_identity() != BridgeUsbIdentity::XInput || !request) return false;
    if (stage != CONTROL_STAGE_SETUP) return true;
    const uint8_t *reply = nullptr;
    uint16_t size = 0;
    if (request->bmRequestType == 0xc0 && request->bRequest == ms_vendor_code &&
        request->wIndex == 4 && request->wValue == 0) {
        reply = compat_id; size = sizeof(compat_id);
    } else if (request->bRequest == 1 && request->wIndex == 0) {
        if (request->bmRequestType == 0xc1 && request->wValue == 0x0100) {
            reply = input_caps; size = sizeof(input_caps);
        } else if (request->bmRequestType == 0xc1 && request->wValue == 0) {
            reply = output_caps; size = sizeof(output_caps);
        } else if (request->bmRequestType == 0xc0 && request->wValue == 0) {
            reply = device_id; size = sizeof(device_id);
        }
    }
    return reply && tud_control_xfer(rhport, request, const_cast<uint8_t *>(reply), size);
}
