#include "bridge_connection_control.h"
#include "bridge_runtime.h"
#include "profiles/auto_profile.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static bool ds5_enabled, ns2_enabled, ds5_suppressed, ns2_suppressed;
static bool ds5_manual, ns2_manual;
static unsigned ds5_cancelled, ns2_cancelled;
static uint8_t store[84];
static bool saved;
uint64_t time_us_64() { return 0; }
bool bridge_store_load(uint8_t *p, size_t n) { if (!saved) return false; std::memcpy(p,store,n); return true; }
bool bridge_store_save(const uint8_t *p, size_t n) { std::memcpy(store,p,n); saved=true; return true; }
void bt_set_classic_enabled(bool on) { ds5_enabled=on; }
void bt_resume_auto() { ds5_enabled=true; ds5_suppressed=false; }
void bt_cancel_manual_request() { ds5_manual=false; ++ds5_cancelled; }
void ns2_ble_suspend_auto() { ns2_enabled=false; }
void ns2_ble_resume_auto(bool user_requested) { ns2_enabled=true; if (user_requested) ns2_suppressed=false; }
void ns2_ble_cancel_manual_request() { ns2_manual=false; ++ns2_cancelled; }

int main() {
    bridge_runtime_init(); auto_profile_init(); bridge_connection_apply();
    assert(ds5_enabled && ns2_enabled);
    for (uint8_t source : {uint8_t(1), uint8_t(2)}) {
        bridge_connection_reset("manual action");
        assert(bridge_runtime_begin_connection(source,100));
        bridge_connection_apply();
        ds5_manual=source==1; ns2_manual=source==2;
        assert(ds5_enabled==(source==1) && ns2_enabled==(source==2));
        assert(bridge_runtime_preference()==0);
        assert(bridge_connection_source_selected(source));
        assert(!bridge_connection_source_selected(source==1?2:1));
        // Same save operation used by both mapping and settings commands.
        assert(bridge_runtime_save());
        // A pending request never locked the input profile. Its timeout still
        // cancels BOTH transport-level modes before restoring Auto.
        const unsigned ds5_before=ds5_cancelled, ns2_before=ns2_cancelled;
        assert(bridge_runtime_connection_tick(60100,false));
        bridge_connection_reset("timeout");
        assert(!ds5_manual && !ns2_manual && ds5_cancelled>ds5_before && ns2_cancelled>ns2_before);
        assert(ds5_enabled && ns2_enabled);
        bridge_runtime_init();
        assert(bridge_runtime_preference()==0 && bridge_runtime_connection_target()==0);
    }
    bridge_runtime_set_preference(1);
    assert(bridge_runtime_begin_connection(2,0)); bridge_connection_apply();
    Ns2InputSnapshot input{}; input.valid=true;
    assert(auto_profile_note_ns2_input(&input));
    // Disconnect/forget of the other controller must not release this session,
    // even when the saved preference differs from the temporary target.
    assert(!bridge_connection_source_selected(1));
    assert(bridge_connection_source_selected(2) && auto_profile_locked());
    bridge_connection_reset("selected controller disconnected");
    assert(!auto_profile_locked() && ds5_enabled && !ns2_enabled);
    bridge_runtime_set_preference(0);
    ds5_suppressed=ns2_suppressed=true;
    bridge_connection_reset("implicit session end");
    assert(ds5_suppressed && ns2_suppressed);
    bridge_connection_apply(true); // Explicit Auto button is a new request.
    assert(ds5_enabled && ns2_enabled && !ds5_suppressed && !ns2_suppressed);
    puts("PASS: real connection-policy wiring, transport cancellation, Auto persistence, timeout, inactive-source isolation and explicit resume");
}
