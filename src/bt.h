//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_BT_H
#define DS5_BRIDGE_BT_H

#include <cstdint>
#include <vector>

enum CHANNEL_TYPE {
    INTERRUPT,
    CONTROL
};

typedef void (*bt_data_callback_t)(CHANNEL_TYPE channel, uint8_t *data, uint16_t len);

int bt_init();
void bt_register_data_callback(bt_data_callback_t callback);
void bt_send_packet(uint8_t *data, uint16_t len);
void bt_send_control(uint8_t *data, uint16_t len);
void bt_write(const uint8_t *data, uint16_t len);
bool bt_try_write(const uint8_t *data, uint16_t len);
void bt_get_signal_strength(int8_t *rssi);
void bt_set_classic_enabled(bool enabled);
// Explicit user selection of Auto/DS5 resumes automatic discovery without
// deleting the bond. Internal profile arbitration preserves manual disconnect.
void bt_resume_auto();
void bt_cancel_manual_request();
bool bt_classic_connected();
void bt_tick();
void bt_pair();
void bt_reconnect();
bool bt_disconnect();
void bt_forget();
bool bt_has_saved_peer();
bool bt_pairing_active();
std::vector<uint8_t> get_feature_data(uint8_t reportId,uint16_t len);
void init_feature();
void set_feature_data(uint8_t reportId, uint8_t* data,uint16_t len);

#endif //DS5_BRIDGE_BT_H
