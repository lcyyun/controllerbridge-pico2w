// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstring>

// BLE HD payloads contain two independent frequency/tone/amplitude bands.
// The overlap formula follows the BL616 mixer; ordinary HD frequencies and
// tone bits survive whenever that band has nonzero ordinary amplitude.
class BridgeNs2AudioMixer {
    uint8_t left_[5]{}, right_[5]{};
    uint64_t until_us_ = 0;
    static uint64_t unpack(const uint8_t data[5]) {
        uint64_t value = 0;
        for (unsigned i = 0; i < 5; ++i) value |= uint64_t(data[i]) << (8 * i);
        return value;
    }
    static void pack(uint64_t value, uint8_t out[5]) {
        for (unsigned i = 0; i < 5; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
    static bool nonzero(const uint8_t data[5]) {
        return (unpack(data) & ((uint64_t(1023) << 10) | (uint64_t(1023) << 30))) != 0;
    }
    static void mix(const uint8_t ordinary[5], const uint8_t audio[5], uint8_t out[5]) {
        if (!nonzero(audio)) { std::memcpy(out, ordinary, 5); return; }
        const uint64_t base = unpack(ordinary), overlay = unpack(audio);
        uint64_t value = 0;
        for (unsigned shift = 0; shift <= 20; shift += 20) {
            const uint32_t a = (base >> (shift + 10)) & 1023;
            const uint32_t b = (overlay >> (shift + 10)) & 1023;
            const uint32_t amplitude = a + b - (a * b + 511) / 1023;
            const uint64_t frequency_tone = ((a ? base : overlay) >> shift) & 1023;
            value |= (frequency_tone | (uint64_t(amplitude) << 10)) << shift;
        }
        pack(value, out);
    }
public:
    bool active = false;
    uint32_t updates = 0, stops = 0, mixed_ticks = 0;
    static void neutral(uint8_t out[5]) { pack(uint64_t(0xe1) | (uint64_t(0x1e1) << 20), out); }
    bool stop() {
        if (!active) return false;
        active = false; until_us_ = 0; ++stops;
        return true;
    }
    bool expire(uint64_t now) { return active && now >= until_us_ ? stop() : false; }
    void submit(const uint8_t left[5], const uint8_t right[5], uint64_t now) {
        if (!nonzero(left) && !nonzero(right)) { stop(); return; }
        std::memcpy(left_, left, 5); std::memcpy(right_, right, 5);
        active = true; until_us_ = now + 150000u; ++updates;
    }
    // Called once per outgoing scheduler tick, never for status snapshots.
    void compose(bool ordinary_active, const uint8_t ordinary_left[5], const uint8_t ordinary_right[5],
                 uint8_t left[5], uint8_t right[5]) {
        if (ordinary_active && active) {
            mix(ordinary_left, left_, left); mix(ordinary_right, right_, right);
            ++mixed_ticks;
        } else if (ordinary_active) {
            std::memcpy(left, ordinary_left, 5); std::memcpy(right, ordinary_right, 5);
        } else if (active) {
            std::memcpy(left, left_, 5); std::memcpy(right, right_, 5);
        } else {
            neutral(left); neutral(right);
        }
    }
};
