//
// Created by awalol on 2026/3/4.
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "bsp/board_api.h"
#include "bt.h"
#include "utils.h"
#include "resample.h"
#include "audio.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/error.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/cyw43_arch.h"
#include "state_mgr.h"
#if ENABLE_SERIAL
#include "pico/stdio_usb.h"
#endif
#include "config.h"
#include "cmd.h"
#if ENABLE_BATT_LED
#include "battery_led.h"
#endif
#if ENABLE_AUTO_PROFILE
#include "profiles/auto_usb.h"
#include "profiles/auto_profile.h"
#include "ns2/ns2_ble.h"
#include "ns2/ns2_input.h"
#include "ns2/ns2_state.h"
#include "ns2/ns2_status.h"
#include "ns2/ns2_usb.h"
#endif

// Pico SDK speciifically for waiting on conditions
#include "pico/critical_section.h"

int reportSeqCounter = 0;
uint8_t packetCounter = 0;
bool spk_active = false;

uint8_t interrupt_in_data[63] = {
    0x7f, 0x7d, 0x7f, 0x7e, 0x00, 0x00, 0xa7,
    0x08, 0x00, 0x00, 0x00, 0x52, 0x43, 0x30, 0x41,
    0x01, 0x00, 0x0e, 0x00, 0xef, 0xff, 0x03, 0x03,
    0x7b, 0x1b, 0x18, 0xf0, 0xcc, 0x9c, 0x60, 0x00,
    0xfc, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xa7, 0xad, 0x60, 0x00, 0x29, 0x18, 0x00,
    0x53, 0x9f, 0x28, 0x35, 0xa5, 0xa8, 0x0c, 0x8b
};

critical_section_t report_cs;
volatile bool report_dirty = false;

#if ENABLE_AUTO_PROFILE
static bool profile_lock_applied = false;
static char auto_line_buffer[160];
static size_t auto_line_len = 0;
static bool auto_manager_bootrom_requested = false;

constexpr uint8_t AUTO_MANAGER_FEATURE_REPORT_ID = 0x7f;
constexpr size_t AUTO_MANAGER_FEATURE_REPORT_SIZE = 64;
constexpr size_t AUTO_MANAGER_REPLY_MAX = 1024;
constexpr char AUTO_MANAGER_SET_MAGIC[] = "Y7HID1";
constexpr char AUTO_MANAGER_REPLY_MAGIC[] = "Y7HRS1";

struct AutoManagerFeatureRuntime {
    uint8_t reply[AUTO_MANAGER_REPLY_MAX];
    uint16_t reply_len;
    uint16_t reply_offset;
    bool reply_complete;
    uint32_t set_count;
    uint32_t get_count;
    char last_command[96];
};

static AutoManagerFeatureRuntime auto_manager{};

struct ReportRateTracker {
    uint32_t raw_total;
    uint32_t unique_total;
    uint32_t raw_window;
    uint32_t unique_window;
    uint32_t raw_hz;
    uint32_t unique_hz;
    uint32_t last_hash;
    bool has_hash;
    uint64_t window_start_us;
};

static ReportRateTracker ds5_usb_rate{};
static uint32_t ds5_usb_failed = 0;

