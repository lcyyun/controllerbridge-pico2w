//
// Created by awalol on 2026/3/4.
//

#include <cstdio>
#include <cstring>
#include "bt.h"
#include <queue>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <deque>
#include "btstack_event.h"
#include "gap.h"
#include "l2cap.h"
#include "pico/cyw43_arch.h"
#include "utils.h"
#include "bsp/board_api.h"
#include "classic/sdp_server.h"
#include "config.h"
#include "state_mgr.h"
#include "pico/util/queue.h"
#if ENABLE_BATT_LED
#include "battery_led.h"
#endif

#define MTU_CONTROL 672
#define MTU_INTERRUPT 672

using std::unordered_map;
using std::vector;
using std::queue;

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static btstack_packet_callback_registration_t hci_event_callback_registration, l2cap_event_callback_registration;
static bd_addr_t current_device_addr;
static bool device_found = false;
static bool classic_enabled = true;
static bool reconnect_suppressed = false;
static bool pairing_requested = false;
static bool connecting = false;
static bool inquiring = false;
static bool stack_was_working = false;
static bool active_reconnect_requested = false;
static bool disconnect_pending = false;
static bool control_open = false, interrupt_open = false;
static uint64_t acl_started_ms = 0;
static uint64_t hid_ready_ms = 0;
static bool full_input_seen = false;
static uint64_t retry_due_ms = 0;
static uint64_t connect_started_ms = 0;
static uint64_t feature_probe_due_ms = 0;
static bd_addr_t saved_device_addr;
static bool saved_device_valid = false;
static bool prune_keys_pending = false;
static bool forget_keys_pending = false;
static bool forget_pair_keys_pending = false;
static bool new_pair = false; // Outgoing HID initiator; incoming peers open their channels.
static hci_con_handle_t acl_handle = HCI_CON_HANDLE_INVALID;
static uint16_t hid_control_cid;
static uint16_t hid_interrupt_cid;
static bt_data_callback_t bt_data_callback = nullptr;
static bool check_dse = false;
static int8_t bt_rssi = 0;
static std::atomic<uint32_t> active_send_epoch{0};
static uint32_t next_send_epoch = 0;
static bool send_requested = false;
static bool control_send_requested = false;
static std::deque<vector<uint8_t>> control_send_queue;
static constexpr size_t CONTROL_QUEUE_LIMIT = 8;
unordered_map<uint8_t, vector<uint8_t> > feature_data;
queue_t send_fifo;

struct send_element {
    uint8_t data[512];
    size_t len;
    uint32_t epoch;
};

absolute_time_t inactive_time = 0; // 手柄长时间静默

void bt_register_data_callback(bt_data_callback_t callback) {
    bt_data_callback = callback;
}

void bt_send_packet(uint8_t *data, uint16_t len) {
    if (interrupt_open && !disconnect_pending) {
        l2cap_send(hid_interrupt_cid, data, len);
    }
}

void bt_send_control(uint8_t *data, uint16_t len) {
    if (control_open && !disconnect_pending) {
        l2cap_send(hid_control_cid, data, len);
    }
}

static bool disconnect_link() {
    active_send_epoch.store(0, std::memory_order_release);
    send_requested = false;
    control_send_requested = false;
    control_send_queue.clear();
    if (disconnect_pending) return acl_handle != HCI_CON_HANDLE_INVALID || connecting;
    disconnect_pending = acl_handle != HCI_CON_HANDLE_INVALID || connecting;
    if (acl_handle == HCI_CON_HANDLE_INVALID) {
        // BTstack has no Classic cancel API for a queued L2CAP create. Keep
        // ownership until its failure/completion, then reject the stale link.
        return connecting;
    }

    // Managed API retries when the shared Classic/LE HCI credit is busy.
    gap_disconnect(acl_handle);
    return true;
}

static uint64_t bt_now_ms() { return time_us_64() / 1000; }

static bool load_saved_peer() {
    btstack_link_key_iterator_t iterator;
    link_key_t key;
    link_key_type_t type;
    saved_device_valid = false;
    if (gap_link_key_iterator_init(&iterator)) {
        saved_device_valid = gap_link_key_iterator_get_next(&iterator, saved_device_addr, key, &type);
        gap_link_key_iterator_done(&iterator);
    }
    return saved_device_valid;
}

bool bt_has_saved_peer() { return load_saved_peer(); }

bool bt_pairing_active() {
    return classic_enabled && !reconnect_suppressed &&
           (pairing_requested || !saved_device_valid) &&
           (inquiring || connecting ||
            (acl_handle != HCI_CON_HANDLE_INVALID && new_pair && hid_interrupt_cid == 0));
}

static void schedule_retry(uint32_t delay_ms = 1000) {
    device_found = false;
    retry_due_ms = 0;
    if (classic_enabled && !reconnect_suppressed) retry_due_ms = bt_now_ms() + delay_ms;
}

