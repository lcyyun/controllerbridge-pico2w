#include "bridge_runtime.h"
#include <cstdio>
#include <cstring>

namespace {
bl616_bridge_input_state_t physical{};
bl616_bridge_mapping_profiles_t mappings{};
uint8_t role = 1, preference = 0, sequence = 0;
uint32_t unique_reports = 0, epoch = 0, last_fresh_ms = 0, last_source_sequence = 0;
bool has_sequence = false, rate_supported = false, dirty = false;
uint8_t native[64]{};
size_t native_size = 0;
constexpr uint32_t stale_ms = 250;
constexpr size_t wire_size = BL616_BRIDGE_MAPPING_WIRE_SIZE + 4;

bl616_bridge_mapping_config_t *mapping_for(uint8_t source) {
    return source == 2 ? &mappings.ns2pro : &mappings.ds5;
}
bool note(bl616_bridge_input_state_t state, uint32_t source_sequence, bool supported, uint32_t now) {
    if (!bridge_runtime_accepts(static_cast<uint8_t>(state.source))) return false;
    if (physical.source != state.source) bridge_runtime_reset_input();
    const bool fresh = !has_sequence || source_sequence != last_source_sequence;
    rate_supported = supported;
    if (!supported || fresh) {
        physical = state;
        last_fresh_ms = now;
        if (supported) ++unique_reports;
    }
    last_source_sequence = source_sequence;
    has_sequence = supported;
    return !supported || fresh;
}
}

