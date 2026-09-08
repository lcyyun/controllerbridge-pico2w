#include "profiles/auto_usb.h"

#include <cstdio>

#include "pico/time.h"
#include "tusb.h"

namespace {

struct AutoUsbRuntime {
    BridgeUsbIdentity current;
    BridgeUsbIdentity requested;
    bool pending;
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
    printf("[AUTO USB] request identity=%s reason=%s\n",
           identity_name(identity),
           reason ? reason : "unknown");
}

void auto_usb_task() {
    if (!runtime.pending) {
        return;
    }

    tud_disconnect();
    sleep_ms(250);
    runtime.current = runtime.requested;
    runtime.last_switch_us = time_us_64();
    printf("[AUTO USB] re-enumerate identity=%s\n", identity_name(runtime.current));
    tud_connect();
    runtime.pending = false;
}
