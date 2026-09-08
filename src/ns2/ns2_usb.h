#ifndef DS5_BRIDGE_NS2_USB_H
#define DS5_BRIDGE_NS2_USB_H

#include <cstddef>
#include <cstdint>

constexpr uint8_t NS2_USB_NINTENDO_INPUT_REPORT_ID = 0x05;
constexpr uint8_t NS2_USB_NINTENDO_OUTPUT_REPORT_ID = 0x02;
constexpr uint8_t NS2_USB_MANAGER_FEATURE_REPORT_ID = 0x7f;
constexpr size_t NS2_USB_NINTENDO_REPORT_SIZE = 64;
constexpr size_t NS2_USB_MANAGER_FEATURE_REPORT_SIZE = 64;

struct Ns2UsbStats {
    bool mounted;
    bool suspended;
    uint32_t reports_sent;
    uint32_t reports_failed;
    uint32_t report_raw_hz;
    uint32_t report_unique_hz;
    uint32_t report_unique_total;
    uint32_t raw_passthrough_reports;
    uint32_t parsed_reports;
    uint16_t report_rate_hz;
    uint32_t report_interval_us;
    uint32_t hid_out_count;
    uint8_t hid_last_report_id;
    uint8_t hid_last_effective_report_id;
    uint8_t hid_last_type;
    uint16_t hid_last_len;
    uint32_t vendor_out_count;
    uint32_t vendor_in_count;
    uint16_t vendor_last_rx_len;
    uint16_t vendor_last_tx_len;
    uint8_t vendor_last_cmd;
    uint8_t vendor_last_arg;
    bool rumble_active;
    uint32_t rumble_updates;
    uint32_t rumble_writes;
    uint32_t rumble_stops;
    uint32_t rumble_errors;
    bool rumble_enabled;
    uint16_t rumble_scale_percent;
    uint16_t rumble_hold_ms;
    uint16_t rumble_tick_ms;
    uint8_t rumble_stop_packets;
    uint32_t feature_set_count;
    uint32_t feature_get_count;
};

void ns2_usb_init();
void ns2_usb_task();
void ns2_usb_get_stats(Ns2UsbStats *out);
bool ns2_usb_handle_debug_command(const char *line, char *out, size_t out_len);

void ns2_usb_mount_cb();
void ns2_usb_umount_cb();
void ns2_usb_suspend_cb(bool remote_wakeup_en);
void ns2_usb_resume_cb();
uint16_t ns2_usb_hid_get_report_cb(uint8_t instance,
                                   uint8_t report_id,
                                   uint8_t report_type,
                                   uint8_t *buffer,
                                   uint16_t reqlen);
void ns2_usb_hid_set_report_cb(uint8_t instance,
                               uint8_t report_id,
                               uint8_t report_type,
                               uint8_t const *buffer,
                               uint16_t bufsize);
void ns2_usb_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize);
void ns2_usb_vendor_tx_cb(uint8_t itf, uint32_t sent_bytes);

#endif
