#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "state_mgr.h"

int main() {
    state_init();
    uint8_t baseline[63], result[80], update[63]{};
    state_set(baseline, 63);
    update[0] = 0xf0; update[1] = 0x82;
    update[4] = 12; update[5] = 34; update[6] = 56; update[7] = 0xc2;
    update[9] = 0x50; update[37] = 7;
    state_update(update, 47); state_set(result, 63);
    assert(result[4] == 12 && result[5] == 34 && result[6] == 56);
    assert(result[7] == 0xc2 && result[9] == 0x50 && result[37] == 7);
    uint8_t audio_state[63]; std::memcpy(audio_state, result, 63);
    // LED-only reports carry arbitrary/default audio bytes: no audio validity
    // bits means they must not reset routes, volume, mute or beamforming.
    std::memset(update, 0, sizeof(update)); update[1] = 4;
    update[44] = 1; update[45] = 2; update[46] = 3;
    state_update(update, 63); state_set(result, 63);
    for (int offset : {4,5,6,7,9,37}) assert(result[offset] == audio_state[offset]);
    assert(result[44] == 1 && result[45] == 2 && result[46] == 3);
    // Each of the four first-byte audio validity flags updates only its field.
    for (int field = 4; field <= 7; ++field) {
        std::memset(update, 0xee, sizeof(update));
        update[0] = static_cast<uint8_t>(1 << field); update[1] = 0; update[38] = 0;
        update[field] = static_cast<uint8_t>(field * 10);
        state_update(update, 47); state_set(result, 63);
        audio_state[field] = update[field];
        for (int offset : {4,5,6,7,9,37}) assert(result[offset] == audio_state[offset]);
    }
    for (int field : {9,37}) {
        std::memset(update, 0xee, sizeof(update));
        update[0] = 0; update[1] = field == 9 ? 2 : 0x80; update[38] = 0;
        update[field] = static_cast<uint8_t>(field + 12);
        state_update(update, 47); state_set(result, 63);
        audio_state[field] = update[field];
        for (int offset : {4,5,6,7,9,37}) assert(result[offset] == audio_state[offset]);
    }
    // Malformed inputs cannot change a prior state or leak past 63 stored bytes.
    std::memcpy(baseline, result, 63);
    state_update(update, 46); state_update(nullptr, 63);
    std::memset(result, 0xa5, sizeof(result)); state_set(result, 80);
    assert(!std::memcmp(result, baseline, 63) && result[63] == 0xa5 && result[79] == 0xa5);
    state_set(nullptr, 63);
    std::puts("PASS: native Sony independent audio validity, LED isolation, routing/mute preservation, bounds");
}
