// Compile the production state machine in this TU; only radio/flash/time are mocked.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include "../src/ns2/ns2_ble.cpp"

static uint64_t clock_ms = 100;
static bool saved = true, auto_connect = true;
static uint8_t peer[6] = {1,2,3,4,5,6};
static Ns2BleState state = Ns2BleState::Idle;
static int scans, stops, connects, cancels, disconnects, config_saves, db_writes;
static int connect_result = 0;
static HCI_STATE controller_state = HCI_STATE_WORKING;
static int security_requests, mtu_requests;
static bool config_save_ok = true, durable_saved = true;
static uint8_t last_connect_addr[6];
static uint32_t notify_age = UINT32_MAX;
static std::set<gatt_client_notification_t *> listeners;
static void (*crypto_callback)(void *);
static void *crypto_arg;
static uint8_t stored_key[16];
static uint8_t write_bytes[64];
static size_t write_len;
static uint16_t write_handle;
static bool have_db = false;
static unsigned input_callbacks;

static void count_input(const Ns2InputSnapshot *snapshot) {
    assert(snapshot && snapshot->valid);
    ++input_callbacks;
}

uint64_t time_us_64() { return clock_ms * 1000; }
bool ns2_config_auto_connect_enabled() { return auto_connect; }
void ns2_config_set_auto_connect(bool v) { auto_connect = v; }
bool ns2_config_has_saved_target() { return saved; }
void ns2_config_get_saved_target(uint8_t *a, uint8_t *t) { memcpy(a,peer,6); *t=0; }
void ns2_config_set_saved_target(const uint8_t *, uint8_t, uint32_t) { saved=true; }
void ns2_config_forget_saved_target() { saved=false; }
bool ns2_config_save() { ++config_saves;if(config_save_ok)durable_saved=saved;return config_save_ok; }
void ns2_config_load() {}
bool ns2_addr_equal(const uint8_t *a,const uint8_t *b) { return memcmp(a,b,6)==0; }
bool ns2_addr_is_empty(const uint8_t *a) { uint8_t b[6]{};return ns2_addr_equal(a,b); }
void ns2_format_addr(const uint8_t *,uint8_t,char *out,size_t size) { snprintf(out,size,"test-peer"); }
void ns2_status_set_state(Ns2BleState value) { state=value; }
Ns2BleState ns2_status_get_state() { return state; }
void ns2_status_set_last_error(const char *) {}
void ns2_status_clear_error() {}
void ns2_status_clear_candidates() {}
void ns2_status_note_connect_attempt() {}
void ns2_status_note_connected(const uint8_t *,uint8_t) {}
void ns2_status_note_disconnected(uint8_t) {}
void ns2_status_clear_connection() {}
void ns2_status_set_local_addr(const uint8_t *) {}
void ns2_status_set_rssi(int8_t) {}
void ns2_status_note_candidate(const uint8_t *,uint8_t,int8_t,const char *,bool,const char *) {}
void ns2_status_note_notify(uint16_t) { notify_age=0; }
uint32_t ns2_status_notify_count() { return 1; }
uint32_t ns2_status_last_notify_age_ms() { return notify_age; }
bool ns2_status_live_notify() { return notify_age < 1000; }

