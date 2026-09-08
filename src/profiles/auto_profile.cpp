#include "profiles/auto_profile.h"

#include <cstdio>

#include "pico/time.h"

namespace {

struct AutoProfileRuntime {
    BridgeProfileId active;
    bool locked;
    uint32_t ds5_reports;
    uint32_t ns2_reports;
    uint64_t last_switch_us;
};

AutoProfileRuntime runtime{};

const char *profile_name(BridgeProfileId profile) {
    switch (profile) {
        case BridgeProfileId::Ds5FastPath:
            return "ds5_fast_path";
        case BridgeProfileId::Ns2ProCompatibility:
            return "ns2pro_compat";
        default:
            return "unknown";
    }
}

bool lock_profile(BridgeProfileId profile, const char *reason) {
    if (runtime.locked) {
        return runtime.active == profile;
    }

    runtime.active = profile;
    runtime.locked = true;
    runtime.last_switch_us = time_us_64();
    printf("[AUTO] locked profile=%s reason=%s\n",
           profile_name(profile),
           reason ? reason : "unknown");
    return true;
}

} // namespace

void auto_profile_init() {
    runtime = {};
    runtime.active = BridgeProfileId::Ds5FastPath;
    runtime.last_switch_us = time_us_64();
    printf("[AUTO] experimental auto profile enabled; mode=detecting usb_identity=debug_only\n");
}

void auto_profile_reset_detection(const char *reason) {
    if (!runtime.locked) {
        return;
    }
    runtime.active = BridgeProfileId::Ds5FastPath;
    runtime.locked = false;
    runtime.last_switch_us = time_us_64();
    printf("[AUTO] reset to detecting reason=%s\n", reason ? reason : "unknown");
}

bool auto_profile_note_ds5_input(uint16_t len) {
    runtime.ds5_reports++;
    (void)len;
    return lock_profile(BridgeProfileId::Ds5FastPath, "classic_hid_input");
}

bool auto_profile_note_ns2_input(const Ns2InputSnapshot *snapshot) {
    runtime.ns2_reports++;
    (void)snapshot;
    return lock_profile(BridgeProfileId::Ns2ProCompatibility, "ble_notify_input");
}

bool auto_profile_locked() {
    return runtime.locked;
}

BridgeProfileId auto_profile_active() {
    return runtime.active;
}

const char *auto_profile_active_name() {
    return profile_name(runtime.active);
}

uint32_t auto_profile_ds5_reports() {
    return runtime.ds5_reports;
}

uint32_t auto_profile_ns2_reports() {
    return runtime.ns2_reports;
}