static uint32_t fnv1a32_update(uint32_t hash, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t hash_bytes(const uint8_t *data, size_t len) {
    return fnv1a32_update(2166136261u, data, len);
}

static uint32_t hash_ds5_report_for_dedupe(const uint8_t *data, size_t len) {
    if (!data || len == 0) {
        return 0;
    }
    uint8_t copy[63];
    const size_t copy_len = len < sizeof(copy) ? len : sizeof(copy);
    memcpy(copy, data, copy_len);
    if (copy_len > 6) {
        copy[6] = 0; // sequence/timing byte should not make a duplicate report unique.
    }
    return hash_bytes(copy, copy_len);
}

static void report_rate_note(ReportRateTracker *tracker, uint32_t hash) {
    if (!tracker) {
        return;
    }
    const uint64_t now = time_us_64();
    if (tracker->window_start_us == 0) {
        tracker->window_start_us = now;
    }

    tracker->raw_total++;
    tracker->raw_window++;
    if (!tracker->has_hash || tracker->last_hash != hash) {
        tracker->has_hash = true;
        tracker->last_hash = hash;
        tracker->unique_total++;
        tracker->unique_window++;
    }

    const uint64_t elapsed = now - tracker->window_start_us;
    if (elapsed >= 1000000u) {
        tracker->raw_hz = static_cast<uint32_t>((static_cast<uint64_t>(tracker->raw_window) * 1000000u + elapsed / 2u) / elapsed);
        tracker->unique_hz = static_cast<uint32_t>((static_cast<uint64_t>(tracker->unique_window) * 1000000u + elapsed / 2u) / elapsed);
        tracker->raw_window = 0;
        tracker->unique_window = 0;
        tracker->window_start_us = now;
    }
}

static bool auto_usb_is_nintendo() {
    return auto_usb_identity() == BridgeUsbIdentity::NintendoSwitchPro;
}

static bool auto_usb_is_sony_ds5() {
    return auto_usb_identity() == BridgeUsbIdentity::SonyDs5;
}

static bool auto_usb_is_debug_only() {
    return auto_usb_identity() == BridgeUsbIdentity::DebugOnly;
}

static bool active_profile_connected() {
    if (!auto_profile_locked()) {
        return false;
    }
    if (auto_profile_active() == BridgeProfileId::Ds5FastPath) {
        return bt_classic_connected();
    }
    return ns2_ble_connected();
}

static void reset_auto_session(const char *reason) {
    if (!auto_profile_locked()) {
        return;
    }

    printf("[AUTO] session ended; return to detect reason=%s\n", reason ? reason : "unknown");
    profile_lock_applied = false;
    auto_profile_reset_detection(reason);
    bt_set_classic_enabled(true);
    ns2_ble_resume_auto();
    auto_usb_request_identity(BridgeUsbIdentity::DebugOnly, reason);
}

static void apply_profile_lock_actions() {
    if (profile_lock_applied || !auto_profile_locked()) {
        return;
    }
    profile_lock_applied = true;

    if (auto_profile_active() == BridgeProfileId::Ds5FastPath) {
        printf("[AUTO] apply DS5 fast path; suspend NS2 BLE\n");
        ns2_ble_suspend_auto();
        auto_usb_request_identity(BridgeUsbIdentity::SonyDs5, "ds5_profile_locked");
        return;
    }

    printf("[AUTO] apply NS2 compatibility path; disable BT Classic\n");
    bt_set_classic_enabled(false);
    auto_usb_request_identity(BridgeUsbIdentity::NintendoSwitchPro, "ns2_profile_locked");
}

static void auto_session_tick() {
    if (profile_lock_applied && !active_profile_connected()) {
        reset_auto_session(auto_profile_active() == BridgeProfileId::Ds5FastPath ?
                           "ds5_disconnected" :
                           "ns2_disconnected");
    }
}

static void on_ns2_input_auto(const Ns2InputSnapshot *snapshot) {
    if (!auto_profile_note_ns2_input(snapshot)) {
        return;
    }
    apply_profile_lock_actions();
}

static const char *skip_spaces(const char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static bool arg_is(const char *line, const char *prefix) {
    const size_t len = strlen(prefix);
    return strncmp(line, prefix, len) == 0 && (line[len] == 0 || line[len] == ' ' || line[len] == '\t');
}

static bool command_has_prefix(const char *line, const char *prefix) {
    const size_t len = strlen(prefix);
    return strncmp(line, prefix, len) == 0;
}

static bool parse_next_float(const char **cursor, float *out) {
    if (!cursor || !*cursor || !out) {
        return false;
    }
    char *end = nullptr;
    const float value = strtof(skip_spaces(*cursor), &end);
    if (end == skip_spaces(*cursor)) {
        return false;
    }
    *cursor = end;
    *out = value;
    return true;
}

static bool parse_next_uint(const char **cursor, uint32_t *out) {
    if (!cursor || !*cursor || !out) {
        return false;
    }
    char *end = nullptr;
    const unsigned long value = strtoul(skip_spaces(*cursor), &end, 10);
    if (end == skip_spaces(*cursor)) {
        return false;
    }
    *cursor = end;
    *out = static_cast<uint32_t>(value);
    return true;
}

static void format_ds5_config_json(char *out, size_t out_len, bool saved = false) {
    const Config_body &cfg = get_config();
    int8_t rssi = 0;
    bt_get_signal_strength(&rssi);
    snprintf(out,
             out_len,
             "{\"ok\":true,\"profile\":\"ds5\",\"saved\":%s,\"firmware\":\"%s\","
             "\"rssi\":%d,\"config\":{\"version\":%u,\"haptics_gain\":%.3f,"
             "\"speaker_volume\":%.3f,\"inactive_time\":%u,"
             "\"disable_inactive_disconnect\":%u,\"disable_pico_led\":%u,"
             "\"polling_rate_mode\":%u,\"audio_buffer_length\":%u,"
             "\"controller_mode\":%u}}",
             saved ? "true" : "false",
             PICO_PROGRAM_VERSION_STRING,
             static_cast<int>(rssi),
             static_cast<unsigned>(cfg.config_version),
             static_cast<double>(cfg.haptics_gain),
             static_cast<double>(cfg.speaker_volume),
             static_cast<unsigned>(cfg.inactive_time),
             static_cast<unsigned>(cfg.disable_inactive_disconnect),
             static_cast<unsigned>(cfg.disable_pico_led),
             static_cast<unsigned>(cfg.polling_rate_mode),
             static_cast<unsigned>(cfg.audio_buffer_length),
             static_cast<unsigned>(cfg.controller_mode));
}

static void handle_ds5_command_json(const char *line, char *out, size_t out_len) {
    if (strcmp(line, "ds5 config") == 0 ||
        strcmp(line, "ds5 status") == 0 ||
        strcmp(line, "ds5 settings") == 0) {
        format_ds5_config_json(out, out_len);
        return;
    }

    if (strcmp(line, "ds5 save") == 0 ||
        strcmp(line, "ds5 config save") == 0 ||
        strcmp(line, "ds5 settings save") == 0) {
        const bool ok = config_save();
        if (ok) {
            format_ds5_config_json(out, out_len, true);
        } else {
            snprintf(out, out_len, "{\"ok\":false,\"profile\":\"ds5\",\"error\":\"config_save_failed\"}");
        }
        return;
    }

    if (strcmp(line, "ds5 usb reconnect") == 0 ||
        strcmp(line, "ds5 reconnect usb") == 0 ||
        strcmp(line, "reconnect usb") == 0) {
        tud_disconnect();
        sleep_ms(150);
        tud_connect();
        snprintf(out, out_len, "{\"ok\":true,\"profile\":\"ds5\",\"action\":\"usb_reconnect\"}");
        return;
    }

    if (command_has_prefix(line, "ds5 set")) {
        const char *cursor = line + strlen("ds5 set");
        float haptics = 0.0f;
        float speaker = 0.0f;
        uint32_t inactive = 0;
        uint32_t disable_inactive = 0;
        uint32_t disable_led = 0;
        uint32_t polling = 0;
        uint32_t audio_buffer = 0;
        uint32_t controller = 0;
        if (!parse_next_float(&cursor, &haptics) ||
            !parse_next_float(&cursor, &speaker) ||
            !parse_next_uint(&cursor, &inactive) ||
            !parse_next_uint(&cursor, &disable_inactive) ||
            !parse_next_uint(&cursor, &disable_led) ||
            !parse_next_uint(&cursor, &polling) ||
            !parse_next_uint(&cursor, &audio_buffer) ||
            !parse_next_uint(&cursor, &controller)) {
            snprintf(out,
                     out_len,
                     "{\"ok\":false,\"profile\":\"ds5\",\"error\":\"usage: ds5 set haptics speaker inactive disable_inactive disable_led polling audio_buffer controller\"}");
            return;
        }

        Config_body cfg = get_config();
        cfg.haptics_gain = haptics;
        cfg.speaker_volume = speaker;
        cfg.inactive_time = static_cast<uint8_t>(inactive);
        cfg.disable_inactive_disconnect = static_cast<uint8_t>(disable_inactive ? 1 : 0);
        cfg.disable_pico_led = static_cast<uint8_t>(disable_led ? 1 : 0);
        cfg.polling_rate_mode = static_cast<uint8_t>(polling);
        cfg.audio_buffer_length = static_cast<uint8_t>(audio_buffer);
        cfg.controller_mode = static_cast<uint8_t>(controller);
        set_config(cfg);
        format_ds5_config_json(out, out_len);
        return;
    }

    snprintf(out, out_len, "{\"ok\":false,\"profile\":\"ds5\",\"error\":\"unknown_ds5_command\"}");
}

static void format_auto_status_json(char *out, size_t out_len) {
    char ns2_json[512];
    ns2_status_format_json(ns2_json, sizeof(ns2_json));
    snprintf(out,
             out_len,
             "{\"ok\":true,\"profile\":\"auto\",\"active\":\"%s\",\"locked\":%s,"
             "\"usb\":\"%s\",\"bt_classic_connected\":%s,\"ns2_connected\":%s,"
             "\"ds5_reports\":%lu,\"ns2_reports\":%lu,\"manager_sets\":%lu,"
             "\"manager_gets\":%lu,\"ns2\":%s}",
             auto_profile_active_name(),
             auto_profile_locked() ? "true" : "false",
             auto_usb_identity_name(),
             bt_classic_connected() ? "true" : "false",
             ns2_ble_connected() ? "true" : "false",
             static_cast<unsigned long>(auto_profile_ds5_reports()),
             static_cast<unsigned long>(auto_profile_ns2_reports()),
             static_cast<unsigned long>(auto_manager.set_count),
             static_cast<unsigned long>(auto_manager.get_count),
             ns2_json);
}

static void print_auto_status() {
    char json[768];
    format_auto_status_json(json, sizeof(json));
    printf("%s\n", json);
}

static void format_auto_usb_status_json(char *out, size_t out_len) {
    Ns2UsbStats stats;
    ns2_usb_get_stats(&stats);
    snprintf(out,
             out_len,
             "{\"ok\":true,\"profile\":\"auto\",\"usb\":\"%s\",\"nintendo_mounted\":%s,"
           "\"nintendo_suspended\":%s,\"reports_sent\":%lu,\"reports_failed\":%lu,"
           "\"report_raw_hz\":%lu,\"report_unique_hz\":%lu,"
           "\"hid_out\":%lu,\"vendor_out\":%lu,\"vendor_in\":%lu,"
             "\"report_rate_hz\":%u,\"rumble_active\":%s}",
             auto_usb_identity_name(),
             stats.mounted ? "true" : "false",
             stats.suspended ? "true" : "false",
           static_cast<unsigned long>(stats.reports_sent),
           static_cast<unsigned long>(stats.reports_failed),
           static_cast<unsigned long>(stats.report_raw_hz),
           static_cast<unsigned long>(stats.report_unique_hz),
           static_cast<unsigned long>(stats.hid_out_count),
             static_cast<unsigned long>(stats.vendor_out_count),
             static_cast<unsigned long>(stats.vendor_in_count),
             static_cast<unsigned>(stats.report_rate_hz),
             stats.rumble_active ? "true" : "false");
}

static void print_auto_usb_status() {
    char json[512];
    format_auto_usb_status_json(json, sizeof(json));
    printf("%s\n", json);
}

static int16_t read_le16s_auto(const uint8_t *p) {
    return static_cast<int16_t>(static_cast<uint16_t>(p[0]) |
                                (static_cast<uint16_t>(p[1]) << 8));
}

static uint16_t scale_u8_to_12(uint8_t value) {
    return static_cast<uint16_t>((static_cast<uint32_t>(value) * 4095u + 127u) / 255u);
}

static void get_ds5_report_copy(uint8_t out[63]) {
    critical_section_enter_blocking(&report_cs);
    memcpy(out, interrupt_in_data, 63);
    critical_section_exit(&report_cs);
}

static void format_auto_live_status_json(char *out, size_t out_len) {
    uint8_t ds5_report[63];
    get_ds5_report_copy(ds5_report);

    const uint16_t ds5_lx = scale_u8_to_12(ds5_report[0]);
    const uint16_t ds5_ly = scale_u8_to_12(ds5_report[1]);
    const uint16_t ds5_rx = scale_u8_to_12(ds5_report[2]);
    const uint16_t ds5_ry = scale_u8_to_12(ds5_report[3]);
    const uint32_t ds5_buttons = static_cast<uint32_t>(ds5_report[7]) |
                                 (static_cast<uint32_t>(ds5_report[8]) << 8) |
                                 (static_cast<uint32_t>(ds5_report[9]) << 16);
    const int16_t ds5_acc_x = read_le16s_auto(ds5_report + 15);
    const int16_t ds5_acc_y = read_le16s_auto(ds5_report + 17);
    const int16_t ds5_acc_z = read_le16s_auto(ds5_report + 19);
    const int16_t ds5_gyr_x = read_le16s_auto(ds5_report + 21);
    const int16_t ds5_gyr_y = read_le16s_auto(ds5_report + 23);
    const int16_t ds5_gyr_z = read_le16s_auto(ds5_report + 25);
    const uint32_t ds5_hash = hash_ds5_report_for_dedupe(ds5_report, sizeof(ds5_report));

    Ns2InputSnapshot ns2_input{};
    const bool ns2_valid = ns2_input_get_snapshot(&ns2_input);
    Ns2MotionSample ns2_motion{};
    const bool ns2_motion_valid = ns2_input_get_motion_sample(&ns2_motion);
    Ns2UsbStats ns2_stats{};
    ns2_usb_get_stats(&ns2_stats);

    snprintf(out,
             out_len,
             "{\"ok\":true,\"profile\":\"auto\",\"active\":\"%s\",\"usb\":\"%s\",\"locked\":%s,"
             "\"ds5\":{\"connected\":%s,\"valid\":%s,\"lx\":%u,\"ly\":%u,\"rx\":%u,\"ry\":%u,"
             "\"buttons\":%lu,\"accX\":%d,\"accY\":%d,\"accZ\":%d,\"gyrX\":%d,\"gyrY\":%d,\"gyrZ\":%d,"
             "\"bt_reports\":%lu,\"usb_raw_hz\":%lu,\"usb_unique_hz\":%lu,\"usb_raw_total\":%lu,"
             "\"usb_unique_total\":%lu,\"usb_failed\":%lu,\"sample_hash\":%lu},"
             "\"ns2\":{\"connected\":%s,\"valid\":%s,\"kind\":\"%s\",\"lx\":%u,\"ly\":%u,\"rx\":%u,\"ry\":%u,"
             "\"buttons\":%lu,\"accX\":%d,\"accY\":%d,\"accZ\":%d,\"gyrX\":%d,\"gyrY\":%d,\"gyrZ\":%d,"
             "\"updates\":%lu,\"usb_raw_hz\":%lu,\"usb_unique_hz\":%lu,\"usb_raw_total\":%lu,"
             "\"usb_unique_total\":%lu,\"usb_failed\":%lu}}",
             auto_profile_active_name(),
             auto_usb_identity_name(),
             auto_profile_locked() ? "true" : "false",
             bt_classic_connected() ? "true" : "false",
             bt_classic_connected() ? "true" : "false",
             static_cast<unsigned>(ds5_lx),
             static_cast<unsigned>(ds5_ly),
             static_cast<unsigned>(ds5_rx),
             static_cast<unsigned>(ds5_ry),
             static_cast<unsigned long>(ds5_buttons),
             static_cast<int>(ds5_acc_x),
             static_cast<int>(ds5_acc_y),
             static_cast<int>(ds5_acc_z),
             static_cast<int>(ds5_gyr_x),
             static_cast<int>(ds5_gyr_y),
             static_cast<int>(ds5_gyr_z),
             static_cast<unsigned long>(auto_profile_ds5_reports()),
             static_cast<unsigned long>(ds5_usb_rate.raw_hz),
             static_cast<unsigned long>(ds5_usb_rate.unique_hz),
             static_cast<unsigned long>(ds5_usb_rate.raw_total),
             static_cast<unsigned long>(ds5_usb_rate.unique_total),
             static_cast<unsigned long>(ds5_usb_failed),
             static_cast<unsigned long>(ds5_hash),
             ns2_ble_connected() ? "true" : "false",
             ns2_valid ? "true" : "false",
             ns2_valid ? ns2_input_kind_name(ns2_input.kind) : "UNK",
             ns2_valid ? static_cast<unsigned>(ns2_input.lx) : 2048u,
             ns2_valid ? static_cast<unsigned>(ns2_input.ly) : 2048u,
             ns2_valid ? static_cast<unsigned>(ns2_input.rx) : 2048u,
             ns2_valid ? static_cast<unsigned>(ns2_input.ry) : 2048u,
             ns2_valid ? static_cast<unsigned long>(ns2_input.buttons) : 0ul,
             ns2_motion_valid ? static_cast<int>(ns2_motion.accel[0]) : 0,
             ns2_motion_valid ? static_cast<int>(ns2_motion.accel[1]) : 0,
             ns2_motion_valid ? static_cast<int>(ns2_motion.accel[2]) : 0,
             ns2_motion_valid ? static_cast<int>(ns2_motion.gyro[0]) : 0,
             ns2_motion_valid ? static_cast<int>(ns2_motion.gyro[1]) : 0,
             ns2_motion_valid ? static_cast<int>(ns2_motion.gyro[2]) : 0,
             ns2_valid ? static_cast<unsigned long>(ns2_input.updates) : 0ul,
             static_cast<unsigned long>(ns2_stats.report_raw_hz),
             static_cast<unsigned long>(ns2_stats.report_unique_hz),
             static_cast<unsigned long>(ns2_stats.reports_sent),
             static_cast<unsigned long>(ns2_stats.report_unique_total),
             static_cast<unsigned long>(ns2_stats.reports_failed));
}

static void format_auto_motion_status_json(char *out, size_t out_len) {
    Ns2InputSnapshot input;
    const bool valid = ns2_input_get_snapshot(&input);
    char motion_json[384];
    ns2_input_format_motion_json(valid ? &input : nullptr, motion_json, sizeof(motion_json));
    snprintf(out,
             out_len,
             "{\"ok\":true,\"profile\":\"auto\",\"input_valid\":%s,"
             "\"kind\":\"%s\",\"len\":%u,\"updates\":%lu,"
             "\"buttons\":%lu,\"lx\":%u,\"ly\":%u,\"rx\":%u,\"ry\":%u,%s}",
             valid ? "true" : "false",
             valid ? ns2_input_kind_name(input.kind) : "UNK",
             valid ? static_cast<unsigned>(input.len) : 0u,
             valid ? static_cast<unsigned long>(input.updates) : 0ul,
             valid ? static_cast<unsigned long>(input.buttons) : 0ul,
             valid ? static_cast<unsigned>(input.lx) : 2048u,
             valid ? static_cast<unsigned>(input.ly) : 2048u,
             valid ? static_cast<unsigned>(input.rx) : 2048u,
             valid ? static_cast<unsigned>(input.ry) : 2048u,
             motion_json);
}

static void print_auto_motion_status() {
    char json[768];
    format_auto_motion_status_json(json, sizeof(json));
    printf("%s\n", json);
}

static void print_auto_candidates() {
    char json[768];
    ns2_status_format_candidates_json(json, sizeof(json));
    printf("%s\n", json);
}

static void print_auto_help() {
    printf("commands: status | usb status | motion status | stick recalibrate | reset detect | ns2 scan | ns2 pair | ns2 reconnect | ns2 disconnect | ns2 forget | ns2 auto on|off | ns2 candidates | bootrom\n");
}

static bool handle_auto_command_json(const char *raw_line, char *out, size_t out_len) {
    const char *line = skip_spaces(raw_line);
    if (!out || out_len == 0) {
        return false;
    }
    if (line[0] == 0) {
        snprintf(out, out_len, "{\"ok\":false,\"error\":\"empty_command\"}");
        return true;
    }

    if (strcmp(line, "status") == 0 || strcmp(line, "auto status") == 0) {
        format_auto_status_json(out, out_len);
        return true;
    }
    if (strcmp(line, "usb status") == 0) {
        format_auto_usb_status_json(out, out_len);
        return true;
    }
    if (strcmp(line, "live status") == 0 || strcmp(line, "input status") == 0) {
        format_auto_live_status_json(out, out_len);
        return true;
    }
    if (arg_is(line, "ds5") || strcmp(line, "reconnect usb") == 0) {
        handle_ds5_command_json(line, out, out_len);
        return true;
    }
    if (strcmp(line, "motion status") == 0 || strcmp(line, "imu status") == 0) {
        format_auto_motion_status_json(out, out_len);
        return true;
    }
    if (strcmp(line, "reset detect") == 0 || strcmp(line, "auto reset") == 0) {
        reset_auto_session("webui_reset");
        format_auto_status_json(out, out_len);
        return true;
    }
    if (strcmp(line, "ns2 scan") == 0) {
        ns2_ble_resume_auto();
        ns2_ble_start_scan();
        format_auto_status_json(out, out_len);
        return true;
    }
    if (strcmp(line, "ns2 pair") == 0) {
        ns2_ble_resume_auto();
        ns2_ble_pair();
        format_auto_status_json(out, out_len);
        return true;
    }
    if (strcmp(line, "ns2 reconnect") == 0) {
        ns2_ble_resume_auto();
        ns2_ble_reconnect();
        format_auto_status_json(out, out_len);
        return true;
    }
    if (strcmp(line, "ns2 disconnect") == 0) {
        ns2_ble_disconnect();
        format_auto_status_json(out, out_len);
        return true;
    }
    if (strcmp(line, "ns2 forget") == 0) {
        ns2_ble_forget();
        format_auto_status_json(out, out_len);
        return true;
    }
    if (strcmp(line, "ns2 candidates") == 0) {
        ns2_status_format_candidates_json(out, out_len);
        return true;
    }
    if (arg_is(line, "ns2 auto")) {
        const char *arg = skip_spaces(line + strlen("ns2 auto"));
        if (strcmp(arg, "on") == 0) {
            ns2_ble_set_auto_connect(true);
            format_auto_status_json(out, out_len);
            return true;
        }
        if (strcmp(arg, "off") == 0) {
            ns2_ble_set_auto_connect(false);
            format_auto_status_json(out, out_len);
            return true;
        }
    }
    if (arg_is(line, "rumble") || arg_is(line, "usb rate") || arg_is(line, "usb raw") ||
        arg_is(line, "report rate") || arg_is(line, "settings") || arg_is(line, "config") ||
        arg_is(line, "web parse") || arg_is(line, "webui parse") ||
        strcmp(line, "usb config") == 0 || strcmp(line, "report config") == 0) {
        if (ns2_usb_handle_debug_command(line, out, out_len)) {
            return true;
        }
    }
    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        snprintf(out,
                 out_len,
                 "{\"ok\":true,\"commands\":\"status | usb status | motion status | stick recalibrate | reset detect | ns2 scan | ns2 pair | ns2 reconnect | ns2 disconnect | ns2 forget | ns2 auto on|off | ns2 candidates | bootrom\"}");
        return true;
    }
    if (strcmp(line, "bootrom") == 0 || strcmp(line, "uf2") == 0) {
        auto_manager_bootrom_requested = true;
        snprintf(out, out_len, "{\"ok\":true,\"action\":\"bootrom\"}");
        return true;
    }

    snprintf(out, out_len, "{\"ok\":false,\"error\":\"unknown command\"}");
    return true;
}

static void handle_auto_command(const char *raw_line) {
    const char *line = skip_spaces(raw_line);
    if (line[0] == 0) {
        return;
    }
    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        print_auto_help();
        return;
    }

    char json[768];
    if (handle_auto_command_json(line, json, sizeof(json))) {
        printf("%s\n", json);
        if (auto_manager_bootrom_requested) {
            sleep_ms(100);
            reset_usb_boot(0, 0);
        }
    }
}

