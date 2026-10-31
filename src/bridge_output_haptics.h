// SPDX-License-Identifier: MIT
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

// Sony USB audio channels 3/4 carry haptic PCM. NS2Pro exposes HD actuator
// commands, not the Sony waveform transport. Preserve independent left/right
// energy over 10 ms; channels 1/2 (speaker) never turn into controller rumble.
class BridgeHapticsEnvelope {
    uint64_t left_energy_ = 0, right_energy_ = 0;
    uint16_t frames_ = 0;
    static uint16_t amplitude(uint64_t energy, float gain) {
        const double rms = std::sqrt(static_cast<double>(energy) / 480.0);
        const double value = rms * (65535.0 / 32768.0) * gain;
        return value >= 65535.0 ? 65535 : static_cast<uint16_t>(value + 0.5);
    }
public:
    void reset() { left_energy_ = right_energy_ = 0; frames_ = 0; }
    bool feed(const int16_t *pcm, size_t frames, float gain, uint16_t &left, uint16_t &right) {
        if (!pcm || !std::isfinite(gain) || gain < 0) return false;
        bool ready = false;
        for (size_t i = 0; i < frames; ++i) {
            const int64_t l = pcm[i * 4 + 2], r = pcm[i * 4 + 3];
            left_energy_ += static_cast<uint64_t>(l * l);
            right_energy_ += static_cast<uint64_t>(r * r);
            if (++frames_ == 480) {
                left = amplitude(left_energy_, gain);
                right = amplitude(right_energy_, gain);
                reset(); ready = true;
            }
        }
        return ready;
    }
};