bool bt_disconnect() {
    reconnect_suppressed = true;
    retry_due_ms = 0;
    gap_inquiry_stop();
    inquiring = false;
    gap_connectable_control(0);
    gap_discoverable_control(0);
    return disconnect_link();
}

void bt_pair() {
    reconnect_suppressed = false;
    classic_enabled = true;
    pairing_requested = true;
    active_reconnect_requested = false;
    forget_keys_pending = true;
    forget_pair_keys_pending = true;
    gap_inquiry_stop();
    inquiring = false;
    disconnect_link();
    schedule_retry(300);
}

void bt_reconnect() {
    reconnect_suppressed = false;
    classic_enabled = true;
    pairing_requested = false;
    active_reconnect_requested = true;
    gap_inquiry_stop();
    inquiring = false;
    disconnect_link();
    schedule_retry(300);
}

void bt_forget() {
    bt_disconnect();
    forget_keys_pending = true;
    forget_pair_keys_pending = false;
    pairing_requested = false;
    active_reconnect_requested = false;
}

void bt_resume_auto() {
    bt_cancel_manual_request();
    reconnect_suppressed = false;
    pairing_requested = false;
    active_reconnect_requested = false;
    classic_enabled = true;
    if (!bt_classic_connected()) schedule_retry(100);
}

void bt_cancel_manual_request() {
    const bool manual = pairing_requested || active_reconnect_requested;
    pairing_requested = active_reconnect_requested = false;
    if (forget_pair_keys_pending) {
        forget_keys_pending = false;
        forget_pair_keys_pending = false;
    }
    if (!manual || bt_classic_connected()) return;
    gap_inquiry_stop();
    inquiring = false;
    device_found = false;
    disconnect_link();
    schedule_retry(100);
}

static void start_outgoing_hid() {
    connecting = true;
    disconnect_pending = false;
    connect_started_ms = bt_now_ms();
    new_pair = true;
    // Unlike raw HCI_CREATE_CONNECTION, L2CAP queues the request and drives
    // ACL + LEVEL_2 security even while LE scanning consumes command credits.
    const uint8_t status = l2cap_create_channel(l2cap_packet_handler,
        current_device_addr, PSM_HID_CONTROL, MTU_CONTROL, &hid_control_cid);
    if (status != ERROR_CODE_SUCCESS) {
        hid_control_cid = 0;
        connecting = false;
        new_pair = false;
        printf("[BT] HID connect could not queue status=0x%02X\n", status);
        schedule_retry(2000);
    }
}

// BTstack invokes this before it allocates/accepts the incoming ACL. Sending a
// second raw ACCEPT_CONNECTION_REQUEST from our event callback races its HCI
// state machine. Let it own accept/reject and the saved link-key/SSP exchange.
static int accept_classic_connection(bd_addr_t addr, hci_link_type_t type) {
    if (type != HCI_LINK_TYPE_ACL || !classic_enabled || reconnect_suppressed ||
        forget_keys_pending || disconnect_pending || acl_handle != HCI_CON_HANDLE_INVALID) return 0;
    // The SDK may install its TLV link-key DB during WORKING after bt_init.
    // A PS wake arriving before our first 100 ms policy tick must still match
    // the persisted peer. This is read-only; keys are never printed or erased.
    if (!saved_device_valid) load_saved_peer();
    if (saved_device_valid && !pairing_requested && memcmp(addr, saved_device_addr, 6) != 0) return 0;
    if (connecting && memcmp(addr, current_device_addr, 6) != 0) return 0;
    if (!saved_device_valid && !pairing_requested && !device_found) return 0;
    if (!connecting) new_pair = false;
    bd_addr_copy(current_device_addr, addr);
    connecting = true;
    connect_started_ms = bt_now_ms();
    return 1;
}

static void forget_keys() {
    // This receiver's Classic key DB only contains paired controller peers.
    for (unsigned i = 0; i < 4 && load_saved_peer(); ++i) {
        gap_drop_link_key_for_bd_addr(saved_device_addr);
    }
    load_saved_peer();
    memset(current_device_addr, 0, sizeof(current_device_addr));
    forget_keys_pending = false;
    forget_pair_keys_pending = false;
}