static void auto_manager_queue_json(const char *json) {
    if (!json) {
        json = "{\"ok\":false,\"error\":\"empty_reply\"}";
    }
    const size_t len = strlen(json);
    auto_manager.reply_len = static_cast<uint16_t>(len < sizeof(auto_manager.reply) ? len : sizeof(auto_manager.reply));
    memcpy(auto_manager.reply, json, auto_manager.reply_len);
    auto_manager.reply_offset = 0;
    auto_manager.reply_complete = auto_manager.reply_len == 0;
}

static void auto_manager_receive_feature_command(const uint8_t *payload, uint16_t payload_size) {
    auto_manager.set_count++;
    if (!payload || payload_size == 0) {
        auto_manager_queue_json("{\"ok\":false,\"error\":\"empty_feature_report\"}");
        return;
    }
    if (payload[0] == AUTO_MANAGER_FEATURE_REPORT_ID) {
        payload++;
        payload_size--;
    }
    if (payload_size < strlen(AUTO_MANAGER_SET_MAGIC) ||
        memcmp(payload, AUTO_MANAGER_SET_MAGIC, strlen(AUTO_MANAGER_SET_MAGIC)) != 0) {
        auto_manager_queue_json("{\"ok\":false,\"error\":\"bad_magic\"}");
        return;
    }

    payload += strlen(AUTO_MANAGER_SET_MAGIC);
    payload_size = static_cast<uint16_t>(payload_size - strlen(AUTO_MANAGER_SET_MAGIC));
    while (payload_size > 0 && payload[payload_size - 1] == 0) {
        payload_size--;
    }

    const size_t copy_len = payload_size < sizeof(auto_manager.last_command) - 1 ?
        payload_size :
        sizeof(auto_manager.last_command) - 1;
    memcpy(auto_manager.last_command, payload, copy_len);
    auto_manager.last_command[copy_len] = 0;

    char json[AUTO_MANAGER_REPLY_MAX];
    handle_auto_command_json(auto_manager.last_command, json, sizeof(json));
    auto_manager_queue_json(json);
}

