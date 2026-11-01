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
#include "hardware/sync.h"
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
#include "bridge_runtime.h"
#include "bridge_connection_control.h"
#include "xinput_usb.h"
#include "bridge_features.h"
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
static bool auto_manager_reboot_requested = false;
static bool auto_manager_command_pending = false;
static bool output_processing = false, output_pending = false;
static uint8_t output_itf, output_id, output_type, output_size, output_bytes[80];
static uint32_t feedback_stop_ms = 0;

constexpr uint8_t AUTO_MANAGER_FEATURE_REPORT_ID = 0x7f;
constexpr size_t AUTO_MANAGER_FEATURE_REPORT_SIZE = 64;
constexpr size_t AUTO_MANAGER_REPLY_MAX = 4096;
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

static uint32_t ds5_usb_sent = 0, ds5_usb_failed = 0;

static bool auto_usb_is_nintendo() {
    return auto_usb_identity() == BridgeUsbIdentity::NintendoSwitchPro;
}

static bool auto_usb_is_sony_ds5() {
    return auto_usb_identity() == BridgeUsbIdentity::SonyDs5 || auto_usb_identity() == BridgeUsbIdentity::SonyDsEdge;
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
    printf("[AUTO] session ended; return to detect reason=%s\n", reason ? reason : "unknown");
    profile_lock_applied = false;
    bridge_connection_reset(reason);
    ns2_usb_set_motors(0, 0);
    output_pending = false; feedback_stop_ms = 0;
    state_init();
    audio_reset_session(false);
}

static void begin_manual_connection(uint8_t source, const char *reason) {
    reset_auto_session(reason);
    bridge_runtime_begin_connection(source, to_ms_since_boot(get_absolute_time()));
    bridge_connection_apply();
}

static void apply_profile_lock_actions() {
    if (profile_lock_applied || !auto_profile_locked()) {
        return;
    }
    profile_lock_applied = true;

    if (auto_profile_active() == BridgeProfileId::Ds5FastPath) {
        printf("[AUTO] apply DS5 fast path; suspend NS2 BLE\n");
        ns2_ble_suspend_auto();

        return;
    }

    printf("[AUTO] apply NS2 compatibility path; disable BT Classic\n");
    bt_set_classic_enabled(false);

}

static void auto_session_tick() {
    if (profile_lock_applied && !active_profile_connected()) {
        reset_auto_session(auto_profile_active() == BridgeProfileId::Ds5FastPath ?
                           "ds5_disconnected" :
                           "ns2_disconnected");
    } else if (bridge_runtime_connection_tick(to_ms_since_boot(get_absolute_time()), auto_profile_locked())) {
        reset_auto_session("connection_attempt_timeout");
    }
}

