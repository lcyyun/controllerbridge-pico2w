// Exercises the actual USB role switch and XUSB class, with host/controller
// endpoints substituted. This does not claim physical USB enumeration success.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include "config.h"
#include "profiles/auto_usb.h"
#include "xinput_usb.h"
#include "device/usbd_pvt.h"

static uint64_t now_us;
static int disconnects, connects, out_arms, releases;
static bool mounted = true, suspended, busy, claim_ok = true, transfer_ok = true;
static uint8_t *out_dma, *in_dma;
static uint16_t in_length;
static std::vector<uint8_t> control;
static std::vector<uint8_t> endpoints;
bool is_dse;
static Config_body config{};
const Config_body &get_config() { return config; }
size_t board_usb_get_serial(uint16_t *p, size_t) {
    for (size_t i = 0; i < 8; ++i) p[i] = "PICO1234"[i];
    return 8;
}
static const uint8_t ns2_marker[] = {0x23};
const uint8_t *ns2_usb_descriptor_device_cb() { return ns2_marker; }
const uint8_t *ns2_usb_descriptor_configuration_cb(uint8_t) { return ns2_marker; }
const uint8_t *ns2_usb_hid_descriptor_report_cb(uint8_t) { return ns2_marker; }
const uint8_t *ns2_usb_descriptor_bos_cb() { return nullptr; }
const uint16_t *ns2_usb_descriptor_string_cb(uint8_t, uint16_t) { return nullptr; }
bool ns2_usb_vendor_control_xfer_cb(uint8_t, uint8_t, const tusb_control_request_t *) { return false; }
const uint8_t *tud_descriptor_device_cb();
const uint8_t *tud_descriptor_configuration_cb(uint8_t);
const uint8_t *tud_hid_descriptor_report_cb(uint8_t);
const uint16_t *tud_descriptor_string_cb(uint8_t, uint16_t);

static uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
static std::string product() {
    const uint16_t *p = tud_descriptor_string_cb(2, 0x409);
    std::string out;
    for (size_t i = 1; i < (p[0] & 255) / 2u; ++i) out += static_cast<char>(p[i]);
    return out;
}

uint64_t time_us_64() { return now_us; }
bool tud_disconnect() { ++disconnects; return true; }
bool tud_connect() { ++connects; return true; }
bool tud_mounted() { return mounted; }
bool tud_suspended() { return suspended; }
bool tud_control_xfer(uint8_t, const tusb_control_request_t *r, void *p, uint16_t n) {
    if (n > r->wLength) n = r->wLength;
    auto *b = static_cast<uint8_t *>(p);
    control.assign(b, b + n); return true;
}
bool usbd_edpt_open(uint8_t, const tusb_desc_endpoint_t *ep) {
    assert(ep->bmAttributes == 3 && ep->wMaxPacketSize == 32);
    endpoints.push_back(ep->bEndpointAddress); return true;
}
bool usbd_edpt_xfer(uint8_t, uint8_t ep, uint8_t *p, uint16_t n) {
    if (!transfer_ok) return false;
    if (ep == 2) { assert(n == 32); out_dma = p; ++out_arms; }
    else { assert(ep == 0x81); in_dma = p; in_length = n; }
    return true;
}
bool usbd_edpt_claim(uint8_t, uint8_t) { return claim_ok; }
bool usbd_edpt_release(uint8_t, uint8_t) { ++releases; return true; }
bool usbd_edpt_busy(uint8_t, uint8_t) { return busy; }

static void select(BridgeUsbIdentity id) {
    auto_usb_request_identity(id, "test");
    now_us += 100000; auto_usb_task();
    now_us += 250000; auto_usb_task();
    assert(auto_usb_identity() == id && !auto_usb_switch_pending());
}