extern "C" {
void hci_dump_log(int,const char *,...) {}
HCI_STATE hci_get_state() { return controller_state; }
void hci_add_event_handler(btstack_packet_callback_registration_t *) {}
int hci_power_control(HCI_POWER_MODE) { return 0; }
void gap_start_scan() { ++scans; }
void gap_stop_scan() { ++stops; }
uint8_t gap_connect(const bd_addr_t address,bd_addr_type_t type) { assert(type==0);memcpy(last_connect_addr,address,6);++connects; return connect_result; }
uint8_t gap_connect_cancel() { ++cancels; return 0; }
uint8_t gap_disconnect(hci_con_handle_t) { ++disconnects; return 0; }
int gap_read_rssi(hci_con_handle_t) { return 0; }
void gap_local_bd_addr(bd_addr_t a) { for(unsigned i=0;i<6;i++) a[i]=i+1; }
void gap_delete_bonding(bd_addr_type_t,bd_addr_t) { have_db=false; }
void gap_set_scan_parameters(uint8_t,uint16_t,uint16_t) {}
void gap_set_connection_parameters(uint16_t,uint16_t,uint16_t,uint16_t,uint16_t,uint16_t,uint16_t,uint16_t) {}
int gap_update_connection_parameters(hci_con_handle_t,uint16_t,uint16_t,uint16_t,uint16_t) { return 0; }
void sm_request_pairing(hci_con_handle_t handle) { assert(handle==0x42);++security_requests; }
void sm_init() {}
void sm_set_io_capabilities(io_capability_t) {}
void sm_set_authentication_requirements(uint8_t) {}
void sm_add_event_handler(btstack_packet_callback_registration_t *) {}
void sm_just_works_confirm(hci_con_handle_t) {}
void sm_numeric_comparison_confirm(hci_con_handle_t) {}
void l2cap_init() {}
void l2cap_set_max_le_mtu(uint16_t) {}
void gatt_client_init() {}
void gatt_client_mtu_enable_auto_negotiation(uint8_t) {}
void gatt_client_send_mtu_negotiation(btstack_packet_handler_t,hci_con_handle_t) { ++mtu_requests; }
uint8_t gatt_client_discover_primary_services(btstack_packet_handler_t,hci_con_handle_t) { return 0; }
uint8_t gatt_client_discover_characteristics_for_service(btstack_packet_handler_t,hci_con_handle_t,gatt_client_service_t *) { return 0; }
// Decode the pinned BTstack event serialization at the mocked GATT boundary.
void gatt_client_deserialize_service(const uint8_t *p,int o,gatt_client_service_t *s) {
    s->start_group_handle=little_endian_read_16(p,o);s->end_group_handle=little_endian_read_16(p,o+2);
    reverse_128(p+o+4,s->uuid128);s->uuid16=0;
}
void gatt_client_deserialize_characteristic(const uint8_t *p,int o,gatt_client_characteristic_t *c) {
    c->start_handle=little_endian_read_16(p,o);c->value_handle=little_endian_read_16(p,o+2);
    c->end_handle=little_endian_read_16(p,o+4);c->properties=little_endian_read_16(p,o+6);
    reverse_128(p+o+8,c->uuid128);c->uuid16=0;
}
void gatt_client_listen_for_characteristic_value_updates(gatt_client_notification_t *n,btstack_packet_handler_t,hci_con_handle_t,gatt_client_characteristic_t *) { assert(listeners.insert(n).second); }
void gatt_client_stop_listening_for_characteristic_value_updates(gatt_client_notification_t *n) { assert(listeners.erase(n)==1); }
uint8_t gatt_client_write_value_of_characteristic_without_response(hci_con_handle_t,uint16_t handle,uint16_t size,uint8_t *data) { assert(size<=sizeof(write_bytes));memcpy(write_bytes,data,size);write_len=size;write_handle=handle;return 0; }
uint8_t gatt_client_write_value_of_characteristic(btstack_packet_handler_t,hci_con_handle_t h,uint16_t v,uint16_t n,uint8_t *d) { return gatt_client_write_value_of_characteristic_without_response(h,v,n,d); }
int le_device_db_count() { return have_db ? 1 : 0; }
int le_device_db_max_count() { return 1; }
void le_device_db_info(int,int *type,bd_addr_t a,sm_key_t irk) { *type=have_db?0:-1;if(have_db)memcpy(a,peer,6);else memset(a,0,6);memset(irk,0,16); }
int le_device_db_add(int,bd_addr_t,sm_key_t) { have_db=true;return 0; }
void le_device_db_encryption_set(int,uint16_t,uint8_t[8],sm_key_t ltk,int size,int,int,int secure) { assert(size==16&&secure);memcpy(stored_key,ltk,16);++db_writes; }
void le_device_db_encryption_get(int,uint16_t *,uint8_t[8],sm_key_t key,int *size,int *,int *,int *secure) { if(key)memcpy(key,stored_key,16);if(size)*size=16;if(secure)*secure=1; }
void btstack_crypto_random_generate(btstack_crypto_random_t *,uint8_t *b,uint16_t n,void(*cb)(void *),void *arg) { for(unsigned i=0;i<n;i++)b[i]=i;crypto_callback=cb;crypto_arg=arg; }
void btstack_crypto_aes128_encrypt(btstack_crypto_aes128_t *,const uint8_t *key,const uint8_t *plain,uint8_t *result,void(*cb)(void *),void *arg) { assert(key[0]==0x5a&&plain[0]==31);memset(result,0x77,16);crypto_callback=cb;crypto_arg=arg; }
}