static uint16_t auto_manager_build_feature_report(uint8_t *buffer, uint16_t reqlen) {
    if (!buffer || reqlen == 0) {
        return 0;
    }

    auto_manager.get_count++;
    if (auto_manager.reply_len == 0) {
        char json[768];
        format_auto_status_json(json, sizeof(json));
        auto_manager_queue_json(json);
    }

    memset(buffer, 0, reqlen);
    if (reqlen < 11) {
        return reqlen;
    }

    memcpy(buffer, AUTO_MANAGER_REPLY_MAGIC, strlen(AUTO_MANAGER_REPLY_MAGIC));
    const uint16_t total = auto_manager.reply_len;
    uint16_t offset = auto_manager.reply_offset;
    if (offset > total) {
        offset = total;
        auto_manager.reply_offset = total;
    }

    const uint16_t remaining = static_cast<uint16_t>(total - offset);
    const uint16_t chunk_max = static_cast<uint16_t>(reqlen - 11);
    const uint16_t chunk_len = remaining < chunk_max ? remaining : chunk_max;

    buffer[6] = static_cast<uint8_t>(total & 0xff);
    buffer[7] = static_cast<uint8_t>((total >> 8) & 0xff);
    buffer[8] = static_cast<uint8_t>(offset & 0xff);
    buffer[9] = static_cast<uint8_t>((offset >> 8) & 0xff);
    buffer[10] = static_cast<uint8_t>(chunk_len);

    if (chunk_len > 0) {
        memcpy(buffer + 11, auto_manager.reply + offset, chunk_len);
        auto_manager.reply_offset = static_cast<uint16_t>(offset + chunk_len);
        if (auto_manager.reply_offset >= total) {
            auto_manager.reply_complete = true;
            if (!auto_manager_bootrom_requested) {
                auto_manager.reply_len = 0;
                auto_manager.reply_offset = 0;
            }
        }
    }

    return reqlen;
}