void bt_tick() {
    const bool working = hci_get_state() == HCI_STATE_WORKING;
    if (working && !stack_was_working) {
        // hci_power_control(ON) does not replay WORKING when another shared
        // client has already powered the controller before we registered.
        load_saved_peer();
        schedule_retry(100);
    }
    stack_was_working = working;
    if (!working) return;
    if (forget_keys_pending && acl_handle == HCI_CON_HANDLE_INVALID && !connecting) forget_keys();
    if (prune_keys_pending && acl_handle != HCI_CON_HANDLE_INVALID) {
        prune_keys_pending = false;
        bd_addr_t peers[4];
        size_t count = 0;
        btstack_link_key_iterator_t iterator;
        link_key_t key;
        link_key_type_t type;
        if (gap_link_key_iterator_init(&iterator)) {
            while (count < 4 && gap_link_key_iterator_get_next(&iterator, peers[count], key, &type)) ++count;
            gap_link_key_iterator_done(&iterator);
        }
        for (size_t i = 0; i < count; ++i) {
            if (memcmp(peers[i], current_device_addr, 6) != 0) gap_drop_link_key_for_bd_addr(peers[i]);
        }
        load_saved_peer();
    }
    if (!classic_enabled || reconnect_suppressed || disconnect_pending) return;
    // The audio producer runs on core 1. Only core 0 may call BTstack.
    if (hid_interrupt_cid && active_send_epoch.load(std::memory_order_acquire) &&
        !send_requested && !queue_is_empty(&send_fifo)) {
        send_requested = true;
        l2cap_request_can_send_now_event(hid_interrupt_cid);
    }
    if (control_open && !control_send_requested && !control_send_queue.empty()) {
        control_send_requested = true;
        l2cap_request_can_send_now_event(hid_control_cid);
    }
    const uint64_t now = bt_now_ms();
    if (bt_classic_connected() && !full_input_seen && now - hid_ready_ms >= 5000) {
        printf("[BT] No full controller input after HID init; reconnect\n");
        disconnect_link();
        return;
    }
    if (check_dse && feature_probe_due_ms && now >= feature_probe_due_ms) {
        // A missing optional Edge feature reply must not stall USB enumeration.
        check_dse = false;
        feature_probe_due_ms = 0;
#if !ENABLE_SERIAL && !ENABLE_AUTO_PROFILE
        tud_connect();
#endif
    }
    if (acl_handle != HCI_CON_HANDLE_INVALID && !bt_classic_connected() &&
        now - acl_started_ms >= 15000) {
        printf("[BT] HID/security setup timeout; disconnect and retry\n");
        disconnect_link();
        return;
    }
    if (connecting && acl_handle == HCI_CON_HANDLE_INVALID && now - connect_started_ms >= 20000) {
        // The controller's page timeout or eventual ACL completion owns cleanup
        // of a pending L2CAP channel; never start a second overlapping attempt.
        printf("[BT] Pending ACL timed out; reject late completion\n");
        disconnect_link();
        return;
    }
    if (acl_handle != HCI_CON_HANDLE_INVALID || connecting || inquiring || hid_control_cid || hid_interrupt_cid ||
        retry_due_ms == 0 || now < retry_due_ms) return;
    retry_due_ms = 0;
    load_saved_peer();
    gap_connectable_control(1); // Preserve controller-initiated HID reconnect.
    gap_discoverable_control(pairing_requested || !saved_device_valid);
    if (saved_device_valid && !pairing_requested && !active_reconnect_requested) {
        // A bonded DualSense pages its saved host when PS is pressed. Stay
        // connectable instead of continuously paging a sleeping controller and
        // racing its incoming HID reconnect (same policy as the BL616 path).
        printf("[BT] Saved controller ready for incoming reconnect\n");
    } else if (saved_device_valid && !pairing_requested) {
        bd_addr_copy(current_device_addr, saved_device_addr);
        start_outgoing_hid();
    } else {
        const int status = gap_inquiry_start(5);
        inquiring = status == ERROR_CODE_SUCCESS;
        if (!inquiring) schedule_retry(2000);
    }
}

void bt_set_classic_enabled(bool enabled) {
    if (classic_enabled == enabled) {
        return;
    }

    classic_enabled = enabled;
    printf("[BT] classic %s\n", enabled ? "enabled" : "disabled");

    if (!enabled) {
        device_found = false;
        gap_inquiry_stop();
        disconnect_link();
        inquiring = false;
        retry_due_ms = 0;
        gap_connectable_control(0);
        gap_discoverable_control(0);
        return;
    }

    if (hci_get_state() == HCI_STATE_WORKING && acl_handle == HCI_CON_HANDLE_INVALID) {
        schedule_retry(100);
    }
}

bool bt_classic_connected() {
    return classic_enabled && !reconnect_suppressed && !disconnect_pending &&
           acl_handle != HCI_CON_HANDLE_INVALID && control_open && interrupt_open;
}

void bt_get_signal_strength(int8_t *rssi) {
    // gap_read_rssi() completes asynchronously, so this function can only
    // return the last cached RSSI value. Trigger a refresh afterwards so a
    // subsequent call can observe the updated value once the RSSI event arrives.
    if (rssi != nullptr) {
        *rssi = bt_rssi;
    }
    if (acl_handle != HCI_CON_HANDLE_INVALID) {
        gap_read_rssi(acl_handle);
    }
}