static void tick_after(uint64_t ms) { clock_ms+=ms;ns2_ble_tick(); }
static void drop(uint8_t reason=0x13) {
    uint8_t event[]={HCI_EVENT_DISCONNECTION_COMPLETE,4,0,0x42,0,reason};
    hci_packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
}
static void reset() {
    reset_gatt_cache();
    ble={};ble.hci_ready=true;ble.con_handle=HCI_CON_HANDLE_INVALID;
    state=Ns2BleState::Idle;saved=true;auto_connect=true;notify_age=UINT32_MAX;
    scans=stops=connects=cancels=disconnects=config_saves=db_writes=0;
    connect_result=0;pair_crypto_pending=false;have_db=false;clock_ms=100;
    input_callbacks=0;ns2_input_reset();ns2_input_register_callback(count_input);
    controller_state=HCI_STATE_WORKING;security_requests=mtu_requests=0;
    config_save_ok=true;durable_saved=true;
    memset(stored_key,0,sizeof(stored_key));
}
// Event layouts are the pinned SDK's btstack_event.h layouts, including the
// normalized GAP connection event emitted before the raw HCI LE event.
static void advertise(bool pairing, bool directed=false, int8_t rssi=-90) {
    uint8_t event[43]={GAP_EVENT_ADVERTISING_REPORT,41,0,0};
    event[2]=directed?1:0;
    reverse_bd_addr(peer,event+4);event[10]=static_cast<uint8_t>(rssi);
    if (!directed) {
        const uint8_t data[]={2,1,6,27,0xff,0x53,0x05,
            1,0,3,0x7e,5,0x69,0x20,0,1,0, 6,5,4,3,2,1, 0x0f,0,0,0,0,0,0,0};
        memcpy(event+12,data,sizeof(data));event[11]=sizeof(data);
        if (pairing) memset(event+12+17,0,6);
    }
    hci_packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
}
static void connected_event(uint8_t status=0) {
    uint8_t event[36]={HCI_EVENT_META_GAP,34,GAP_SUBEVENT_LE_CONNECTION_COMPLETE,0,0x42,0,0,0};
    event[3]=status;
    reverse_bd_addr(peer,event+8);event[26]=6;event[30]=100;
    hci_packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
}
static void encrypted_event(uint8_t status=0) {
    uint8_t event[12]={SM_EVENT_REENCRYPTION_COMPLETE,10,0x42,0,0};
    reverse_bd_addr(peer,event+5);event[11]=status;
    sm_packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
}
static void gatt_complete() {
    uint8_t event[9]={GATT_EVENT_QUERY_COMPLETE,7,0x42,0,0,0,0,0,0};
    gatt_packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
}
static void notification(uint16_t handle,const uint8_t *data,uint16_t len) {
    uint8_t event[76]={GATT_EVENT_NOTIFICATION,0,0x42,0};
    event[1]=static_cast<uint8_t>(10+len);
    little_endian_store_16(event,8,handle);little_endian_store_16(event,10,len);
    memcpy(event+12,data,len);
    gatt_packet_handler(HCI_EVENT_PACKET,0,event,12+len);
}
static void finish_reconnect_gatt() {
    uint8_t mtu[]={GATT_EVENT_MTU,4,0x42,0,0,1};
    gatt_packet_handler(HCI_EVENT_PACKET,0,mtu,sizeof(mtu));
    uint8_t service[28]={GATT_EVENT_SERVICE_QUERY_RESULT,26,0x42,0};
    service[8]=1;service[10]=0x30;
    gatt_packet_handler(HCI_EVENT_PACKET,0,service,sizeof(service));gatt_complete();
    struct Characteristic {uint8_t handle,properties;const uint8_t *uuid;};
    const Characteristic fixtures[]={{0x0a,0x10,ADV_UUID_NOTIFY_FD2},{0x14,0x04,UUID_COMMAND},{0x1a,0x10,UUID_ACK}};
    for (const auto &fixture:fixtures) {
        uint8_t event[32]={GATT_EVENT_CHARACTERISTIC_QUERY_RESULT,30,0x42,0};
        event[8]=fixture.handle-1;event[10]=fixture.handle;event[12]=fixture.handle+1;
        event[14]=fixture.properties;reverse_128(fixture.uuid,event+16);
        gatt_packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
    }
    gatt_complete();assert(write_handle==0x1b&&write_len==2&&write_bytes[0]==1);
    gatt_complete(); // ACK CCC enabled before any command is sent.
    for (unsigned i=0;i<ns2_gatt_init_command_count();++i) {
        if (i==3) continue; // Normal wake must never run vendor key provisioning.
        tick_after(6);
        const auto &command=ns2_gatt_init_command(i);
        assert(write_handle==0x14&&write_len==command.len&&!memcmp(write_bytes,command.data,write_len));
        uint8_t ack[9]={command.data[0],1,1,command.data[3],0,1,0,0,1};
        notification(0x1a,ack,sizeof(ack));
    }
    tick_after(6);assert(write_handle==0x0b&&write_len==2&&write_bytes[0]==1);
    gatt_complete();assert(ble.gatt_stage==GattStage::Ready);
    uint8_t fd2[60]{};fd2[0]=123;fd2[11]=fd2[14]=8;fd2[12]=fd2[15]=128;
    notification(0x0a,fd2,sizeof(fd2));
    Ns2InputSnapshot snapshot{};
    assert(ns2_input_get_snapshot(&snapshot)&&snapshot.source_sequence==123&&input_callbacks==1);
    assert(state==Ns2BleState::ConnectedLive&&db_writes==0);
}
static void ready_notify_link() {
    ble.con_handle=0x42;ble.gatt_stage=GattStage::Ready;ble.ready_since_ms=clock_ms;
    state=Ns2BleState::ConnectedNoNotify;
    add_known_characteristic(Ns2GattRole::InputNotify,0x20,0x10,ADV_UUID_NOTIFY_FD2);
}
static void notify_input(uint8_t sequence) {
    uint8_t fd2[60]{};
    fd2[0]=sequence;fd2[11]=fd2[14]=8;fd2[12]=fd2[15]=128;
    handle_notification(0x20,fd2,sizeof(fd2));
}
static void pair_ack(uint8_t step) {
    uint8_t ack[25]={0x15,1,1,step,0,0,0,0,1};
    for(unsigned i=0;i<16;i++)ack[9+i]=step==4 ? (i ^ 0x5a) : 0x77;
    handle_pairing_ack(ack,sizeof(ack));
}

