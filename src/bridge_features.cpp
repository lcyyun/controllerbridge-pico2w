// SPDX-License-Identifier: MIT
// Local compatibility calibration follows ControllerBridge BL616's
// bl616_ds5_feature_build_fallback (see LICENSES/BL616-protocol-MIT.txt).
#include "bridge_features.h"
#include <cstring>

namespace {
void put16(uint8_t *out, int16_t value) {
    const auto raw = static_cast<uint16_t>(value);
    out[0] = static_cast<uint8_t>(raw);
    out[1] = static_cast<uint8_t>(raw >> 8);
}
}

size_t bridge_features_get_fallback(uint8_t report_id, uint8_t *body, size_t capacity) {
    if (!body || !capacity) return 0;
    uint8_t report[63]{};
    size_t length;
    switch (report_id) {
        case 0x05: {
            length = 40;
            constexpr int16_t calibration[] = {
                0, 0, 0, // gyro biases
                8192, -8192, 8192, -8192, 8192, -8192,
                1024, -1024, // gyro speeds
                8192, -8192, 8192, -8192, 8192, -8192,
            };
            for (size_t i = 0; i < sizeof(calibration) / sizeof(calibration[0]); ++i)
                put16(report + i * 2, calibration[i]);
            break;
        }
        case 0x09:
            length = 19;
            // Locally administered compatibility address, not a claim about
            // the physical NS2Pro radio address or pairing data.
            report[0] = 0x02; report[1] = 0x50; report[2] = 0xcb;
            report[3] = 0x23; report[4] = 0x50; report[5] = 0x01;
            break;
        case 0x20:
            length = 63;
            report[23] = 0; report[24] = 2;
            report[27] = 0x56; report[28] = 2;
            report[43] = 0x21; report[44] = 2;
            break;
        default:
            return 0; // Unsupported Edge profiles/features must STALL.
    }
    if (length > capacity) length = capacity;
    std::memcpy(body, report, length);
    return length;
}
