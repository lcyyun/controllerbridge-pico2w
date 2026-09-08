#ifndef DS5_BRIDGE_NS2_USB_DESCRIPTORS_H
#define DS5_BRIDGE_NS2_USB_DESCRIPTORS_H

#include <cstdint>

#include "tusb.h"

uint8_t const *ns2_usb_descriptor_device_cb();
uint8_t const *ns2_usb_hid_descriptor_report_cb(uint8_t instance);
uint8_t const *ns2_usb_descriptor_configuration_cb(uint8_t index);
uint8_t const *ns2_usb_descriptor_bos_cb();
uint16_t const *ns2_usb_descriptor_string_cb(uint8_t index, uint16_t langid);
bool ns2_usb_vendor_control_xfer_cb(uint8_t rhport,
                                    uint8_t stage,
                                    tusb_control_request_t const *request);

#endif // DS5_BRIDGE_NS2_USB_DESCRIPTORS_H