void bt_l2cap_init() {
    l2cap_init();
    l2cap_event_callback_registration.callback = &l2cap_packet_handler;
    l2cap_add_event_handler(&l2cap_event_callback_registration);
    // 修复重连后自动断开的关键点
    sdp_init();
    l2cap_register_service(l2cap_packet_handler, PSM_HID_CONTROL, MTU_CONTROL, LEVEL_2);
    l2cap_register_service(l2cap_packet_handler, PSM_HID_INTERRUPT, MTU_INTERRUPT, LEVEL_2);

}

int bt_init() {
    queue_init(&send_fifo, sizeof(send_element), 10);

    bt_l2cap_init();

    // SSP (Secure Simple Pairing)
    gap_ssp_set_enable(true);
    gap_secure_connections_enable(true);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(1);
    gap_set_bondable_mode(1);
    gap_set_security_level(LEVEL_2);
    gap_register_classic_connection_filter(accept_classic_connection);
    load_saved_peer();

    gap_connectable_control(1);
    gap_discoverable_control(!saved_device_valid);

    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    hci_power_control(HCI_POWER_ON);
    if (hci_get_state() == HCI_STATE_WORKING) schedule_retry(100);
    return 0;
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) channel;
    if (packet_type != HCI_EVENT_PACKET || !packet || size < 2) return;

    const uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case BTSTACK_EVENT_STATE: {
            const uint8_t state = btstack_event_state_get_state(packet);
            printf("[BT] State: %u\n", state);
            if (state == HCI_STATE_WORKING && classic_enabled) {
                stack_was_working = true;
                load_saved_peer();
                printf("[BT] Stack ready, restore controller policy\n");
                schedule_retry(100);
            }
            break;
        }
        case HCI_EVENT_INQUIRY_RESULT:
        case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
        case HCI_EVENT_EXTENDED_INQUIRY_RESPONSE: {
            if (!classic_enabled || reconnect_suppressed || !inquiring ||
                connecting || device_found || acl_handle != HCI_CON_HANDLE_INVALID) {
                break;
            }

            bd_addr_t addr;
            uint32_t cod;

            if (event_type == HCI_EVENT_INQUIRY_RESULT) {
                cod = hci_event_inquiry_result_get_class_of_device(packet);
                hci_event_inquiry_result_get_bd_addr(packet, addr);
            } else if (event_type == HCI_EVENT_INQUIRY_RESULT_WITH_RSSI) {
                cod = hci_event_inquiry_result_with_rssi_get_class_of_device(packet);
                hci_event_inquiry_result_with_rssi_get_bd_addr(packet, addr);
            } else {
                cod = hci_event_extended_inquiry_response_get_class_of_device(packet);
                hci_event_extended_inquiry_response_get_bd_addr(packet, addr);
            }

            // CoD 0x002508 = Gamepad (Major: Peripheral, Minor: Gamepad)
            if ((cod & 0x000F00) == 0x000500 &&
                (pairing_requested || !saved_device_valid ||
                 memcmp(addr, saved_device_addr, 6) == 0)) {
                printf("[HCI] Gamepad found: %s (CoD: 0x%06x)\n", bd_addr_to_str(addr), (unsigned int) cod);
                bd_addr_copy(current_device_addr, addr);
                device_found = true;
                gap_inquiry_stop();
            }
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
        case HCI_EVENT_INQUIRY_COMPLETE: {
            printf("[HCI] Inquiry complete.\n");
            if (!classic_enabled || reconnect_suppressed || connecting ||
                acl_handle != HCI_CON_HANDLE_INVALID || (!inquiring && !device_found)) {
                break;
            }
            inquiring = false;
            if (device_found) {
                printf("[HCI] Connecting to %s...\n", bd_addr_to_str(current_device_addr));
                start_outgoing_hid();
                break;
            }
            printf("[HCI] Restart inquiry\n");
            schedule_retry(500);
            break;
        }
        case HCI_EVENT_COMMAND_STATUS: {
            const uint8_t status = hci_event_command_status_get_status(packet);
            const uint16_t opcode = hci_event_command_status_get_command_opcode(packet);
            printf("[HCI] CmdStatus %s(0x%04X) status=0x%02X\n", opcode_to_str(opcode), opcode, status);
            // L2CAP owns outgoing HCI failures and emits CHANNEL_OPENED with
            // an error; a raw CmdStatus has no peer identity and may be stale.
            break;
        }

        case HCI_EVENT_COMMAND_COMPLETE: {
            const uint8_t status = hci_event_command_complete_get_return_parameters(packet)[0];
            const uint16_t opcode = hci_event_command_complete_get_command_opcode(packet);
            if (opcode != HCI_OPCODE_HCI_READ_RSSI) {
                printf("[HCI] CmdComplete %s(0x%04X) status=0x%02X\n", opcode_to_str(opcode), opcode, status);
            }
            if (opcode == HCI_OPCODE_HCI_READ_RSSI) {
                if (status != ERROR_CODE_SUCCESS || packet[1] < 7) {
                    printf("[HCI] RSSI complete failed status=0x%02X param_len=%u\n", status, packet[1]);
                }
            }
            break;
        }

        case HCI_EVENT_CONNECTION_COMPLETE: {
            const uint8_t status = hci_event_connection_complete_get_status(packet);
            bd_addr_t addr;
            hci_event_connection_complete_get_bd_addr(packet, addr);
            if (hci_event_connection_complete_get_link_type(packet) != HCI_LINK_TYPE_ACL) break;
            if (status == 0) {
                const hci_con_handle_t handle = hci_event_connection_complete_get_connection_handle(packet);
                if (!classic_enabled || reconnect_suppressed ||
                    memcmp(addr, current_device_addr, 6) != 0 ||
                    (acl_handle != HCI_CON_HANDLE_INVALID && acl_handle != handle)) {
                    printf("[HCI] ACL connected while Classic disabled, disconnect handle=0x%04X\n", handle);
                    if (acl_handle == HCI_CON_HANDLE_INVALID && connecting && memcmp(addr,current_device_addr,6)==0)
                        acl_handle = handle; // retain ownership until disconnection cleanup
                    gap_disconnect(handle);
                    break;
                }
                acl_handle = handle;
                acl_started_ms = bt_now_ms();
                inquiring = false;
                retry_due_ms = 0;
                bt_rssi = 0;
                printf("[HCI] ACL connected handle=0x%04X\n", handle);
                if (disconnect_pending) { gap_disconnect(handle); break; }
                // For outgoing links the pending control channel drives this
                // too. The managed request coalesces rather than duplicating
                // AUTHENTICATION_REQUESTED/SET_ENCRYPTION commands.
                gap_request_security_level(handle, LEVEL_2);
            } else if (memcmp(addr,current_device_addr,6)==0 && connecting) {
                device_found = false;
                new_pair = false;
                connecting = false;
                disconnect_pending = false;
                printf("[HCI] ACL connect failed status=0x%02X, restart inquiry\n", status);
                schedule_retry(2000);
            }
            break;
        }

        case HCI_EVENT_LINK_KEY_REQUEST: {
            // ENABLE_EXPLICIT_LINK_KEY_REPLY is not enabled: BTstack loads
            // the persistent key and schedules exactly one reply itself.
            break;
        }

        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            // BTstack's configured Just Works auto-accept owns this reply.
            break;
        }

        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t addr;
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            printf("[HCI] Legacy pin request from %s, reply 0000\n", bd_addr_to_str(addr));
            gap_pin_code_response(addr, "0000");
            break;
        }

        case HCI_EVENT_AUTHENTICATION_COMPLETE: {
            const uint8_t status = hci_event_authentication_complete_get_status(packet);
            const hci_con_handle_t handle = hci_event_authentication_complete_get_connection_handle(packet);
            if (handle != acl_handle) {
                break;
            }
            printf("[HCI] Authentication complete handle=0x%04X status=0x%02X\n", handle, status);
            if (status != ERROR_CODE_SUCCESS) {
                // Keep the saved key on transient radio failures. Pair/forget is explicit.
                disconnect_link();
            }
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE: {
            const uint8_t status = hci_event_encryption_change_get_status(packet);
            const hci_con_handle_t handle = hci_event_encryption_change_get_connection_handle(packet);
            const uint8_t enabled = hci_event_encryption_change_get_encryption_enabled(packet);
            if (handle != acl_handle) {
                break;
            }
            printf("[HCI] Encryption change handle=0x%04X status=0x%02X enabled=%u\n", handle, status, enabled);
            if (status != ERROR_CODE_SUCCESS || !enabled) disconnect_link();
            break;
        }

        case HCI_EVENT_CONNECTION_REQUEST: {
            bd_addr_t addr;
            hci_event_connection_request_get_bd_addr(packet, addr);
            const uint32_t cod = hci_event_connection_request_get_class_of_device(packet);
            printf("[HCI] Incoming ACL request from %s cod=0x%06x\n", bd_addr_to_str(addr), (unsigned int) cod);
            if (connecting && !disconnect_pending && memcmp(addr,current_device_addr,6)==0) {
                gap_inquiry_stop();
                inquiring = false;
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            const hci_con_handle_t handle = hci_event_disconnection_complete_get_connection_handle(packet);
            if (handle != acl_handle) {
                break;
            }
#if !ENABLE_SERIAL && !ENABLE_AUTO_PROFILE
            tud_disconnect();
#endif
            const uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            device_found = false;
            new_pair = false;
            acl_handle = HCI_CON_HANDLE_INVALID;
            bt_rssi = 0;
            hid_control_cid = 0;
            hid_interrupt_cid = 0;
            control_open = interrupt_open = false;
            full_input_seen = false;
            connecting = false;
            disconnect_pending = false;
            active_send_epoch.store(0, std::memory_order_release);
            send_requested = false;
            control_send_requested = false;
            control_send_queue.clear();
            check_dse = false;
            feature_probe_due_ms = 0;
            send_element discarded;
            while (queue_try_remove(&send_fifo, &discarded)) {}
            feature_data.clear();
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
#if ENABLE_BATT_LED
            battery_led_on_disconnect();
#endif
            printf("[HCI] Disconnected reason=0x%02X, start inquiry\n", reason);
            schedule_retry(500);
            break;
        }

        case GAP_EVENT_RSSI_MEASUREMENT: {
            const hci_con_handle_t handle = gap_event_rssi_measurement_get_con_handle(packet);
            if (handle == acl_handle) {
                bt_rssi = static_cast<int8_t>(gap_event_rssi_measurement_get_rssi(packet));
            }
            break;
        }
    }
}