static void on_ns2_input_auto(const Ns2InputSnapshot *snapshot) {
    if (!bridge_runtime_accepts(2) || !auto_profile_note_ns2_input(snapshot)) {
        return;
    }
    bridge_runtime_ns2(snapshot, to_ms_since_boot(get_absolute_time()));
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
        auto_usb_request_identity(auto_usb_identity(), "manager_reconnect");
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
    bridge_runtime_status(out, out_len, to_ms_since_boot(get_absolute_time()));
    const size_t len = strlen(out);
    const char *role = auto_usb_identity() == BridgeUsbIdentity::SonyDs5 ? "ds5" :
        auto_usb_identity() == BridgeUsbIdentity::SonyDsEdge ? "dse" :
        auto_usb_identity() == BridgeUsbIdentity::XInput ? "xbox" :
        auto_usb_identity() == BridgeUsbIdentity::NintendoSwitchPro ? "ns2pro" : "unknown";
    AudioMicStats mic{}; audio_mic_get_stats(&mic);
    AudioStats audio{}; audio_get_stats(&audio);
    if (len && out[len - 1] == '}') {
        snprintf(out + len - 1, out_len - len + 1,
                 ",\"firmware\":\"%s\",\"role\":\"%s\",\"usb_role\":\"%s\",\"input_preference\":\"%s\","
                 "\"connection_target\":\"%s\",\"ns2_state\":\"%s\",\"ns2_auto_enabled\":%s,\"ns2_connect_attempts\":%lu,\"ns2_last_disconnect_reason\":%u,"
                 "\"ds5_connected\":%s,\"ds5_saved\":%s,\"ds5_pairing\":%s,\"ns2_connected\":%s,\"ns2_saved\":%s,\"ns2_scanning\":%s,"
                 "\"usb_mounted\":%s,\"mounted\":%s,\"suspended\":%s,\"report_rate_hz\":%u,\"sys_clock_khz\":%lu,"
                 "\"rumble_enabled\":%s,\"rumble_scale_percent\":%u,\"rumble_hold_ms\":%u,\"rumble_tick_ms\":%u,\"rumble_stop_packets\":%u,"
                 "\"usb_raw_passthrough\":%s,\"web_parse_reports\":%s,"
                 "\"mic_enabled\":%s,\"mic_received\":%lu,\"mic_dropped\":%lu,\"mic_decode_errors\":%lu,"
                 "\"audio_capable\":%s,\"audio_speaker_open\":%s,\"audio_mic_open\":%s,\"audio_ns2_active\":%s,"
                 "\"audio_out_packets\":%lu,\"audio_out_bytes\":%lu,\"audio_out_errors\":%lu,"
                 "\"audio_in_packets\":%lu,\"audio_in_bytes\":%lu,\"audio_in_errors\":%lu,"
                 "\"audio_bt_reports\":%lu,\"audio_bt_dropped\":%lu,\"audio_opus_errors\":%lu,"
                 "\"audio_haptic_blocks\":%lu,\"audio_ns2_updates\":%lu,\"audio_ns2_stops\":%lu,\"audio_ns2_mixed_ticks\":%lu}",
                 PICO_PROGRAM_VERSION_STRING, role, role,
                 bridge_runtime_preference() == 1 ? "ds5" : bridge_runtime_preference() == 2 ? "ns2pro" : "auto",
                 bridge_runtime_connection_target() == 1 ? "ds5" : bridge_runtime_connection_target() == 2 ? "ns2pro" : "none",
                 ns2_ble_state_name(ns2_status_get_state()), ns2_ble_auto_connect_enabled() ? "true" : "false",
                 (unsigned long)ns2_status_connect_attempts(), ns2_status_last_disconnect_reason(),
                 bt_classic_connected() ? "true" : "false", bt_has_saved_peer() ? "true" : "false", bt_pairing_active() ? "true" : "false",
                 ns2_ble_connected() ? "true" : "false", ns2_config_has_saved_target() ? "true" : "false",
                 ns2_status_get_state() == Ns2BleState::Scanning ? "true" : "false", tud_mounted() ? "true" : "false",
                 tud_mounted() ? "true":"false", tud_suspended() ? "true":"false", ns2_config_report_rate_hz(), (unsigned long)SYS_CLOCK_KHZ,
                 ns2_config_rumble_enabled() ? "true":"false", ns2_config_rumble_scale_percent(), ns2_config_rumble_hold_ms(),
                 ns2_config_rumble_tick_ms(), ns2_config_rumble_stop_packets(), ns2_config_usb_raw_passthrough_enabled() ? "true":"false",
                 ns2_config_web_parse_reports_enabled() ? "true":"false", mic.enabled ? "true" : "false",
                 (unsigned long)mic.received,(unsigned long)mic.dropped,(unsigned long)mic.decode_errors,
                 auto_usb_is_sony_ds5() ? "true":"false", audio.speaker_open ? "true":"false",
                 audio.mic_open ? "true":"false", audio.ns2_active ? "true":"false",
                 (unsigned long)audio.out_packets,(unsigned long)audio.out_bytes,(unsigned long)audio.out_errors,
                 (unsigned long)audio.in_packets,(unsigned long)audio.in_bytes,(unsigned long)audio.in_errors,
                 (unsigned long)audio.bt_reports,(unsigned long)audio.bt_dropped,(unsigned long)audio.opus_errors,
                 (unsigned long)audio.haptic_blocks,(unsigned long)audio.ns2_updates,(unsigned long)audio.ns2_stops,
                 (unsigned long)audio.ns2_mixed_ticks);
    }
}