static void auto_serial_poll() {
    while (true) {
        const int ch = getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT) {
            return;
        }
        if (ch == '\r' || ch == '\n') {
            if (auto_line_len > 0) {
                auto_line_buffer[auto_line_len] = 0;
                handle_auto_command(auto_line_buffer);
                auto_line_len = 0;
            }
            continue;
        }
        if (ch == 0x08 || ch == 0x7f) {
            if (auto_line_len > 0) {
                auto_line_len--;
            }
            continue;
        }
        if (auto_line_len + 1 < sizeof(auto_line_buffer)) {
            auto_line_buffer[auto_line_len++] = static_cast<char>(ch);
        }
    }
}
#endif

void interrupt_loop() {
    if (!tud_hid_ready()) return;

    // TODO: Refactor for better code reuse
    if (get_config().polling_rate_mode != 2) {
        if (!tud_hid_report(0x01, interrupt_in_data, 63)) {
            printf("[USBHID] tud_hid_report error\n");
#if ENABLE_AUTO_PROFILE
            ds5_usb_failed++;
#endif
        } else {
#if ENABLE_AUTO_PROFILE
            report_rate_note(&ds5_usb_rate, hash_ds5_report_for_dedupe(interrupt_in_data, 63));
#endif
        }
        return;
    }

    bool should_send = false;
    // Local buffer to hold the report data while we prepare it to send. 
    uint8_t safe_report[63];


    critical_section_enter_blocking(&report_cs);
    if (report_dirty) {
        memcpy(safe_report, interrupt_in_data, 63);
        report_dirty = false;
        should_send = true;
    }
    critical_section_exit(&report_cs);

    // Only send to TinyUSB if we actually grabbed fresh data
    if (should_send) {
        if (!tud_hid_report(0x01, safe_report, 63)) {
            printf("[USBHID] tud_hid_report error\n");
#if ENABLE_AUTO_PROFILE
            ds5_usb_failed++;
#endif

            // If the report failed to queue, restore the dirty flag 
            // so we try again on the next loop iteration.
            critical_section_enter_blocking(&report_cs);
            report_dirty = true;
            critical_section_exit(&report_cs);
#if ENABLE_AUTO_PROFILE
        } else {
            report_rate_note(&ds5_usb_rate, hash_ds5_report_for_dedupe(safe_report, 63));
#endif
        }
    }
}