int main() {
    auto_usb_init();
    assert(auto_usb_identity() == BridgeUsbIdentity::DebugOnly);
    auto_usb_request_identity(BridgeUsbIdentity::SonyDs5, "test");
    for (int i = 0; i < 99; ++i) { now_us += 1000; auto_usb_task(); }
    assert(disconnects == 0 && connects == 0);
    now_us += 1000; auto_usb_task();
    assert(disconnects == 1 && connects == 0);
    // A second command while disconnected selects the latest identity and
    // does not reconnect an intermediate role or restart a blocking wait.
    auto_usb_request_identity(BridgeUsbIdentity::SonyDsEdge, "replace");
    now_us += 249999; auto_usb_task(); assert(connects == 0);
    now_us++; auto_usb_task();
    assert(connects == 1 && auto_usb_identity() == BridgeUsbIdentity::SonyDsEdge);
    // The physical controller flag and old config cannot override an explicit
    // USB role. Existing native audio interfaces and management feature remain.
    config.controller_mode = 0; is_dse = false;
    assert(le16(tud_descriptor_device_cb() + 10) == 0x0df2);
    assert(product() == "Pico 2 W DualSense Edge HID");
    const uint8_t *sony_config = tud_descriptor_configuration_cb(0);
    assert(le16(sony_config + 2) == 227 && sony_config[4] == 4);
    const uint8_t *edge_hid = tud_hid_descriptor_report_cb(0);
    assert(edge_hid[0x1bd - 9] == 0x85 && edge_hid[0x1bd - 8] == 0x7f);
    auto_usb_request_identity(BridgeUsbIdentity::SonyDsEdge, "same");
    auto_usb_task(); assert(!auto_usb_switch_pending() && disconnects == 1);
    select(BridgeUsbIdentity::XInput);
    assert(le16(tud_descriptor_device_cb() + 8) == 0xcafe);
    assert(le16(tud_descriptor_device_cb() + 10) == 0x4013);
    assert(product() == "Pico 2 W XInput Bridge");
    const uint8_t *xconfig = tud_descriptor_configuration_cb(0);
    assert(le16(xconfig + 2) == 74 && xconfig[4] == 2);
    assert(xconfig[14] == 0xff && xconfig[15] == 0x5d && xconfig[16] == 1);
    assert(xconfig[49 + 2] == 1 && xconfig[49 + 5] == 3);
    const uint8_t *manager_hid = tud_hid_descriptor_report_cb(0);
    assert(manager_hid[0] == 6 && manager_hid[2] == 0xff && manager_hid[8] == 0x7f);

    uint8_t count = 0;
    const auto *driver = usbd_app_driver_get_cb(&count);
    assert(count == 1 && driver && !driver->xfer_isr);
    driver->init();
    uint8_t descriptor[40] = {
        9, 4, 0, 0, 2, 0xff, 0x5d, 1, 0,
        17, 0x21, 0, 1, 1, 0x25, 0x81, 20, 0, 0, 0, 0, 0x13, 2, 8, 0, 0,
        7, 5, 0x81, 3, 32, 0, 1, 7, 5, 2, 3, 32, 0, 8
    };
    auto *interface = reinterpret_cast<const tusb_desc_interface_t *>(descriptor);
    assert(driver->open(0, interface, 39) == 0);
    descriptor[6] = 0;
    assert(driver->open(0, interface, 40) == 0); // don't steal Nintendo vendor
    descriptor[6] = 0x5d;
    assert(driver->open(0, interface, 40) == 40);
    assert(endpoints == std::vector<uint8_t>({0x81, 2}) && out_arms == 1);

    uint8_t report[20] = {0, 20, 0x11, 0x22};
    assert(xinput_usb_ready());
    assert(!xinput_usb_send(nullptr, 20) && !xinput_usb_send(report, 19));
    assert(xinput_usb_send(report, 20));
    report[2] = 0; // DMA must use owned copy, not the caller's stack buffer
    assert(in_length == 20 && in_dma[2] == 0x11);
    suspended = true; assert(!xinput_usb_ready()); suspended = false;
    busy = true; assert(!xinput_usb_send(report, 20)); busy = false;
    claim_ok = false; assert(!xinput_usb_send(report, 20)); claim_ok = true;
    transfer_ok = false; assert(!xinput_usb_send(report, 20) && releases == 1); transfer_ok = true;

    uint8_t reply[8];
    const uint8_t rumble[] = {0, 8, 0, 0x66, 0x88, 0, 0, 0};
    std::memcpy(out_dma, rumble, 8);
    driver->xfer_cb(0, 2, XFER_RESULT_SUCCESS, 8);
    assert(out_arms == 2 && !xinput_usb_take_output(reply, 7));
    assert(xinput_usb_take_output(reply, 8) == 8 && !std::memcmp(reply, rumble, 8));
    assert(!xinput_usb_take_output(reply, 8));
    out_dma[0] = 1; out_dma[1] = 3; // LED message
    driver->xfer_cb(0, 2, XFER_RESULT_SUCCESS, 3);
    assert(!xinput_usb_take_output(reply, 8) && out_arms == 3);
    driver->xfer_cb(0, 2, XFER_RESULT_FAILED, 8);
    assert(!xinput_usb_take_output(reply, 8) && out_arms == 4);

    tusb_control_request_t request{0xc1, 1, 0x0100, 0, 20};
    assert(xinput_usb_control_xfer(0, 0, &request) && control.size() == 20);
    assert(control[1] == 20 && control[2] == 0xff && control[3] == 0xf3);
    request.wValue = 0; request.wLength = 8;
    assert(xinput_usb_control_xfer(0, 0, &request) && control[1] == 8);
    request = {0xc0, 0xcd, 0, 4, 40};
    assert(xinput_usb_control_xfer(0, 0, &request) && control.size() == 40);
    assert(control[16] == 0 && !std::memcmp(control.data() + 18, "XUSB10", 6));
    request.wIndex = 5; assert(!xinput_usb_control_xfer(0, 0, &request));
    assert(xinput_usb_msos_string()[0] == 0x0312 && xinput_usb_msos_string()[8] == 0xcd);

    auto_usb_request_identity(BridgeUsbIdentity::NintendoSwitchPro, "leave");
    assert(!xinput_usb_ready());
    driver->reset(0); assert(!xinput_usb_ready());
    std::puts("PASS: USB role switching, XUSB endpoints, DMA ownership, controls, rumble/error recovery");
}
