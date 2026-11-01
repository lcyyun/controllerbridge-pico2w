// Real production Classic state machine + pinned BTstack headers/event layouts.
// Only time, persistence and radio transport/scheduling boundaries are mocked.
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#ifndef CLASSIC_SOURCE
#define CLASSIC_SOURCE "../src/bt.cpp"
#endif
#include CLASSIC_SOURCE

static uint64_t clock_ms=100;
static HCI_STATE mock_hci_state=HCI_STATE_WORKING;
static bool saved=true, iterator_read=false, command_credit=true, acl_credit=true;
static uint8_t peer[6]={1,2,3,4,5,6};
static int inquiries=0, drops=0, disconnects=0, security_requests=0, raw_commands=0;
static bool page_scan=false, discoverable=false;
static uint8_t create_status=0;
static uint16_t next_cid=0x40;
static int (*connection_filter)(bd_addr_t,hci_link_type_t)=nullptr;
static gap_security_level_t required_security=LEVEL_0;
static std::vector<uint16_t> created_psms, accepted_cids, declined_cids, detached_cids;
static std::set<uint16_t> can_send_requests;
static std::vector<std::vector<uint8_t>> sent_control;
static unsigned input_callbacks=0;
static unsigned send_attempts=0;
static Config_body config{1,1,0,5,1,1,0,16,2};
bool is_dse=false;
uint64_t time_us_64() { return clock_ms*1000; }
const Config_body &get_config() { return config; }
void state_set(uint8_t *p,uint8_t n) { memset(p,0,n); }

extern "C" {
void hci_dump_log(int,const char *,...) {}
HCI_STATE hci_get_state() { return mock_hci_state; }
void hci_add_event_handler(btstack_packet_callback_registration_t *) {}
int hci_power_control(HCI_POWER_MODE) { return 0; }
bool hci_can_send_command_packet_now() { return command_credit; }
uint8_t hci_send_cmd(const hci_cmd_t *,...) {
    ++raw_commands;
    // Exact pinned SDK contract: busy rejects synchronously, with NO later
    // HCI_COMMAND_STATUS event. Managed GAP/L2CAP calls enqueue instead.
    return command_credit ? ERROR_CODE_SUCCESS : ERROR_CODE_COMMAND_DISALLOWED;
}
uint16_t hci_usable_acl_packet_types() { return 0xcc18; }
void gap_ssp_set_enable(int) {}
void gap_secure_connections_enable(bool) {}
void gap_ssp_set_io_capability(int) {}
void gap_ssp_set_authentication_requirement(int) {}
void gap_ssp_set_auto_accept(int) {}
void gap_set_bondable_mode(int) {}
void gap_set_security_level(gap_security_level_t level) { required_security=level; }
void gap_register_classic_connection_filter(int(*filter)(bd_addr_t,hci_link_type_t)) { connection_filter=filter; }
void gap_connectable_control(uint8_t enabled) { page_scan=enabled; }
void gap_discoverable_control(uint8_t enabled) { discoverable=enabled; }
int gap_inquiry_start(uint8_t) { ++inquiries;return 0; }
int gap_inquiry_stop() { return 0; }
int gap_link_key_iterator_init(btstack_link_key_iterator_t *) { iterator_read=false;return 1; }
int gap_link_key_iterator_get_next(btstack_link_key_iterator_t *,bd_addr_t address,link_key_t key,link_key_type_t *type) {
    if(!saved||iterator_read)return 0;iterator_read=true;memcpy(address,peer,6);memset(key,0xa5,16);*type=UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P256;return 1;
}
void gap_link_key_iterator_done(btstack_link_key_iterator_t *) {}
void gap_drop_link_key_for_bd_addr(bd_addr_t) { saved=false;++drops; }
bool gap_get_link_key_for_bd_addr(bd_addr_t,link_key_t key,link_key_type_t *type) {
    memset(key,0xa5,16);*type=UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P256;return saved;
}
int gap_pin_code_response(const bd_addr_t,const char *) { return 0; }
void gap_request_security_level(hci_con_handle_t,gap_security_level_t level) { assert(level==LEVEL_2);++security_requests; }
uint8_t gap_disconnect(hci_con_handle_t) { ++disconnects;return 0; }
int gap_read_rssi(hci_con_handle_t) { return 0; }
void l2cap_init() {}
void l2cap_add_event_handler(btstack_packet_callback_registration_t *) {}
uint8_t l2cap_register_service(btstack_packet_handler_t,uint16_t,uint16_t,gap_security_level_t level) { assert(level==LEVEL_2);return 0; }
void sdp_init() {}
uint8_t l2cap_create_channel(btstack_packet_handler_t,bd_addr_t,uint16_t psm,uint16_t,uint16_t *cid) {
    created_psms.push_back(psm);
    if(create_status)return create_status;
    // BTstack owns the queued channel even before an HCI credit is available.
    *cid=next_cid++;return 0;
}
void l2cap_accept_connection(uint16_t cid) { accepted_cids.push_back(cid); }
void l2cap_decline_connection(uint16_t cid) { declined_cids.push_back(cid); }
uint8_t l2cap_disconnect(uint16_t cid) { detached_cids.push_back(cid);return 0; }
uint16_t l2cap_get_remote_mtu_for_local_cid(uint16_t) { return 672; }
uint8_t l2cap_request_can_send_now_event(uint16_t cid) { can_send_requests.insert(cid);return 0; }
uint8_t l2cap_send(uint16_t cid,const uint8_t *p,uint16_t n) {
    ++send_attempts;
    if(!acl_credit)return BTSTACK_ACL_BUFFERS_FULL;
    if(cid==hid_control_cid)sent_control.emplace_back(p,p+n);
    return 0;
}
}

