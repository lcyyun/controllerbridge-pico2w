#include <cassert>
#include <cstdio>
#include <cstring>
#include "bridge_features.h"

static int16_t read16(const uint8_t *p) {
    return static_cast<int16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}
int main() {
    uint8_t buffer[65];
    std::memset(buffer, 0xa5, sizeof(buffer));
    assert(!bridge_features_get_fallback(0x70, buffer, 64));
    assert(!bridge_features_get_fallback(0x05, nullptr, 40));
    assert(!bridge_features_get_fallback(0x05, buffer, 0));
    assert(bridge_features_get_fallback(0x05, buffer + 1, 40) == 40);
    assert(buffer[0] == 0xa5 && buffer[41] == 0xa5);
    // Every calibration axis must have distinct endpoints: zero/zero causes
    // division by zero in Sony host sensor normalisation.
    for (int axis = 0; axis < 3; ++axis) {
        assert(read16(buffer + 1 + 6 + axis * 4) > read16(buffer + 1 + 8 + axis * 4));
        assert(read16(buffer + 1 + 22 + axis * 4) > read16(buffer + 1 + 24 + axis * 4));
    }
    assert(bridge_features_get_fallback(0x09, buffer, 19) == 19 && (buffer[0] & 2));
    assert(bridge_features_get_fallback(0x20, buffer, 63) == 63 && buffer[24] == 2);
    std::memset(buffer, 0xa5, sizeof(buffer));
    assert(bridge_features_get_fallback(0x05, buffer, 7) == 7 && buffer[7] == 0xa5);
    std::puts("PASS: fallback feature bounds, nondegenerate calibration, unsupported Edge features");
}
