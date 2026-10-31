// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// DS5 microphone packets share report ID 0x31 with game input but use flag
// bit 1 and a separate short frame. They must never reach the gamepad parser.
inline bool bridge_mic_packet(const uint8_t *data, size_t len, const uint8_t **opus) {
    if (opus) *opus = nullptr;
    if (!data || len < 3 || data[0] != 0xa1 || data[1] != 0x31 || !(data[2] & 2)) return false;
    if (opus && len >= 75) *opus = data + 4;
    return true;
}

// Protected externally when shared by Core 1 decoder and the USB ISR.
class BridgeMicRing {
    static constexpr size_t capacity = 1920;
    int16_t samples_[capacity]{};
    size_t head_ = 0, tail_ = 0, count_ = 0;
    bool primed_ = false;
public:
    uint32_t overflow_frames = 0, underflows = 0;
    void reset() { head_ = tail_ = count_ = 0; primed_ = false; }
    size_t count() const { return count_; }
    void push(const int16_t *mono, size_t frames) {
        if (!mono) return;
        for (size_t i = 0; i < frames; ++i) {
            if (count_ == capacity) { tail_ = (tail_ + 1) % capacity; --count_; ++overflow_frames; }
            samples_[head_] = mono[i]; head_ = (head_ + 1) % capacity; ++count_;
        }
    }
    // Always supplies a complete 1 ms stereo USB packet, including silence
    // during 20 ms priming and any underflow. Mute still consumes PCM.
    void packet(int16_t stereo[96], uint32_t gain_q12, bool muted) {
        std::memset(stereo, 0, 96 * sizeof(int16_t));
        if (!primed_) {
            if (count_ < 960) return;
            primed_ = true;
        }
        const size_t frames = count_ < 48 ? count_ : 48;
        for (size_t i = 0; i < frames; ++i) {
            int64_t sample = muted ? 0 : (static_cast<int64_t>(samples_[tail_]) * gain_q12) / 4096;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            stereo[i * 2] = stereo[i * 2 + 1] = static_cast<int16_t>(sample);
            tail_ = (tail_ + 1) % capacity;
        }
        count_ -= frames;
        if (frames < 48) { ++underflows; primed_ = false; }
    }
};
