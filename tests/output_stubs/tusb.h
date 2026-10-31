#pragma once
#include <cstdint>
#include <cstring>
#include <cstddef>

constexpr uint8_t TUSB_DESC_ENDPOINT = 5;
constexpr uint8_t CONTROL_STAGE_SETUP = 0;
enum xfer_result_t { XFER_RESULT_SUCCESS, XFER_RESULT_FAILED };
struct tusb_desc_interface_t {
    uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting;
    uint8_t bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
};
struct __attribute__((packed)) tusb_desc_endpoint_t {
    uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
};
struct tusb_control_request_t {
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
};
bool tud_mounted();
bool tud_suspended();
bool tud_control_xfer(uint8_t, const tusb_control_request_t *, void *, uint16_t);
bool tud_disconnect();
bool tud_connect();

constexpr uint8_t TUSB_DESC_DEVICE = 1;
constexpr uint8_t TUSB_DESC_INTERFACE = 4;
constexpr uint8_t TUSB_DESC_STRING = 3;
constexpr uint8_t TUSB_XFER_INTERRUPT = 3;
constexpr uint8_t TUSB_DESC_CONFIG_ATT_SELF_POWERED = 0x40;
constexpr uint8_t HID_ITF_PROTOCOL_NONE = 0;
constexpr uint8_t CFG_TUD_ENDPOINT0_SIZE = 64;
constexpr uint8_t TUD_CONFIG_DESC_LEN = 9;
constexpr uint8_t TUD_HID_DESC_LEN = 25;
#define U16_TO_U8S_LE(v) static_cast<uint8_t>((v) & 255), static_cast<uint8_t>((v) >> 8)
#define TUD_CONFIG_DESCRIPTOR(n, itfs, str, len, attr, ma) \
    9, 2, U16_TO_U8S_LE(len), itfs, n, str, static_cast<uint8_t>(0x80 | (attr)), (ma) / 2
#define TUD_HID_DESCRIPTOR(n, str, protocol, len, ep, size, interval) \
    9, 4, n, 0, 1, 3, 0, protocol, str, 9, 0x21, 0x11, 1, 0, 1, 0x22, \
    U16_TO_U8S_LE(len), 7, 5, ep, 3, U16_TO_U8S_LE(size), interval
struct __attribute__((packed)) tusb_desc_device_t {
    uint8_t bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};
