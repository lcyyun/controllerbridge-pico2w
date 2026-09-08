#ifndef DS5_BRIDGE_AUTO_PROFILE_H
#define DS5_BRIDGE_AUTO_PROFILE_H

#include <cstdint>

#include "profile.h"
#include "ns2/ns2_input.h"

void auto_profile_init();
void auto_profile_reset_detection(const char *reason);
bool auto_profile_note_ds5_input(uint16_t len);
bool auto_profile_note_ns2_input(const Ns2InputSnapshot *snapshot);
bool auto_profile_locked();
BridgeProfileId auto_profile_active();
const char *auto_profile_active_name();
uint32_t auto_profile_ds5_reports();
uint32_t auto_profile_ns2_reports();

#endif // DS5_BRIDGE_AUTO_PROFILE_H
