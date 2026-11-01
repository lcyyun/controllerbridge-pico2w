#include "bridge_connection_control.h"
#include "bridge_runtime.h"
#include "bt.h"
#include "ns2/ns2_ble.h"
#include "profiles/auto_profile.h"

void bridge_connection_apply(bool user_requested) {
    const uint8_t selected = bridge_runtime_effective_preference();
    if (selected == 2) bt_set_classic_enabled(false);
    else if (user_requested) bt_resume_auto();
    else bt_set_classic_enabled(true);
    if (selected == 1) ns2_ble_suspend_auto();
    else ns2_ble_resume_auto(user_requested);
}

void bridge_connection_reset(const char *reason) {
    // Expiring only the bridge target leaves the radios in PairingScan/active
    // paging. Cancel their temporary intent before restoring saved policy.
    bt_cancel_manual_request();
    ns2_ble_cancel_manual_request();
    bridge_runtime_clear_connection();
    auto_profile_reset_detection(reason);
    bridge_runtime_reset_input();
    bridge_connection_apply();
}

bool bridge_connection_source_selected(uint8_t source) {
    if (bridge_runtime_connection_target() == source) return true;
    if (!auto_profile_locked()) return false;
    return source == (auto_profile_active() == BridgeProfileId::Ds5FastPath ? 1 : 2);
}