static void print_auto_status() {
    static char json[AUTO_MANAGER_REPLY_MAX];
    format_auto_status_json(json, sizeof(json));
    printf("%s\n", json);
}

static void format_auto_usb_status_json(char *out, size_t out_len) { format_auto_status_json(out,out_len); }

static void print_auto_usb_status() {
    static char json[AUTO_MANAGER_REPLY_MAX];
    format_auto_usb_status_json(json, sizeof(json));
    printf("%s\n", json);
}

static void format_auto_live_status_json(char *out, size_t size) { format_auto_status_json(out, size); }
static void format_auto_motion_status_json(char *out, size_t size) { format_auto_status_json(out, size); }
static void print_auto_help() { printf("status | mapping get/set/reset/save ds5|ns2pro | role set ds5|ds5-edge|xinput|ns2pro | input set auto|ds5|ns2pro | settings save/reset | bootsel\n"); }
static void bridge_feedback(const bl616_bridge_feedback_t &feedback);

static bool handle_auto_command_json(const char *raw_line, char *out, size_t out_len) {
    const char *line = skip_spaces(raw_line);
    if (!out || !out_len) return false;
    if (bridge_runtime_mapping_command(line, out, out_len)) return true;
    if (strncmp(line, "role set ", 9) == 0) {
        const char *name = line + 9;
        uint8_t selected = strcmp(name, "ds5") == 0 || strcmp(name, "dualsense") == 0 ? 1 :
            strcmp(name, "ns2pro") == 0 ? 2 :
            strcmp(name, "ds5-edge") == 0 || strcmp(name, "dse") == 0 || strcmp(name, "dualsense-edge") == 0 ? 3 :
            strcmp(name, "xinput") == 0 || strcmp(name, "xbox") == 0 || strcmp(name, "xbox360") == 0 ? 4 : 0;
        if (!selected) { snprintf(out,out_len,"{\"ok\":false,\"error\":\"invalid_role\"}"); return true; }
        bl616_bridge_feedback_t stop{}; stop.valid=1; bridge_feedback(stop);
        output_pending=false; feedback_stop_ms=0;
        bridge_runtime_set_role(selected);
        spk_active = false; audio_reset_session(); state_init();
        auto_usb_request_identity(static_cast<BridgeUsbIdentity>(selected), "manager");
        snprintf(out,out_len,"{\"ok\":true,\"reboot_required\":false,\"reenumerating\":true}"); return true;
    }
    if (strncmp(line, "input set ", 10) == 0) {
        const char *name = line + 10;
        uint8_t selected = strcmp(name,"auto") == 0 ? 0 : strcmp(name,"ds5") == 0 ? 1 : strcmp(name,"ns2pro") == 0 ? 2 : 255;
        if (selected == 255) { snprintf(out,out_len,"{\"ok\":false,\"error\":\"invalid_source\"}"); return true; }
        bridge_runtime_set_preference(selected);
        reset_auto_session("source_change");
        bridge_connection_apply(true);
        format_auto_status_json(out,out_len); return true;
    }
    if (strcmp(line,"settings save") == 0) {
        const bool bridge_ok = bridge_runtime_save();
        const bool ds5_ok = config_save();
        const bool ns2_ok = ns2_config_save();
        snprintf(out,out_len,"{\"ok\":%s,\"saved\":%s}",bridge_ok && ds5_ok && ns2_ok ? "true":"false",bridge_ok && ds5_ok && ns2_ok ? "true":"false"); return true;
    }
    if (strcmp(line,"settings reset") == 0) {
        const Ns2ConfigBody saved_ns2=ns2_config_get();
        const bool had_target=ns2_config_has_saved_target();
        bridge_runtime_defaults(); config_default(); ns2_config_reset_defaults();
        if (had_target) ns2_config_set_saved_target(saved_ns2.saved_addr,saved_ns2.saved_addr_type,saved_ns2.last_success_boot_counter);
        ns2_usb_init(); reset_auto_session("settings_reset");
        bridge_runtime_reset_input();
        bridge_connection_apply(true);
        auto_usb_request_identity(BridgeUsbIdentity::SonyDs5,"settings_reset");
        const bool ok = bridge_runtime_save() && config_save() && ns2_config_save();
        snprintf(out,out_len,"{\"ok\":%s}",ok ? "true":"false"); return true;
    }
    if (strcmp(line,"reboot") == 0) {
        auto_manager_reboot_requested = true;
        snprintf(out,out_len,"{\"ok\":true,\"reboot\":true}"); return true;
    }
    if (strcmp(line,"ds5 pair") == 0 || strcmp(line,"ds5 reconnect") == 0) {
        begin_manual_connection(1, "ds5_connect");
        if (strcmp(line,"ds5 pair") == 0) bt_pair(); else bt_reconnect();
        format_auto_status_json(out,out_len); return true;
    }
    if (strcmp(line,"ds5 disconnect") == 0 || strcmp(line,"ds5 forget") == 0) {
        const bool selected = bridge_connection_source_selected(1);
        if (strcmp(line,"ds5 forget") == 0) bt_forget(); else bt_disconnect();
        if (selected) reset_auto_session("ds5_manual_disconnect");
        format_auto_status_json(out,out_len); return true;
    }
    if ((strcmp(line,"rumble test left") == 0 || strcmp(line,"rumble test right") == 0 ||
         strcmp(line,"rumble test both") == 0 || strcmp(line,"rumble stop") == 0) &&
        auto_profile_locked() && auto_profile_active() == BridgeProfileId::Ds5FastPath) {
        bl616_bridge_feedback_t feedback{}; feedback.valid = 1;
        feedback.left_motor = strcmp(line,"rumble test right") && strcmp(line,"rumble stop") ? 32768 : 0;
        feedback.right_motor = strcmp(line,"rumble test left") && strcmp(line,"rumble stop") ? 32768 : 0;
        bridge_feedback(feedback); feedback_stop_ms=to_ms_since_boot(get_absolute_time())+250; snprintf(out,out_len,"{\"ok\":true}"); return true;
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
        begin_manual_connection(2, "ns2_scan");
        ns2_ble_start_scan();
        format_auto_status_json(out, out_len);
        return true;
    }
    if (strcmp(line, "ns2 status") == 0) {
        ns2_status_format_json(out, out_len);
        return true;
    }
    if (strcmp(line, "ns2 pair") == 0) {
        begin_manual_connection(2, "ns2_pair");
        ns2_ble_pair();
        format_auto_status_json(out, out_len);
        return true;
    }
    if (strcmp(line, "ns2 reconnect") == 0) {
        begin_manual_connection(2, "ns2_reconnect");
        ns2_ble_reconnect();
        format_auto_status_json(out, out_len);
        return true;
    }
    if (strcmp(line, "ns2 disconnect") == 0) {
        const bool selected = bridge_connection_source_selected(2);
        ns2_ble_disconnect();
        if (selected) reset_auto_session("ns2_manual_disconnect");
        format_auto_status_json(out, out_len);
        return true;
    }
    if (strcmp(line, "ns2 forget") == 0) {
        const bool selected = bridge_connection_source_selected(2);
        ns2_ble_forget();
        if (selected) reset_auto_session("ns2_forget");
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
    if (strcmp(line, "bootrom") == 0 || strcmp(line, "uf2") == 0 || strcmp(line,"bootsel") == 0) {
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

    static char json[AUTO_MANAGER_REPLY_MAX];
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

    auto_manager_command_pending = true;
    auto_manager.reply_len = 0; auto_manager.reply_complete = false;
}

static uint16_t auto_manager_build_feature_report(uint8_t *buffer, uint16_t reqlen) {
    if (!buffer || reqlen == 0) {
        return 0;
    }

    auto_manager.get_count++;
    if (auto_manager_command_pending) return 0;
    if (auto_manager.reply_len == 0) {
        static char json[AUTO_MANAGER_REPLY_MAX];
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
            if (!auto_manager_bootrom_requested && !auto_manager_reboot_requested) {
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
#if !ENABLE_AUTO_PROFILE
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
            ++ds5_usb_sent;
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
            ++ds5_usb_sent;
#endif
        }
    }
#endif
}

void on_bt_data(CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
    if (channel == INTERRUPT && audio_bt_input(data,len)) return;
    if (channel == INTERRUPT && data && len >= 66 && data[1] == 0x31) {
#if ENABLE_AUTO_PROFILE
        if (!bridge_runtime_accepts(1) || !auto_profile_note_ds5_input(len)) {
            return;
        }
        bridge_runtime_ds5(data + 3, 63, to_ms_since_boot(get_absolute_time()));
#endif
        set_headset(data[56] & 1);

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

#if ENABLE_AUTO_PROFILE
    if (!bt_classic_connected()) return bridge_features_get_fallback(report_id,buffer,reqlen);
#endif
    std::vector<uint8_t> feature_data = get_feature_data(report_id, reqlen);
    const size_t length = feature_data.empty() ? 0 : (feature_data.size()-1 < reqlen ? feature_data.size()-1 : reqlen);
    if (length) memcpy(buffer,feature_data.data()+1,length);
    return length;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void) rhport;
    uint8_t const itf = tu_u16_low(p_request->wIndex); // wInterface
    uint8_t const alt = tu_u16_low(p_request->wValue); // bAlternateSetting

    if (itf == 1) {
        printf("[AUDIO] Set interface Speaker to alternate setting %d\n", alt);
        spk_active = alt;
        audio_speaker_set_open(alt != 0);
    }
    if (itf == 2) audio_mic_set_open(alt != 0);

    return true;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
#if ENABLE_AUTO_PROFILE
    // Nintendo's manager transport prefers output report 0x02, with the
    // same Y7HID1 body as Feature 0x7f. Consume it before rumble routing.
    if (auto_usb_is_nintendo() && report_type != HID_REPORT_TYPE_FEATURE && buffer) {
        const uint8_t *payload=buffer; uint16_t length=bufsize;
        uint8_t id=report_id;
        if (!id && length) { id=*payload++; --length; }
        if (id==2 && length>=6 && memcmp(payload,AUTO_MANAGER_SET_MAGIC,6)==0) {
            auto_manager_receive_feature_command(payload,length); return;
        }
    }
    if (!output_processing && report_type != HID_REPORT_TYPE_FEATURE) {
        if (!buffer || bufsize > sizeof(output_bytes)) return;
        output_itf=itf; output_id=report_id; output_type=report_type; output_size=bufsize;
        memcpy(output_bytes,buffer,bufsize); output_pending=true; return;
    }
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
    if (report_type != HID_REPORT_TYPE_FEATURE && auto_profile_locked()) {
        uint8_t report[80]{};
        size_t len = bufsize < sizeof(report) - 1 ? bufsize : sizeof(report) - 1;
        if (report_id) { report[0] = report_id; memcpy(report + 1,buffer,len); ++len; }
        else { memcpy(report,buffer,len); }
        bl616_bridge_feedback_t feedback{};
        if (auto_usb_is_nintendo() && auto_profile_active() == BridgeProfileId::Ds5FastPath) {
            if (bl616_bridge_decode_ns2pro_output(report,len,&feedback) == 0) bridge_feedback(feedback);
            return;
        }
        if (auto_usb_is_sony_ds5() && auto_profile_active() == BridgeProfileId::Ns2ProCompatibility) {
            if (bl616_bridge_decode_ds5_output(report,len,&feedback) == 0) bridge_feedback(feedback);
            return;
        }
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

    if (report_id == 2 && bufsize <= 63) {
        uint8_t full[64]{}; full[0]=2; memcpy(full+1,buffer,bufsize);
        tud_hid_set_report_cb(itf,0,report_type,full,bufsize+1); return;
    }
    // INTERRUPT OUT
    if (!buffer || !bufsize) return;
    if (report_id == 0) {
        switch (buffer[0]) {
            case 0x02: {
                state_update(buffer + 1, (bufsize - 1 < sizeof(SetStateData) ? bufsize - 1 : sizeof(SetStateData)));
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
    ns2_usb_set_motors(0, 0);
    output_pending = false; feedback_stop_ms = 0;
    state_init();
    spk_active=false; audio_reset_session();
    if (auto_usb_is_nintendo()) {
        ns2_usb_umount_cb();
    }
}

void tud_suspend_cb(bool remote_wakeup_en) {
    ns2_usb_set_motors(0, 0);
    output_pending = false; feedback_stop_ms = 0;
    audio_reset_session(false);
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

#if ENABLE_AUTO_PROFILE
static void bridge_feedback(const bl616_bridge_feedback_t &feedback) {
    if (!feedback.valid || !auto_profile_locked()) return;
    if (auto_profile_active() == BridgeProfileId::Ns2ProCompatibility) {
        ns2_usb_set_motors(feedback.left_motor,feedback.right_motor);
    } else if (bt_classic_connected()) {
        uint8_t usb_report[48]{};
        bl616_bridge_encode_ds5_output(&feedback,usb_report);
        state_update(usb_report+1,47);
        uint8_t report[78]{}; report[0]=0x31; report[1]=(reportSeqCounter++ & 15)<<4; report[2]=0x10;
        state_set(report+3,sizeof(SetStateData)); bt_write(report,sizeof(report));
    }
}
static void bridge_output_tick() {
    if (auto_usb_switch_pending()) {
        output_pending=false;
        uint8_t discarded[80]; xinput_usb_take_output(discarded,sizeof(discarded));
        return;
    }
    if (output_pending) {
        output_pending=false; output_processing=true;
        tud_hid_set_report_cb(output_itf,output_id,static_cast<hid_report_type_t>(output_type),output_bytes,output_size);
        output_processing=false;
    }
    uint8_t report[80]{};
    size_t length = xinput_usb_take_output(report,sizeof(report));
    bl616_bridge_feedback_t feedback{};
    if (length && bl616_bridge_decode_xbox_output(report,length,&feedback) == 0) bridge_feedback(feedback);
    const uint32_t now_ms=to_ms_since_boot(get_absolute_time());
    if (feedback_stop_ms && static_cast<int32_t>(now_ms-feedback_stop_ms)>=0) {
        feedback_stop_ms=0; feedback={}; feedback.valid=1; bridge_feedback(feedback);
    }
    if (auto_usb_switch_pending() || auto_usb_is_nintendo() || auto_usb_is_debug_only()) return;
    static uint64_t next_report_us=0;
    const uint64_t now=time_us_64();
    if (now < next_report_us) return;
    next_report_us=now + 1000000u/ns2_config_report_rate_hz();
    auto target=auto_usb_identity()==BridgeUsbIdentity::XInput ? BL616_BRIDGE_ROLE_XBOX_360 :
        auto_usb_identity()==BridgeUsbIdentity::SonyDsEdge ? BL616_BRIDGE_ROLE_DUALSENSE_EDGE : BL616_BRIDGE_ROLE_DUALSENSE;
    length=bridge_runtime_report(target,report,sizeof(report),now_ms);
    if (!length) return;
    if (target==BL616_BRIDGE_ROLE_XBOX_360) { if (xinput_usb_ready()) xinput_usb_send(report,length); }
    else if (tud_hid_ready()) { if(tud_hid_report(1,report+1,63)) ++ds5_usb_sent; else ++ds5_usb_failed; }
}
#endif

int main() {
    vreg_set_voltage(SYS_CLOCK_KHZ > 150000 ? VREG_VOLTAGE_1_20 : VREG_VOLTAGE_1_10);
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

    // Initialize the critical section for the report buffer
    critical_section_init(&report_cs);

    config_load();
#if ENABLE_AUTO_PROFILE
    bridge_runtime_init();
#endif

    audio_init();
    state_init();

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

    bt_init();
    bt_register_data_callback(on_bt_data);

#if ENABLE_AUTO_PROFILE
    auto_profile_init();
    ns2_status_init();
    ns2_config_load();
    ns2_input_register_callback(on_ns2_input_auto);
    ns2_ble_init_for_shared_hci();
    ns2_usb_init();
    bridge_connection_apply();
    auto_usb_request_identity(static_cast<BridgeUsbIdentity>(bridge_runtime_role()),"boot");
#endif


#if !ENABLE_SERIAL
    watchdog_enable(1000, true);
#endif

    while (1) {
#if !ENABLE_SERIAL
        watchdog_update();
#endif
        cyw43_arch_poll();
        bt_tick();
        tud_task();
#if ENABLE_AUTO_PROFILE
#if ENABLE_SERIAL
        auto_serial_poll();
#endif
        if (auto_manager_command_pending) {
            static char json[AUTO_MANAGER_REPLY_MAX];
            auto_manager_command_pending = false;
            handle_auto_command_json(auto_manager.last_command,json,sizeof(json));
            auto_manager_queue_json(json);
        }
        apply_profile_lock_actions();
        auto_usb_task();
        ns2_ble_tick();
        auto_session_tick();
        if ((auto_manager_bootrom_requested || auto_manager_reboot_requested) &&
            auto_manager.reply_complete) {
            sleep_ms(50);
            if (auto_manager_reboot_requested) watchdog_reboot(0,0,0); else reset_usb_boot(0, 0);
        }
#endif
#if ENABLE_AUTO_PROFILE
        audio_mic_service(auto_usb_is_sony_ds5() && !auto_usb_switch_pending() && auto_profile_locked() && auto_profile_active()==BridgeProfileId::Ds5FastPath && bt_classic_connected() && tud_mounted() && !tud_suspended());
        bridge_output_tick();
        if (auto_usb_is_nintendo()) ns2_usb_task(); else ns2_usb_feedback_task();
        const bool audio_output_eligible = auto_usb_is_sony_ds5() && spk_active &&
            !auto_usb_switch_pending() && tud_mounted() && !tud_suspended() && active_profile_connected();
        if (audio_output_eligible && auto_profile_active() == BridgeProfileId::Ds5FastPath)
            audio_loop();
        else if (audio_output_eligible && auto_profile_active() == BridgeProfileId::Ns2ProCompatibility)
            audio_loop_ns2_haptics();
        else if (tud_audio_available()) {
            const uint32_t irq_state = save_and_disable_interrupts();
            tud_audio_clear_ep_out_ff();
            restore_interrupts(irq_state);
        }
#else
        audio_mic_service(bt_classic_connected() && tud_mounted() && !tud_suspended());
        audio_loop();
        interrupt_loop();
#endif
#if ENABLE_BATT_LED
        battery_led_tick();
#endif
    }
}
