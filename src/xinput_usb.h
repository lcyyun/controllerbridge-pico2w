#ifndef CONTROLLERBRIDGE_XINPUT_USB_H
#define CONTROLLERBRIDGE_XINPUT_USB_H

#include <cstddef>
#include <cstdint>
#include "tusb.h"

// XUSB interrupt transport. Management remains on the independent HID
// interface, so switching gamepad output never removes configuration access.
bool xinput_usb_ready();
bool xinput_usb_send(const uint8_t *report, size_t len);
// Consume rumble in the application loop, never inside a USB callback.
size_t xinput_usb_take_output(uint8_t *buffer, size_t capacity);
bool xinput_usb_control_xfer(uint8_t rhport, uint8_t stage,
                            const tusb_control_request_t *request);
const uint16_t *xinput_usb_msos_string();

#endif
