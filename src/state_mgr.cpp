// SPDX-License-Identifier: MIT
// Native Sony output-state merge. Audio fields use independent validity bits,
// matching ControllerBridge BL616's bl616_ds5_output_state reference.
#include "state_mgr.h"
#include <cstring>

static constexpr uint8_t state_init_data[63] = {
    0xfd, 0xf7, 0, 0, 0x7f, 0x64, 0xff, 0x09, 0, 0x0f,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0x0a, 0x07, 0, 0, 0x02, 0x01,
    0, 0xff, 0xd7, 0,
};
static uint8_t state[63]{};

void state_init() { std::memcpy(state, state_init_data, sizeof(state)); }

void state_set(uint8_t *data, uint8_t size) {
    if (!data) return;
    if (size > sizeof(state)) size = sizeof(state);
    std::memcpy(data, state, size);
}

void state_update(const uint8_t *data, uint8_t size) {
    // DualSense SetState body is 47 bytes; Edge supplies a longer compatible
    // body. A short packet cannot partially overwrite a previous valid state.
    if (!data || size < 47) return;
    state[0] = static_cast<uint8_t>((state[0] & ~3u) | (data[0] & 3u));
    state[38] = static_cast<uint8_t>((state[38] & ~4u) | (data[38] & 4u));
    if (data[0] & 3) std::memcpy(state + 2, data + 2, 2); // motors
    if (data[0] & 0x10) state[4] = data[4]; // headphone volume
    if (data[0] & 0x20) state[5] = data[5]; // speaker volume
    if (data[0] & 0x40) state[6] = data[6]; // microphone volume
    if (data[0] & 0x80) state[7] = data[7]; // audio routing / mic selection
    if (data[1] & 0x01) state[8] = data[8]; // mute LED
    if (data[1] & 0x02) state[9] = data[9]; // independent audio mute/power flags
    if (data[1] & 0x80) state[37] = data[37]; // audio gain / beamforming
    if (data[0] & 0x04) std::memcpy(state + 10, data + 10, 11); // right trigger
    if (data[0] & 0x08) std::memcpy(state + 21, data + 21, 11); // left trigger
    if (data[38] & 0x02) state[41] = data[41];
    if (data[38] & 0x01) state[42] = data[42];
    if (data[1] & 0x10) state[43] = data[43];
    if (data[1] & 0x04) std::memcpy(state + 44, data + 44, 3);
}