static void complete_hid_session() {
    if (!control_open || !interrupt_open || disconnect_pending ||
        active_send_epoch.load(std::memory_order_acquire)) return;
    connecting = false;
    full_input_seen = false;
    hid_ready_ms = bt_now_ms();
    active_reconnect_requested = false;
    if (++next_send_epoch == 0) ++next_send_epoch;
    active_send_epoch.store(next_send_epoch, std::memory_order_release);
    send_requested = false;
    pairing_requested = false;
    prune_keys_pending = true;
    if (!get_config().disable_pico_led) cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
    inactive_time = get_absolute_time();
    printf("[BT] Both HID channels ready\n");
    init_feature();
    uint8_t report32[142]{};
    report32[0] = 0x32; report32[1] = 0x10;
    report32[2] = 0x90; report32[3] = 63;
    state_set(report32 + 4, sizeof(SetStateData));
    bt_write(report32, sizeof(report32));
    gap_connectable_control(0);
    gap_discoverable_control(0);
}

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) channel;

    if (packet_type == L2CAP_DATA_PACKET) {
        if (!packet || size == 0 || !bt_classic_connected()) return;
        if (channel == hid_interrupt_cid) {
            // Mic packets share report 0x31 but are not controller state.
            if (size >= 66 && packet[0] == 0xa1 && packet[1] == 0x31 && !(packet[2] & 2))
                full_input_seen = true;
            // printf("[L2CAP] HID Interrupt data len=%u\n", size);
            // printf_hexdump(packet, size);
            if (bt_data_callback) bt_data_callback(INTERRUPT, packet, size);

            // 静默检测
            if (size < 13 || get_config().disable_inactive_disconnect) {
                return;
            }
            if (packet[3] < 120 || packet[3] > 140 ||
                packet[4] < 120 || packet[4] > 140 ||
                packet[5] < 120 || packet[5] > 140 ||
                packet[6] < 120 || packet[6] > 140 ||
                packet[7] > 0 || packet[8] > 0 ||
                packet[10] != 0x08 || packet[11] != 0x00 ||
                packet[12] != 0x00) {
                inactive_time = get_absolute_time();
            } else if (absolute_time_diff_us(inactive_time, get_absolute_time()) >
                       static_cast<int64_t>(get_config().inactive_time) * 60 * 1000 * 1000) {
                printf("disconnect when inactive\n");
                inactive_time = get_absolute_time();
                disconnect_link();
            }
        } else if (channel == hid_control_cid) {
            if (check_dse) {
                if (size >= 2 && packet[0] == 0xA3 && packet[1] == 0x70) {
                    printf("Connected DSE Controller\n");
                    check_dse = false;
                    is_dse = true;
#if !ENABLE_SERIAL && !ENABLE_AUTO_PROFILE
                    tud_connect();
#endif
                } else if (packet[0] == 0x02) {
                    printf("Connected DS5 Controller\n");
                    check_dse = false;
                    is_dse = false;
#if !ENABLE_SERIAL && !ENABLE_AUTO_PROFILE
                    tud_connect();
#endif
                }
            }
            if (size >= 2 && packet[0] == 0xA3) {
                uint8_t report_id = packet[1];
                feature_data[report_id].assign(packet + 1, packet + size);
#if ENABLE_VERBOSE
                printf("[L2CAP] Stored Feature Report 0x%02X, len=%u\n", report_id, size - 1);
#endif
            }
#if ENABLE_VERBOSE
            printf("[L2CAP] HID Control data len=%u\n", size);
            printf_hexdump(packet, size);
#endif
            if (bt_data_callback) bt_data_callback(CONTROL, packet, size);
        } else {
            printf("[L2CAP] Data on unknown channel 0x%04X (Interrupt: 0x%04X, Control: 0x%04X)\n",
                   channel, hid_interrupt_cid, hid_control_cid);
        }
        return;
    }

    if (packet_type != HCI_EVENT_PACKET || !packet || size < 2) return;
    const uint8_t event_type = hci_event_packet_get_type(packet);
    switch (event_type) {
        case L2CAP_EVENT_CHANNEL_OPENED: {
            const uint8_t status = l2cap_event_channel_opened_get_status(packet);
            const uint16_t local_cid = l2cap_event_channel_opened_get_local_cid(packet);
            if (status == 0) {
                if (!classic_enabled || reconnect_suppressed ||
                    disconnect_pending || l2cap_event_channel_opened_get_handle(packet) != acl_handle ||
                    (local_cid != hid_control_cid && local_cid != hid_interrupt_cid)) {
                    l2cap_disconnect(local_cid);
                    break;
                }
                const uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
                if (psm == PSM_HID_CONTROL) {
                    printf("[L2CAP] HID Control opened cid=0x%04X\n", local_cid);
                    hid_control_cid = local_cid;
                    control_open = true;

                    const auto mtu = l2cap_get_remote_mtu_for_local_cid(hid_control_cid);
                    printf("[L2CAP] Remote Control MTU: %d\n",mtu);
                    if (new_pair && hid_interrupt_cid == 0) {
                        const uint8_t opened = l2cap_create_channel(l2cap_packet_handler,
                            current_device_addr, PSM_HID_INTERRUPT, MTU_INTERRUPT, &hid_interrupt_cid);
                        if (opened != ERROR_CODE_SUCCESS) disconnect_link();
                    }
                } else if (psm == PSM_HID_INTERRUPT) {
                    printf("[L2CAP] HID Interrupt opened cid=0x%04X\n", local_cid);
                    hid_interrupt_cid = local_cid;
                    interrupt_open = true;

                    const auto mtu = l2cap_get_remote_mtu_for_local_cid(hid_interrupt_cid);
                    printf("[L2CAP] Remote Interrupt MTU: %d\n",mtu);

                } else {
                    printf("[L2CAP] Unknown Channel psm: 0x%02X", psm);
                }

                complete_hid_session();
            } else {
                const uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
                if (local_cid != hid_control_cid && local_cid != hid_interrupt_cid) break;
                if (local_cid == hid_control_cid) { hid_control_cid = 0; control_open = false; }
                if (local_cid == hid_interrupt_cid) { hid_interrupt_cid = 0; interrupt_open = false; }
                active_send_epoch.store(0, std::memory_order_release);
                send_requested = false;
                device_found = false;
                printf("[L2CAP] Open failed psm=0x%04X status=0x%02X\n", psm, status);
                if (acl_handle != HCI_CON_HANDLE_INVALID) disconnect_link();
                else {
                    connecting = false;
                    disconnect_pending = false;
                    new_pair = false;
                    schedule_retry(2000);
                }
            }
            break;
        }

        case L2CAP_EVENT_INCOMING_CONNECTION: {
            const uint16_t local_cid = l2cap_event_incoming_connection_get_local_cid(packet);
            const uint16_t psm = l2cap_event_incoming_connection_get_psm(packet);
            printf("[L2CAP] Incoming connection psm=0x%04X cid=0x%04X\n", psm, local_cid);
            if (classic_enabled && !reconnect_suppressed && !disconnect_pending &&
                l2cap_event_incoming_connection_get_handle(packet) == acl_handle &&
                ((psm == PSM_HID_CONTROL && hid_control_cid == 0) ||
                 (psm == PSM_HID_INTERRUPT && hid_interrupt_cid == 0))) {
                if (psm == PSM_HID_CONTROL) hid_control_cid = local_cid;
                else hid_interrupt_cid = local_cid;
                l2cap_accept_connection(local_cid);
            } else {
                l2cap_decline_connection(local_cid);
            }
            break;
        }

        case L2CAP_EVENT_CHANNEL_CLOSED: {
            const uint16_t local_cid = l2cap_event_channel_closed_get_local_cid(packet);
            if (local_cid == hid_control_cid) {
                hid_control_cid = 0;
                control_open = false;
                printf("[L2CAP] HID Control closed cid=0x%04X\n", local_cid);
            } else if (local_cid == hid_interrupt_cid) {
                hid_interrupt_cid = 0;
                interrupt_open = false;
                active_send_epoch.store(0, std::memory_order_release);
                send_requested = false;
                printf("[L2CAP] HID Interrupt closed cid=0x%04X\n", local_cid);
            } else {
                printf("[L2CAP] Channel closed cid=0x%04X\n", local_cid);
                break;
            }
            // One surviving channel is not a usable HID session. Invalidate
            // queued audio immediately and recover through a fresh ACL.
            disconnect_link();
            break;
        }

        case L2CAP_EVENT_CAN_SEND_NOW: {
            // printf("[L2CAP] L2CAP_EVENT_CAN_SEND_NOW\n");
            const uint16_t cid = l2cap_event_can_send_now_get_local_cid(packet);
            if (cid == hid_control_cid) {
                control_send_requested = false;
                if (!control_open || disconnect_pending || control_send_queue.empty()) break;
                auto &request = control_send_queue.front();
                const uint8_t status = l2cap_send(cid, request.data(), static_cast<uint16_t>(request.size()));
                if (status == ERROR_CODE_SUCCESS) control_send_queue.pop_front();
                // A busy buffer retains the request. bt_tick rearms after this
                // callback returns, avoiding recursive CAN_SEND_NOW spinning.
                break;
            }
            if (cid != hid_interrupt_cid) break;
            send_requested = false;
            if (!active_send_epoch.load(std::memory_order_acquire)) break;
            send_element send_packet{};
            if (queue_try_remove(&send_fifo, &send_packet)) {
                if (send_packet.epoch == active_send_epoch.load(std::memory_order_acquire)) {
                    const uint8_t status = l2cap_send(hid_interrupt_cid, send_packet.data, send_packet.len);
                    if (status != 0) {
                        printf("[L2CAP] L2CAP Send Error, Status: 0x%02X\n", status);
                    }
                }
            }
            if (!queue_is_empty(&send_fifo)) {
                send_requested = true;
                l2cap_request_can_send_now_event(hid_interrupt_cid);
            }
            break;
        }
    }
}