void on_bt_data(CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
    // printf("[Main] BT data callback: channel=%u len=%u\n", channel, len);
    if (channel == INTERRUPT && data[1] == 0x31) {
#if ENABLE_AUTO_PROFILE
        if (!auto_profile_note_ds5_input(len)) {
            return;
        }
        apply_profile_lock_actions();
#endif
        if ((data[56] & 1) != (interrupt_in_data[53] & 1)) {
            set_headset(data[56] & 1);
        }

        if (get_config().polling_rate_mode != 2) {
            memcpy(interrupt_in_data, data + 3, 63);
#if ENABLE_BATT_LED
            battery_led_note_report();
#endif
            return;
        }

        // We add the critical section here to avoid any race conditions when writing to the interrupt_in_data buffer,
        // which is shared between the main loop and this callback. 
        // The critical section ensures that only one thread can access the buffer at a time, 
        // preventing data corruption and ensuring thread safety.   
        // We also set the report_dirty flag to true to indicate that new data is available
        //  and needs to be sent in the next interrupt report.
        critical_section_enter_blocking(&report_cs);
        memcpy(interrupt_in_data, data + 3, 63);
        report_dirty = true;
        critical_section_exit(&report_cs);
#if ENABLE_BATT_LED
        battery_led_note_report();
#endif
    }
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
#if ENABLE_AUTO_PROFILE
    if (report_type == HID_REPORT_TYPE_FEATURE &&
        (report_id == 0 || report_id == AUTO_MANAGER_FEATURE_REPORT_ID)) {
        (void)itf;
        return auto_manager_build_feature_report(buffer, reqlen);
    }
    if (auto_usb_is_debug_only()) {
        (void)itf;
        return 0;
    }
    if (auto_usb_is_nintendo()) {
        return ns2_usb_hid_get_report_cb(itf,
                                         report_id,
                                         static_cast<uint8_t>(report_type),
                                         buffer,
                                         reqlen);
    }
#endif
    (void) itf;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;

    if (is_pico_cmd(report_id)) {
        return pico_cmd_get(report_id, buffer, reqlen);
    }

    std::vector<uint8_t> feature_data = get_feature_data(report_id, reqlen);
    if (!feature_data.empty()) {
        memcpy(buffer, feature_data.data() + 1, feature_data.size() - 1);
    }

    return feature_data.empty() ? 0 : feature_data.size() - 1;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void) rhport;
    uint8_t const itf = tu_u16_low(p_request->wIndex); // wInterface
    uint8_t const alt = tu_u16_low(p_request->wValue); // bAlternateSetting

    if (itf == 1) {
        printf("[AUDIO] Set interface Speaker to alternate setting %d\n", alt);
        spk_active = alt;
    }

    return true;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