int main() {
    // Cold boot must wait for the shared HCI's working event and its startup
    // delay; a repeated event must not reset a running scan to Idle.
    reset();controller_state=HCI_STATE_INITIALIZING;ns2_ble_init_for_shared_hci();
    tick_after(1000);assert(scans==0&&!ble.hci_ready);
    controller_state=HCI_STATE_WORKING;
    uint8_t working[]={BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING};
    hci_packet_handler(HCI_EVENT_PACKET,0,working,sizeof(working));
    tick_after(599);assert(scans==0);tick_after(2);assert(scans==1);
    hci_packet_handler(HCI_EVENT_PACKET,0,working,sizeof(working));
    assert(ble.scanning&&state==Ns2BleState::Scanning);

    // Register after the shared controller is already working: INIT still must
    // use its real address, rather than the ff:ff factory placeholder.
    reset();
    uint8_t placeholder[6];memset(placeholder,0xff,sizeof(placeholder));
    ns2_gatt_set_console_mac(placeholder);
    ns2_ble_init_for_shared_hci();tick_after(601);
    const auto &init=ns2_gatt_init_command(0);
    assert(init.data[10]==6&&init.data[15]==1&&ble.hci_ready&&scans==1);

    // Ordinary saved any-key wake: non-pairable manufacturer data or directed
    // advertisements with no payload still select the exact saved peer.
    for (bool directed : {false,true}) {
        reset();have_db=true;memset(stored_key,0x5a,16);
        ns2_ble_init_for_shared_hci();tick_after(601);advertise(false,directed);
        assert(connects==1&&memcmp(last_connect_addr,peer,6)==0);
        connected_event();assert(security_requests==1&&ble.security_pending&&mtu_requests==0);
        // SDK also forwards the raw legacy event after its normalized event.
        uint8_t raw[21]={HCI_EVENT_LE_META,19,HCI_SUBEVENT_LE_CONNECTION_COMPLETE,0,0x42,0};
        hci_packet_handler(HCI_EVENT_PACKET,0,raw,sizeof(raw));
        assert(security_requests==1&&disconnects==0);
        encrypted_event();tick_after(1);
        assert(!ble.security_pending&&mtu_requests==1&&!ble.pairing_required);
        finish_reconnect_gatt();
        drop();tick_after(2001);assert(ble.scanning&&ble.connect_mode==ConnectMode::ScanAuto);
    }

    // The saved controller explicitly advertises an empty host address after
    // entering sync mode. Auto must provision it, not keep retrying an old LTK.
    reset();have_db=true;memset(stored_key,0x5a,16);
    ns2_ble_init_for_shared_hci();tick_after(601);advertise(true);connected_event();tick_after(1);
    assert(ble.pairing_required&&security_requests==0&&mtu_requests==1);

    reset();have_db=true;memset(stored_key,0x5a,16);
    ns2_ble_init_for_shared_hci();tick_after(601);advertise(false);connected_event();
    encrypted_event(ERROR_CODE_PIN_OR_KEY_MISSING);
    assert(disconnects==1&&saved&&have_db&&mtu_requests==0);
    drop();tick_after(2001);assert(ble.scanning&&ble.connect_mode==ConnectMode::ScanAuto);

    // Passive profile recovery preserves an explicit disconnect. Choosing an
    // input source in the UI opts in again, without overriding auto=off.
    reset();ns2_ble_disconnect();ns2_ble_resume_auto();tick_after(601);assert(scans==0);
    ns2_ble_resume_auto(true);tick_after(101);assert(scans==1&&!ble.manually_disconnected);
    reset();auto_connect=false;ns2_ble_disconnect();ns2_ble_resume_auto(true);tick_after(1001);
    assert(scans==0&&!auto_connect&&!ble.manually_disconnected);

    // Cancelling explicit pairing restores saved auto wake, without retaining
    // PairingScan or changing independent auto/suppression settings.
    reset();have_db=true;memset(stored_key,0x5a,16);ns2_ble_pair();
    assert(ble.connect_mode==ConnectMode::PairingScan&&ble.scanning);
    ns2_ble_cancel_manual_request();assert(!ble.scanning&&ble.requested_mode==ConnectMode::None);
    ns2_ble_resume_auto();tick_after(2001);advertise(false);connected_event();
    assert(ble.connect_mode==ConnectMode::ScanAuto&&security_requests==1&&db_writes==0);

    reset();ns2_ble_pair();advertise(true,false,-40);assert(ble.connecting);
    ns2_ble_cancel_manual_request();
    assert(cancels==1&&!ble.connecting&&!ble.pending_connect_valid&&saved);

    reset();ns2_ble_pair();advertise(true,false,-40);connected_event();
    ns2_ble_cancel_manual_request();assert(disconnects==1&&ble.disconnect_pending&&saved);
    drop();tick_after(2001);assert(ble.scanning&&ble.connect_mode==ConnectMode::ScanAuto);

    reset();auto_connect=false;ns2_ble_pair();ns2_ble_cancel_manual_request();
    ns2_ble_resume_auto();tick_after(30000);
    assert(scans==1&&!ble.scanning&&!auto_connect&&state==Ns2BleState::Idle);
    ns2_ble_disconnect();ns2_ble_cancel_manual_request();assert(ble.manually_disconnected);

    reset();auto_connect=false;ns2_ble_pair();advertise(true,false,-40);
    ns2_ble_cancel_manual_request();connected_event(ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
    tick_after(30000);assert(cancels==1&&scans==1&&!ble.scanning&&state==Ns2BleState::Idle);

    reset();ns2_ble_pair();ns2_ble_suspend_auto();ns2_ble_cancel_manual_request();
    assert(ble.suspended&&ble.requested_mode==ConnectMode::None);
    reset();ready_notify_link();notify_input(1);ble.requested_mode=ConnectMode::PairingScan;
    ns2_ble_cancel_manual_request();
    assert(disconnects==0&&state==Ns2BleState::ConnectedLive&&ns2_ble_connected());

    // A failed target flash save must not become "saved" just because its
    // address is already updated in RAM. Retry in the foreground after backoff.
    reset();saved=durable_saved=false;config_save_ok=false;
    ble.con_handle=0x42;ble.current_addr_valid=true;memcpy(ble.current_addr,peer,6);
    save_current_target_if_live("test persistence");
    assert(saved&&!durable_saved&&!ble.target_saved_this_connection&&config_saves==1);
    config_save_ok=true;tick_after(999);assert(config_saves==1);
    tick_after(2);assert(config_saves==2&&durable_saved&&ble.target_saved_this_connection);

    reset();start_auto_attempt();
    assert(scans==1&&ble.connect_mode==ConnectMode::ScanAuto);
    // Normal any-key wake advertises bound, not pairable. Exact saved address wins even at low RSSI.
    maybe_connect_from_advertisement(peer,0,-100,false,"bound");
    assert(connects==1&&ble.connecting);
    on_connection_failed(0x3e);tick_after(2001);
    assert(scans==2&&ble.connect_mode==ConnectMode::ScanAuto);
    connect_result=0x0c;
    maybe_connect_from_advertisement(peer,0,-40,true,"retry");
    assert(state==Ns2BleState::Backoff);tick_after(2001);assert(scans==3);
    ble.con_handle=0x42;ble.scanning=false;
    drop();assert(state==Ns2BleState::Backoff);tick_after(2001);assert(scans==4);
    ns2_ble_disconnect();ns2_ble_resume_auto();tick_after(20000);
    assert(scans==4&&ble.manually_disconnected);
    ns2_ble_reconnect();assert(scans==5&&!ble.manually_disconnected);
    ble.con_handle=0x42;ble.scanning=false;ns2_ble_pair();assert(disconnects>0);
    drop();tick_after(2001);assert(ble.connect_mode==ConnectMode::PairingScan);
    ns2_ble_suspend_auto();tick_after(3000);assert(ble.suspended&&!ble.scanning);
    ns2_ble_resume_auto();tick_after(101);assert(ble.scanning);
    ns2_ble_set_auto_connect(false);assert(!ble.scanning);tick_after(20000);assert(!ble.scanning);
    ns2_ble_set_auto_connect(true);tick_after(101);assert(ble.scanning);
    clear_saved_target_after_security_failure();assert(saved&&auto_connect); // No destructive timeout recovery.
    reset();have_db=true;ble.con_handle=0x42;ns2_ble_forget();
    assert(!saved&&have_db&&ble.manually_disconnected);
    drop();tick_after(1);assert(!have_db&&!ble.forget_bond_pending&&scans==0);
    reset();ble.con_handle=0x42;memcpy(ble.pending_addr,peer,6);ble.connect_mode=ConnectMode::ScanAuto;
    on_connected();assert(disconnects==1&&saved); // Address-only imports must re-pair, never invent a key.

    // Pending HCI disconnect still permits queued GATT callbacks. They must not
    // repopulate the reset input, invoke its consumer, or schedule persistence.
    for (unsigned action=0;action<3;++action) {
        reset();ready_notify_link();notify_input(1);
        Ns2InputSnapshot snapshot{};
        assert(ns2_input_get_snapshot(&snapshot)&&input_callbacks==1&&snapshot.source_sequence==1);
        if (action==0) ns2_ble_disconnect();
        else if (action==1) ns2_ble_forget();
        else ns2_ble_suspend_auto();
        assert(ble.disconnect_pending&&disconnects==1&&!ns2_input_get_snapshot(&snapshot));
        const auto waiting_state=state;
        notify_age=500;
        notify_input(2);tick_after(1);
        assert(!ns2_input_get_snapshot(&snapshot)&&input_callbacks==1);
        assert(state==waiting_state&&notify_age==500&&!ble.save_target_pending);
        drop();
        assert(state==Ns2BleState::Idle&&!ns2_input_get_snapshot(&snapshot));
    }

    reset();ready_notify_link();tick_after(5001);
    assert(disconnects==1); // First-notify watchdog, even without a last notification.
    assert(ble.disconnect_pending&&!ble.manually_disconnected);
    notify_input(1); // A timed-out link must not recover to ConnectedLive while disconnecting.
    assert(state==Ns2BleState::ConnectedNoNotify&&input_callbacks==0);
    assert(!ns2_input_get_snapshot(nullptr)&&!ble.save_target_pending&&notify_age==UINT32_MAX);
    tick_after(5001);assert(disconnects==1); // Never repeat disconnect while HCI completion is pending.
    reset();ble.con_handle=0x42;
    add_known_characteristic(Ns2GattRole::AckNotify,0x1a,0x10,UUID_ACK);
    ble.subscribe_index=-1;assert(subscribe_next(Ns2GattRole::AckNotify));
    assert(listeners.size()==1);reset_gatt_cache();assert(listeners.empty());

    reset();saved=durable_saved=false;ble.requested_mode=ConnectMode::PairingScan;
    ble.con_handle=0x42;ble.current_addr_valid=true;memcpy(ble.current_addr,peer,6);
    ble.command_value_handle=0x14;ble.command_write_without_response=true;
    ble.gatt_stage=GattStage::Initializing;ble.init_index=3;ble.pairing_required=true;
    send_current_init_command();assert(pair_crypto_pending);
    crypto_callback(crypto_arg);tick_after(2);assert(ble.pair_step==1&&ble.pair_waiting_ack);
    assert(write_bytes[10]==6&&write_bytes[15]==1); // LE console address.
    pair_ack(1);tick_after(6);assert(ble.pair_step==4);
    pair_ack(4);tick_after(6);assert(ble.pair_step==2);
    pair_ack(2);assert(pair_crypto_pending&&db_writes==0);
    crypto_callback(crypto_arg);tick_after(6);assert(ble.pair_step==3);
    pair_ack(3);assert(db_writes==0&&ble.pair_save_pending);
    tick_after(1);assert(db_writes==1&&!ble.pairing_required&&ble.init_index==4&&current_peer_has_ltk());
    assert(saved&&durable_saved&&config_saves==1&&ble.requested_mode==ConnectMode::None);
    drop();tick_after(2001);advertise(false);
    assert(connects==1&&ble.connect_mode==ConnectMode::ScanAuto);
    connected_event();assert(security_requests==1&&ble.security_pending);
    ble.pair_step=2;ble.pair_waiting_ack=true;ble.pair_deadline_ms=clock_ms+3500;
    memset(ble.pair_ltk,0x5a,16);for(unsigned i=0;i<32;i++)ble.pair_random[i]=i;
    uint8_t bad_ack[25]={0x15,1,1,2,0,0,0,0,1};
    handle_pairing_ack(bad_ack,sizeof(bad_ack));
    crypto_callback(crypto_arg);assert(disconnects==1); // Challenge mismatch never stores a key.
    assert(db_writes==1);
    // A crypto callback from a disconnected link cannot resurrect pairing state.
    ble.session=2;pair_crypto_session=1;ble.pair_step=0;pair_random_ready(nullptr);assert(ble.pair_step==0);
    puts("PASS: production BLE shared-HCI startup/bound and directed wake/encrypted GATT-to-input/saved sync advertising/explicit resume/persistence retry/disconnect window/vendor pairing state machine");
}
