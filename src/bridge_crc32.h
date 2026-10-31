#pragma once
#include <cstddef>
#include <cstdint>

// IEEE CRC-32 (reflected polynomial). State is unfinalized so disjoint record
// fields can be checked without copying or including the checksum field.
inline uint32_t bridge_crc32_update(uint32_t state, const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    while (size--) {
        state ^= *bytes++;
        for (unsigned bit = 0; bit < 8; ++bit) {
            state = (state >> 1) ^ (0xedb88320u & (0u - (state & 1u)));
        }
    }
    return state;
}

inline uint32_t bridge_crc32(const void *data, size_t size) {
    return bridge_crc32_update(0xffffffffu, data, size) ^ 0xffffffffu;
}