[[maybe_unused]] static void event_working() {
    uint8_t event[]={BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING};
    hci_packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
}
static void tick(uint64_t after=1) { clock_ms+=after;bt_tick(); }

#ifdef CLASSIC_BASELINE_ONLY
int main() {
    command_credit=false;
    bt_init();event_working();tick(101);
    bool paging_lost=raw_commands && connecting;
    if(paging_lost) {
        std::puts("REPRODUCED fe6bf09: busy raw HCI create rejected, firmware still marks connecting; no completion event exists");
    }
    uint8_t event[13]={HCI_EVENT_CONNECTION_COMPLETE,11,0,0x42,0};
    reverse_bytes(peer,event+5,6);event[11]=HCI_LINK_TYPE_ACL;
    hci_packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
    tick(30001);
    const bool security_lost=acl_handle==0x42&&hid_interrupt_cid==0&&disconnects==0;
    if(security_lost)std::puts("REPRODUCED fe6bf09: rejected raw authentication leaves ACL-only connection indefinitely, no HID watchdog");
    hid_control_cid=0x70;acl_credit=false;send_attempts=0;init_feature();
    const bool features_lost=send_attempts==5&&sent_control.empty();
    if(features_lost)std::puts("REPRODUCED fe6bf09: all five initialization feature requests, including 05, silently lost while ACL buffer busy");
    return paging_lost&&security_lost&&features_lost ? 1 : 0;
}
#else
static void reset(bool bond=true,bool working=true) {
    device_found=false;classic_enabled=true;reconnect_suppressed=false;pairing_requested=false;
    connecting=false;inquiring=false;stack_was_working=false;active_reconnect_requested=false;
    disconnect_pending=false;control_open=interrupt_open=false;acl_started_ms=hid_ready_ms=0;full_input_seen=false;
    retry_due_ms=connect_started_ms=feature_probe_due_ms=0;saved_device_valid=false;
    prune_keys_pending=forget_keys_pending=forget_pair_keys_pending=new_pair=false;acl_handle=HCI_CON_HANDLE_INVALID;
    hid_control_cid=hid_interrupt_cid=0;check_dse=false;active_send_epoch=0;next_send_epoch=0;
    send_requested=control_send_requested=false;control_send_queue.clear();feature_data.clear();
    memset(current_device_addr,0,6);memset(saved_device_addr,0,6);
    saved=bond;clock_ms=100;mock_hci_state=working?HCI_STATE_WORKING:HCI_STATE_OFF;
    command_credit=acl_credit=true;inquiries=drops=disconnects=security_requests=raw_commands=0;
    page_scan=discoverable=false;create_status=0;next_cid=0x40;connection_filter=nullptr;
    created_psms.clear();accepted_cids.clear();declined_cids.clear();detached_cids.clear();
    sent_control.clear();can_send_requests.clear();input_callbacks=send_attempts=0;
    bt_register_data_callback([](CHANNEL_TYPE,uint8_t *,uint16_t){++input_callbacks;});
    assert(bt_init()==0);assert(connection_filter&&required_security==LEVEL_2);
}
static void acl_complete(uint8_t status=0,uint16_t handle=0x42,const uint8_t *address=peer) {
    uint8_t event[13]={HCI_EVENT_CONNECTION_COMPLETE,11,status};
    little_endian_store_16(event,3,handle);reverse_bytes(address,event+5,6);event[11]=HCI_LINK_TYPE_ACL;
    hci_packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
}
static void dropped(uint16_t handle=0x42) {
    uint8_t event[]={HCI_EVENT_DISCONNECTION_COMPLETE,4,0,0,0,0x13};little_endian_store_16(event,3,handle);
    hci_packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
}
static void opened(uint16_t cid,uint16_t psm,uint8_t status=0,uint16_t handle=0x42) {
    uint8_t event[26]={L2CAP_EVENT_CHANNEL_OPENED,24,status};reverse_bytes(peer,event+3,6);
    little_endian_store_16(event,9,handle);little_endian_store_16(event,11,psm);little_endian_store_16(event,13,cid);
    l2cap_packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
}
static void incoming(uint16_t cid,uint16_t psm,uint16_t handle=0x42) {
    uint8_t event[16]={L2CAP_EVENT_INCOMING_CONNECTION,14};reverse_bytes(peer,event+2,6);
    little_endian_store_16(event,8,handle);little_endian_store_16(event,10,psm);little_endian_store_16(event,12,cid);
    l2cap_packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
}
static void incoming_ready(bool reverse=false) {
    assert(connection_filter(peer,HCI_LINK_TYPE_ACL)==1);acl_complete();
    assert(!bt_classic_connected()&&security_requests==1);
    if(reverse){incoming(0x71,PSM_HID_INTERRUPT);opened(0x71,PSM_HID_INTERRUPT);assert(!bt_classic_connected());}
    incoming(0x70,PSM_HID_CONTROL);opened(0x70,PSM_HID_CONTROL);
    if(!reverse){assert(!bt_classic_connected());incoming(0x71,PSM_HID_INTERRUPT);opened(0x71,PSM_HID_INTERRUPT);}
    assert(bt_classic_connected());
}
static void can_send(uint16_t cid) {
    uint8_t event[]={L2CAP_EVENT_CAN_SEND_NOW,2,0,0};little_endian_store_16(event,2,cid);
    can_send_requests.erase(cid);l2cap_packet_handler(HCI_EVENT_PACKET,0,event,sizeof(event));
}
static void inquiry_found() {
    uint8_t result[17]={HCI_EVENT_INQUIRY_RESULT,15,1};reverse_bytes(peer,result+3,6);
    result[12]=8;result[13]=0x25; // gamepad CoD 0x002508
    hci_packet_handler(HCI_EVENT_PACKET,0,result,sizeof(result));
    uint8_t complete[]={GAP_EVENT_INQUIRY_COMPLETE,1,0};
    hci_packet_handler(HCI_EVENT_PACKET,0,complete,sizeof(complete));
}
int main() {
    // Shared HCI was already working before bt_init, so no new WORKING event.
    reset();command_credit=false;tick();tick(101);
    assert(page_scan&&!discoverable&&inquiries==0&&!connecting&&raw_commands==0);
    incoming_ready();assert(created_psms.empty());
    uint8_t data[79]={0xa1,0x31};l2cap_packet_handler(L2CAP_DATA_PACKET,hid_interrupt_cid,data,sizeof(data));
    assert(input_callbacks==1);
    dropped();tick(501);assert(page_scan&&saved&&drops==0&&!bt_classic_connected());
    security_requests=0;incoming_ready(true); // PS wake can open channels in either order
    assert(bt_classic_connected()&&raw_commands==0);
    std::puts("PASS: saved-peer boot without WORKING replay, PS incoming, disconnect/reconnect, both HID ordering");

    // Explicit reconnect is host-initiated and remains queued while HCI busy.
    reset();command_credit=false;bt_reconnect();tick(301);tick(101);
    assert(created_psms.size()==1&&created_psms[0]==PSM_HID_CONTROL&&connecting&&raw_commands==0);
    acl_complete();assert(security_requests==1&&!bt_classic_connected());
    opened(hid_control_cid,PSM_HID_CONTROL);
    assert(created_psms.size()==2&&created_psms[1]==PSM_HID_INTERRUPT);
    opened(hid_interrupt_cid,PSM_HID_INTERRUPT);assert(bt_classic_connected());
    // Control requests are not lost under an occupied ACL buffer; 05 is first.
    assert(control_send_queue.size()==5&&sent_control.empty());
    acl_credit=false;tick();can_send(hid_control_cid);
    assert(control_send_queue.size()==5&&sent_control.empty());
    acl_credit=true;
    for(unsigned i=0;i<5;++i){tick();can_send(hid_control_cid);}
    assert(sent_control.size()==5&&sent_control[0]==std::vector<uint8_t>({0x43,0x05}));
    assert(control_send_queue.empty());
    for(unsigned i=0;i<30;++i)get_feature_data(0x50+i,64);
    assert(control_send_queue.size()==CONTROL_QUEUE_LIMIT);
    bt_disconnect();assert(control_send_queue.empty()&&!bt_classic_connected());
    l2cap_packet_handler(L2CAP_DATA_PACKET,hid_interrupt_cid,data,sizeof(data));assert(input_callbacks==0);
    dropped();tick(10000);assert(!page_scan&&saved&&drops==0);
    bt_resume_auto();tick(101);assert(page_scan&&!reconnect_suppressed&&saved);
    std::puts("PASS: managed outgoing/security under credit pressure; feature 05 queue, busy retry, bounds, teardown");

    // Pairing is explicit destructive re-bond, automatic resumes keep keys.
    reset();bt_pair();tick(301);tick(101);assert(!saved&&drops==1&&inquiring);
    command_credit=false;inquiry_found();assert(connecting&&created_psms.size()==1&&raw_commands==0);
    acl_complete();opened(hid_control_cid,PSM_HID_CONTROL);opened(hid_interrupt_cid,PSM_HID_INTERRUPT);
    saved=true;tick();assert(saved_device_valid&&!pairing_requested);
    dropped();tick(501);assert(page_scan&&!inquiring&&saved&&drops==1);
    security_requests=0;incoming_ready();assert(bt_classic_connected());
    reset(true);tick();tick(101);assert(page_scan&&!inquiring); // persisted bond after reboot
    std::puts("PASS: pair -> persist -> disconnect -> PS automatic reconnect -> reboot preserves bond");

    reset();bt_set_classic_enabled(false);tick();tick(1000);assert(!page_scan&&!inquiring);
    bt_set_classic_enabled(true);tick(101);assert(page_scan);
    bt_disconnect();bt_set_classic_enabled(false);bt_set_classic_enabled(true);tick(1000);
    assert(!page_scan&&reconnect_suppressed);bt_resume_auto();tick(101);assert(page_scan);
    uint8_t stranger[6]={9,9,9,9,9,9};
    assert(connection_filter(stranger,HCI_LINK_TYPE_ACL)==0&&connection_filter(peer,HCI_LINK_TYPE_SCO)==0);
    assert(connection_filter(peer,HCI_LINK_TYPE_ACL)==1);acl_complete();
    tick(15001);assert(disconnects==1&&disconnect_pending&&!bt_classic_connected());
    tick(15001);assert(disconnects==1);dropped();tick(501);assert(page_scan);
    security_requests=0;incoming_ready();
    uint8_t closed[]={L2CAP_EVENT_CHANNEL_CLOSED,2,0x70,0};
    l2cap_packet_handler(HCI_EVENT_PACKET,0,closed,sizeof(closed));
    assert(disconnect_pending&&!bt_classic_connected()&&active_send_epoch.load()==0);
    std::puts("PASS: preference arbitration vs manual suppression, peer filtering, ACL/HID watchdog, partial close");

    reset();bt_reconnect();tick(301);tick(101);auto old_cid=hid_control_cid;
    bt_set_classic_enabled(false);assert(disconnect_pending);
    acl_complete();assert(disconnects==1);opened(old_cid,PSM_HID_CONTROL);
    assert(!bt_classic_connected()&&!detached_cids.empty());dropped();
    bt_resume_auto();tick(101);assert(page_scan&&saved);
    // Immediate resource failures must not leave a phantom CID or connecting.
    reset();create_status=BTSTACK_MEMORY_ALLOC_FAILED;bt_reconnect();tick(301);tick(101);
    assert(!connecting&&hid_control_cid==0&&retry_due_ms>clock_ms);
    create_status=0;tick(2001);assert(connecting&&hid_control_cid!=0);
    auto current=hid_control_cid;opened(current+10,PSM_HID_CONTROL,1);
    assert(hid_control_cid==current&&connecting); // stale failure cannot kill new attempt
    opened(current,PSM_HID_CONTROL,1);assert(!connecting&&!disconnect_pending);
    assert(raw_commands==0);
    std::puts("PASS: cancellation/late callbacks, immediate create failure/retry, stale channel isolation");

    reset();bt_pair();assert(forget_keys_pending&&forget_pair_keys_pending);
    bt_cancel_manual_request();tick();tick(101);
    assert(saved&&drops==0&&!pairing_requested&&!active_reconnect_requested&&page_scan&&!inquiring);
    bt_reconnect();tick(301);assert(connecting&&active_reconnect_requested);
    old_cid=hid_control_cid;bt_cancel_manual_request();assert(disconnect_pending&&!active_reconnect_requested);
    opened(old_cid,PSM_HID_CONTROL,ERROR_CODE_PAGE_TIMEOUT);tick(2001);
    assert(page_scan&&!connecting&&created_psms.size()==1&&saved&&drops==0);
    bt_forget();bt_cancel_manual_request();tick();assert(!saved&&drops==1&&reconnect_suppressed);
    std::puts("PASS: cancel pair before key deletion, cancel queued reconnect restores passive Auto, explicit forget remains explicit");

    reset();tick();tick(101);incoming_ready();tick(5001);
    assert(disconnect_pending&&disconnects==1&&saved&&drops==0);
    dropped();tick(501);security_requests=0;incoming_ready();
    l2cap_packet_handler(L2CAP_DATA_PACKET,hid_interrupt_cid,data,sizeof(data));tick(5001);
    assert(full_input_seen&&bt_classic_connected()&&disconnects==1);
    // Canceling a completed manual request must not tear down a live session.
    active_reconnect_requested=true;bt_cancel_manual_request();assert(bt_classic_connected());
    std::puts("PASS: missing full 0x31 input recovers automatically; live input and live manual-cancel remain connected");

    // SDK link-key DB can become available after bt_init, before the first
    // policy tick. The first PS page must not be rejected by a stale cache.
    reset(false);saved=true;assert(!saved_device_valid);
    assert(connection_filter(peer,HCI_LINK_TYPE_ACL)==1&&saved_device_valid&&drops==0);
    std::puts("PASS: cold-start incoming PS wake reloads the newly available SDK bond before first policy tick");
}
#endif