bool bt_try_write(const uint8_t *data, const uint16_t len) {
    const uint32_t epoch = active_send_epoch.load(std::memory_order_acquire);
    if (!epoch || !data || len < 4 || len >= sizeof(send_element::data)) return false;
    send_element packet{};
    packet.epoch = epoch;
    packet.len = len + 1;
    packet.data[0] = 0xA2;
    memcpy(packet.data + 1, data, len);
    fill_output_report_checksum(packet.data + 1, len);

    if (!queue_try_add(&send_fifo, &packet)) {
        printf("[L2CAP bt_write] Error: Failed to add packet to send FIFO\n");
        return false;
    }
    return true;
}

void bt_write(const uint8_t *data, const uint16_t len) { (void)bt_try_write(data, len); }

vector<uint8_t> get_feature_data(uint8_t reportId, uint16_t len) {
    (void)len;
    // 若为0x81则会请求新内容，其他若有旧数据则不进行请求
    auto ret = vector<uint8_t>{};
    if (feature_data.contains(reportId)) {
        ret = feature_data[reportId];
    }
    if (!feature_data.contains(reportId) ||
        // Get Test Command Result
        reportId == 0x81 ||
        // DSE: Set Profile Save?
        reportId == 0x63 ||
        reportId == 0x65 ||
        reportId == 0x64
    ) {
        if (control_open && !disconnect_pending) {
            bool queued = false;
            for (const auto &request : control_send_queue)
                if (request.size() == 2 && request[0] == 0x43 && request[1] == reportId) queued = true;
            if (!queued && control_send_queue.size() < CONTROL_QUEUE_LIMIT)
                control_send_queue.push_back({0x43, reportId});
#if ENABLE_VERBOSE
            printf("[L2CAP] Requesting Get Feature Report 0x%02X\n", reportId);
#endif
        }
    }
    return ret;
}

