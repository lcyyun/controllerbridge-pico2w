#pragma once
#include "tusb.h"
struct usbd_class_driver_t {
    const char *name;
    void (*init)();
    bool (*deinit)();
    void (*reset)(uint8_t);
    uint16_t (*open)(uint8_t, const tusb_desc_interface_t *, uint16_t);
    bool (*control_xfer_cb)(uint8_t, uint8_t, const tusb_control_request_t *);
    bool (*xfer_cb)(uint8_t, uint8_t, xfer_result_t, uint32_t);
    bool (*xfer_isr)(uint8_t, uint8_t, xfer_result_t, uint32_t);
    void (*sof)(uint8_t, uint32_t);
};
extern "C" const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *);
bool usbd_edpt_open(uint8_t, const tusb_desc_endpoint_t *);
bool usbd_edpt_xfer(uint8_t, uint8_t, uint8_t *, uint16_t);
bool usbd_edpt_claim(uint8_t, uint8_t);
bool usbd_edpt_release(uint8_t, uint8_t);
bool usbd_edpt_busy(uint8_t, uint8_t);
