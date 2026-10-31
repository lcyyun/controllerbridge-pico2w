#include "profiles/auto_usb.h"

#include <cstdio>

#include "pico/time.h"
#include "tusb.h"

namespace {

struct AutoUsbRuntime {
    BridgeUsbIdentity current;
    BridgeUsbIdentity requested;
    bool pending;
    bool disconnected;
    uint64_t requested_us;
    uint64_t last_switch_us;
};

AutoUsbRuntime runtime{};

const char *identity_name(BridgeUsbIdentity identity) {
    switch (identity) {
        case BridgeUsbIdentity::DebugOnly:
            return "debug_only";
        case BridgeUsbIdentity::SonyDs5:
            return "sony_ds5";
        case BridgeUsbIdentity::NintendoSwitchPro:
            return "nintendo_switch_pro";
        case BridgeUsbIdentity::SonyDsEdge:
            return "sony_dsedge";
        case BridgeUsbIdentity::XInput:
            return "xinput";
        default:
            return "unknown";
    }
}

} // namespace

void auto_usb_init() {
    runtime = {};
    runtime.current = BridgeUsbIdentity::DebugOnly;
    runtime.requested = BridgeUsbIdentity::DebugOnly;
    runtime.last_switch_us = time_us_64();
    printf("[AUTO USB] identity=%s\n", identity_name(runtime.current));
}

BridgeUsbIdentity auto_usb_identity() {
    return runtime.current;
}

const char *auto_usb_identity_name() {
    return identity_name(runtime.current);
}

void auto_usb_request_identity(BridgeUsbIdentity identity, const char *reason) {
    if (identity == runtime.current && !runtime.pending) {
        return;
    }
    if (identity == runtime.requested && runtime.pending) {
        return;
    }
    runtime.requested = identity;
    runtime.pending = true;
    runtime.requested_us = time_us_64();
    printf("[AUTO USB] request identity=%s reason=%s\n",
           identity_name(identity),
           reason ? reason : "unknown");
}

void auto_usb_task() {
    if (!runtime.pending) {
        return;
    }

    // Let the host finish reading the manager response before disconnecting.
    // Never sleep here: this is the same cooperative loop as BTstack.
    const uint64_t now = time_us_64();
    if (!runtime.disconnected) {
        if (now - runtime.requested_us < 100000u) return;
        tud_disconnect();
        runtime.disconnected = true;
        runtime.last_switch_us = now;
        return;
    }
    if (now - runtime.last_switch_us < 250000u) return;
    runtime.current = runtime.requested;
    runtime.last_switch_us = time_us_64();
    printf("[AUTO USB] re-enumerate identity=%s\n", identity_name(runtime.current));
    tud_connect();
    runtime.pending = false;
    runtime.disconnected = false;
}

bool auto_usb_switch_pending() { return runtime.pending; }