void set_feature_data(uint8_t reportId, uint8_t *data, uint16_t len) {
    if (control_open && !disconnect_pending && data && len >= 3 && len <= MTU_CONTROL - 2 &&
        control_send_queue.size() < CONTROL_QUEUE_LIMIT) {
        vector<uint8_t> get_feature(len + 2);
        get_feature[0] = 0x53;
        get_feature[1] = reportId;
        memcpy(get_feature.data() + 2, data, len);
        fill_feature_report_checksum(get_feature.data() + 1, len + 1);
#if ENABLE_VERBOSE
        printf("[L2CAP] Requesting Set Feature Report 0x%02X\n", reportId);
        printf_hexdump(get_feature.data(), len + 2);
#endif
        control_send_queue.push_back(std::move(get_feature));
    }
}

void init_feature() {
    // Request calibration first: this is also the native initialization path
    // which enables the controller's full 0x31 input reports.
    get_feature_data(0x05, 41);
    get_feature_data(0x09, 20);
    get_feature_data(0x20, 64);
    get_feature_data(0x22, 64);
    // DSE
    // check DSE by request 0x70 feature report. DSE return DEFAULT
    // If len == 1, it's DS5
    check_dse = true;
    is_dse = false;
    feature_probe_due_ms = bt_now_ms() + 1500;
    get_feature_data(0x70, 64);
}