#if ENABLE_AUTO_PROFILE
    if (report_type == HID_REPORT_TYPE_FEATURE) {
        uint8_t effective_report_id = report_id;
        uint8_t const *payload = buffer;
        uint16_t payload_size = bufsize;
        if (effective_report_id == 0 && buffer && bufsize > 0) {
            effective_report_id = buffer[0];
            payload = buffer + 1;
            payload_size = static_cast<uint16_t>(bufsize - 1);
        }
        if (effective_report_id == AUTO_MANAGER_FEATURE_REPORT_ID) {
            (void)itf;
            auto_manager_receive_feature_command(payload, payload_size);
            return;
        }
    }
    if (auto_usb_is_debug_only()) {
        (void)itf;
        return;
    }
    if (auto_usb_is_nintendo()) {
        ns2_usb_hid_set_report_cb(itf,
                                  report_id,
                                  static_cast<uint8_t>(report_type),
                                  buffer,
                                  bufsize);
        return;
    }
#endif
    (void) itf;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) bufsize;

    if (is_pico_cmd(report_id)) {
        printf("[HID] Receive 0xf6 setting config, funcid:0x%02X\n", buffer[0]);
        pico_cmd_set(report_id, buffer, bufsize);
        return;
    }

    // INTERRUPT OUT
    if (report_id == 0) {
        switch (buffer[0]) {
            case 0x02: {
                state_update(buffer + 1, bufsize - 1);
                if (spk_active) {
                    break;
                }
                uint8_t outputData[78]{};
                outputData[0] = 0x31;
                outputData[1] = reportSeqCounter << 4;
                if (++reportSeqCounter == 256) {
                    reportSeqCounter = 0;
                }
                outputData[2] = 0x10;
                // memcpy(outputData + 3, buffer + 1, bufsize - 1);
                state_set(outputData + 3,sizeof(SetStateData));
                bt_write(outputData, sizeof(outputData));
                break;
            }
        }
    }
    if (report_id == 0x80 ||
        // DSE: Write Profile Block
        report_id == 0x60 ||
        report_id == 0x62 ||
        report_id == 0x61) {
        set_feature_data(report_id, const_cast<uint8_t *>(buffer), bufsize);
        return;
    }
}