void bridge_runtime_defaults() {
    bl616_bridge_mapping_profiles_defaults(&mappings);
    role = 1; preference = 0; dirty = true;
}
void bridge_runtime_init() {
    bridge_runtime_defaults();
    uint8_t wire[wire_size]{};
    if (bridge_store_load(wire, sizeof(wire)) && wire[0] == 1 && wire[1] >= 1 && wire[1] <= 4 &&
        wire[2] <= 2 && bl616_bridge_mapping_profiles_deserialize(wire + 4, sizeof(wire) - 4, &mappings)) {
        role = wire[1]; preference = wire[2];
    }
    dirty = false;
    bridge_runtime_reset_input();
}
bool bridge_runtime_save() {
    uint8_t wire[wire_size] = {1, role, preference, 0};
    if (!bl616_bridge_mapping_profiles_serialize(&mappings, wire + 4, sizeof(wire) - 4)) return false;
    if (!bridge_store_save(wire, sizeof(wire))) return false;
    dirty = false; return true;
}
void bridge_runtime_reset_input() {
    bl616_bridge_input_state_reset(&physical);
    native_size = 0; has_sequence = false; rate_supported = false;
    ++epoch;
}
bool bridge_runtime_accepts(uint8_t source) { return preference == 0 || preference == source; }
uint8_t bridge_runtime_role() { return role; }
void bridge_runtime_set_role(uint8_t value) { if (value >= 1 && value <= 4) { role = value; dirty = true; } }
uint8_t bridge_runtime_preference() { return preference; }
void bridge_runtime_set_preference(uint8_t value) {
    if (value <= 2) { preference = value; dirty = true; bridge_runtime_reset_input(); }
}
bool bridge_runtime_live(uint32_t now) { return physical.valid && (uint32_t)(now - last_fresh_ms) < stale_ms; }
void bridge_runtime_ds5(const uint8_t *body, size_t len, uint32_t now) {
    if (!body || len < 63 || !bridge_runtime_accepts(1)) return;
    bl616_bridge_input_state_t state{};
    if (bl616_bridge_parse_ds5_usb_input(body, 63, &state) != 0) return;
    if (!note(state, state.sensor_timestamp, state.timestamp_valid, now)) return;
    native[0] = 1; memcpy(native + 1, body, 63); native_size = 64;
}
void bridge_runtime_ns2(const Ns2InputSnapshot *input, uint32_t now) {
    if (!input || !input->valid || !bridge_runtime_accepts(2)) return;
    bl616_ns2_ble_snapshot_t snapshot{};
    snapshot.valid = 1; snapshot.buttons = input->buttons;
    snapshot.lx = input->lx; snapshot.ly = input->ly; snapshot.rx = input->rx; snapshot.ry = input->ry;
    snapshot.motion_valid = input->motion_valid; memcpy(snapshot.motion, input->motion, 12);
    bl616_bridge_input_state_t state{};
    if (bl616_bridge_state_from_ns2_snapshot(&snapshot, now * 1000u, &state) != 0) return;
    state.timestamp_valid = input->source_sequence_valid;
    state.battery_valid = input->battery_percent >= 0 && input->battery_percent <= 100;
    state.battery_percent = state.battery_valid ? input->battery_percent : BL616_BRIDGE_BATTERY_UNKNOWN;
    if (!note(state, input->source_sequence, input->source_sequence_valid, now)) return;
    native_size = 0;
    if (input->kind == Ns2InputReportKind::Fd2 && input->raw_valid && input->raw_len == 63) {
        native[0]=5; memcpy(native+1,input->raw,63); native_size=64;
    }
}
void bridge_runtime_state(bl616_bridge_input_state_t *out, uint32_t now, bool mapped) {
    if (!out) return;
    if (!bridge_runtime_live(now)) { bl616_bridge_input_state_reset(out); return; }
    *out = physical;
    if (mapped) bl616_bridge_mapping_apply(mapping_for(physical.source), &physical, out);
}
size_t bridge_runtime_report(bl616_bridge_role_t target, uint8_t *out, size_t capacity, uint32_t now, bool allow_native) {
    const size_t required=bl616_bridge_input_report_size(target);
    if (!out || !required || capacity < required) return 0;
    bl616_bridge_input_state_t state{};
    bridge_runtime_state(&state, now, true);
    auto *mapping = mapping_for(physical.source);
    if (state.valid && native_size == 64 &&
        ((physical.source == BL616_BRIDGE_INPUT_SOURCE_DUALSENSE_BT &&
          (target == BL616_BRIDGE_ROLE_DUALSENSE || target == BL616_BRIDGE_ROLE_DUALSENSE_EDGE)) ||
         (allow_native && physical.source == BL616_BRIDGE_INPUT_SOURCE_NS2PRO_BLE && target == BL616_BRIDGE_ROLE_NS2PRO))) {
        memcpy(out, native, 64);
        if (!bl616_bridge_mapping_patch_native_report(mapping, target, &physical, out, 64)) return 0;
        return 64;
    }
    // BL616 default NS2 Minus -> touchpad, Capture -> Create, with explicit bindings respected.
    if (state.source == BL616_BRIDGE_INPUT_SOURCE_NS2PRO_BLE &&
        (target == BL616_BRIDGE_ROLE_DUALSENSE || target == BL616_BRIDGE_ROLE_DUALSENSE_EDGE) &&
        mapping->source_for_target[BL616_BRIDGE_BUTTON_BACK] == BL616_BRIDGE_BUTTON_BACK &&
        mapping->source_for_target[BL616_BRIDGE_BUTTON_TOUCHPAD] == BL616_BRIDGE_BUTTON_TOUCHPAD) {
        if (state.buttons & BL616_BRIDGE_BUTTON_MASK(BL616_BRIDGE_BUTTON_BACK))
            state.buttons |= BL616_BRIDGE_BUTTON_MASK(BL616_BRIDGE_BUTTON_TOUCHPAD);
        state.buttons &= ~BL616_BRIDGE_BUTTON_MASK(BL616_BRIDGE_BUTTON_BACK);
    }
    if (bl616_bridge_encode_input(target, &state, sequence++, out, capacity) != 0) return 0;
    return bl616_bridge_input_report_size(target);
}
void bridge_runtime_status(char *out, size_t size, uint32_t now) {
    bl616_bridge_input_state_t state{};
    bridge_runtime_state(&state, now, false);
    const bool live = state.valid;
    snprintf(out, size,
        "{\"ok\":true,\"profile\":\"pico-unified\",\"input_snapshot_schema\":1,\"active_input\":\"%s\",\"input_valid\":%s,\"input_stale\":%s,"
        "\"unique_input_reports\":%lu,\"input_rate_time_ms\":%lu,\"input_rate_supported\":%s,\"input_epoch\":%lu,"
        "\"input_battery_percent\":%d,\"buttons\":%lu,\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,"
        "\"lt\":%u,\"rt\":%u,\"ax\":%d,\"ay\":%d,\"az\":%d,\"gx\":%d,\"gy\":%d,\"gz\":%d,"
        "\"motion_valid\":%s,\"axis_format\":\"signed16\",\"button_format\":\"canonical\"}",
        physical.source == 1 ? "ds5" : physical.source == 2 ? "ns2pro" : "none",
        live ? "true" : "false", physical.valid && !live ? "true" : "false",
        (unsigned long)unique_reports, (unsigned long)now, rate_supported ? "true" : "false", (unsigned long)epoch,
        live && state.battery_valid ? state.battery_percent : -1, (unsigned long)state.buttons,
        state.left_x, state.left_y, state.right_x, state.right_y, state.left_trigger, state.right_trigger,
        state.accel[0], state.accel[1], state.accel[2], state.gyro[0], state.gyro[1], state.gyro[2],
        state.motion_valid ? "true" : "false");
}
bool bridge_runtime_mapping_command(const char *line, char *out, size_t size) {
    bl616_bridge_mapping_command_t cmd{};
    const int parsed = bl616_bridge_mapping_parse_command(line, &cmd);
    if (parsed == 0) return false;
    if (parsed < 0) { snprintf(out,size,"{\"ok\":false,\"error\":\"invalid_mapping_command\"}"); return true; }
    if (cmd.profile == BL616_BRIDGE_MAPPING_PROFILE_UNSPECIFIED)
        cmd.profile = physical.source == 2 ? BL616_BRIDGE_MAPPING_PROFILE_NS2PRO : BL616_BRIDGE_MAPPING_PROFILE_DS5;
    auto *mapping = cmd.profile == BL616_BRIDGE_MAPPING_PROFILE_NS2PRO ? &mappings.ns2pro : &mappings.ds5;
    bool ok = true;
    if (cmd.kind == BL616_BRIDGE_MAPPING_COMMAND_SET) { ok = bl616_bridge_mapping_set(mapping, cmd.target, cmd.source); dirty |= ok; }
    if (cmd.kind == BL616_BRIDGE_MAPPING_COMMAND_RESET) { bl616_bridge_mapping_defaults(mapping); dirty = true; }
    if (cmd.kind == BL616_BRIDGE_MAPPING_COMMAND_SAVE) ok = bridge_runtime_save();
    if (!ok || bl616_bridge_mapping_format_profile_json(cmd.profile, mapping, dirty, out, size) < 0)
        snprintf(out,size,"{\"ok\":false,\"error\":\"mapping_operation_failed\"}");
    return true;
}