#if ENABLE_AUTO_PROFILE
void tud_mount_cb(void) {
    if (auto_usb_is_nintendo()) {
        ns2_usb_mount_cb();
    }
}

void tud_umount_cb(void) {
    if (auto_usb_is_nintendo()) {
        ns2_usb_umount_cb();
    }
}

void tud_suspend_cb(bool remote_wakeup_en) {
    if (auto_usb_is_nintendo()) {
        ns2_usb_suspend_cb(remote_wakeup_en);
    }
}

void tud_resume_cb(void) {
    if (auto_usb_is_nintendo()) {
        ns2_usb_resume_cb();
    }
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize) {
    if (auto_usb_is_nintendo()) {
        ns2_usb_vendor_rx_cb(itf, buffer, bufsize);
    }
}

void tud_vendor_tx_cb(uint8_t itf, uint32_t sent_bytes) {
    if (auto_usb_is_nintendo()) {
        ns2_usb_vendor_tx_cb(itf, sent_bytes);
    }
}
#endif

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(1000);
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);

    board_init();
    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);
#if !ENABLE_SERIAL
    tud_disconnect();
#endif
    board_init_after_tusb();
#if ENABLE_SERIAL
    stdio_usb_init();
#endif
#if ENABLE_AUTO_PROFILE
    auto_usb_init();
#endif

    if (cyw43_arch_init()) {
        printf("Failed to initialize CYW43\n");
        return 1;
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);

#if ENABLE_BATT_LED
    battery_led_init();
#endif

#if !ENABLE_SERIAL
    if (watchdog_caused_reboot()) {
        printf("Rebooted by Watchdog!\n");
        // 当崩溃重启以后，闪三下灯
        for (int i = 0; i < 6; i++) {
            if (i % 2 == 0) {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
            } else {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
            }
            sleep_ms(500);
        }
    } else {
        printf("Clean boot\n");
    }
#endif

    // Initialize the critical section for the report buffer
    critical_section_init(&report_cs);

    config_load();

    bt_init();
    bt_register_data_callback(on_bt_data);

#if ENABLE_AUTO_PROFILE
    auto_profile_init();
    ns2_status_init();
    ns2_config_load();
    ns2_input_register_callback(on_ns2_input_auto);
    ns2_ble_init_for_shared_hci();
    ns2_usb_init();
#endif

    audio_init();
    state_init();

#if !ENABLE_SERIAL
    watchdog_enable(1000, true);
#endif

    while (1) {
#if !ENABLE_SERIAL
        watchdog_update();
#endif
        cyw43_arch_poll();
        tud_task();
#if ENABLE_AUTO_PROFILE
#if ENABLE_SERIAL
        auto_serial_poll();
#endif
        auto_usb_task();
        ns2_ble_tick();
        auto_session_tick();
        if (auto_manager_bootrom_requested &&
            auto_manager.reply_complete) {
            sleep_ms(50);
            reset_usb_boot(0, 0);
        }
#endif
#if ENABLE_AUTO_PROFILE
        if (auto_usb_is_nintendo()) {
            ns2_usb_task();
        } else if (auto_usb_is_sony_ds5()) {
            audio_loop();
            interrupt_loop();
        }
#else
        audio_loop();
        interrupt_loop();
#endif
#if ENABLE_BATT_LED
        battery_led_tick();
#endif
    }
}
