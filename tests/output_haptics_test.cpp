#include <cassert>
#include <cstdio>
#include <limits>
#include "bridge_output_haptics.h"
#include "bridge_ns2_audio_mixer.h"

static void hd(uint16_t low, uint16_t high, uint16_t low_amp, uint16_t high_amp, uint8_t out[5]) {
    const uint64_t bits = low | (uint64_t(low_amp) << 10) | (uint64_t(high) << 20) | (uint64_t(high_amp) << 30);
    for (unsigned i = 0; i < 5; ++i) out[i] = static_cast<uint8_t>(bits >> (8 * i));
}
static uint64_t hd_bits(const uint8_t data[5]) {
    uint64_t bits = 0;
    for (unsigned i = 0; i < 5; ++i) bits |= uint64_t(data[i]) << (8 * i);
    return bits;
}

int main() {
    BridgeHapticsEnvelope envelope;
    int16_t pcm[48 * 4]{};
    uint16_t left = 123, right = 456;
    // Max speaker audio must never be interpreted as haptic input.
    for (int i = 0; i < 48; ++i) pcm[i * 4] = pcm[i * 4 + 1] = 32767;
    for (int i = 0; i < 9; ++i) assert(!envelope.feed(pcm, 48, 1, left, right));
    assert(envelope.feed(pcm, 48, 1, left, right) && left == 0 && right == 0);
    for (int i = 0; i < 48; ++i) { pcm[i * 4 + 2] = -16384; pcm[i * 4 + 3] = -32768; }
    for (int i = 0; i < 10; ++i) envelope.feed(pcm, 48, 1, left, right);
    assert(left == 32768 && right == 65535);
    envelope.reset();
    for (int i = 0; i < 10; ++i) envelope.feed(pcm, 48, 2, left, right);
    assert(left == 65535 && right == 65535);
    assert(!envelope.feed(pcm, 48, std::numeric_limits<float>::quiet_NaN(), left, right));
    assert(!envelope.feed(nullptr, 48, 1, left, right));
    envelope.reset();
    for (int i = 0; i < 5; ++i) envelope.feed(pcm, 48, 1, left, right);
    envelope.reset(); // old connection's half window must not leak
    for (int i = 0; i < 9; ++i) assert(!envelope.feed(pcm, 48, 1, left, right));
    assert(envelope.feed(pcm, 48, 1, left, right));
    BridgeNs2AudioMixer mixer;
    uint8_t ordinary[5], overlay[5], zero[5], l[5], r[5];
    hd(0x212, 0x345, 300, 700, ordinary); // frequency plus tone bit
    hd(0xe1, 0x1e1, 800, 500, overlay);
    BridgeNs2AudioMixer::neutral(zero);
    mixer.compose(true, ordinary, ordinary, l, r);
    assert(std::memcmp(l, ordinary, 5) == 0 && mixer.mixed_ticks == 0);
    mixer.submit(overlay, zero, 100);
    mixer.compose(true, ordinary, ordinary, l, r);
    assert(mixer.active && mixer.updates == 1 && mixer.mixed_ticks == 1);
    const uint64_t mixed = hd_bits(l);
    assert((mixed & 1023) == 0x212 && ((mixed >> 20) & 1023) == 0x345);
    assert(((mixed >> 10) & 1023) == 300 + 800 - (300 * 800 + 511) / 1023);
    assert(((mixed >> 30) & 1023) == 700 + 500 - (700 * 500 + 511) / 1023);
    assert(std::memcmp(r, ordinary, 5) == 0); // silent audio side preserves native bytes
    mixer.compose(false, ordinary, ordinary, l, r); // ordinary hold expired
    assert(std::memcmp(l, overlay, 5) == 0 && std::memcmp(r, zero, 5) == 0);
    assert(mixer.mixed_ticks == 1);
    mixer.submit(zero, zero, 200); // silence stops only audio
    mixer.compose(true, ordinary, ordinary, l, r);
    assert(!mixer.active && mixer.stops == 1 && std::memcmp(l, ordinary, 5) == 0);
    mixer.submit(zero, zero, 300);
    assert(mixer.stops == 1); // repeated silence never rearms stops
    mixer.submit(overlay, overlay, 400);
    assert(!mixer.expire(150399) && mixer.expire(150400));
    assert(mixer.stops == 2 && !mixer.expire(200000));
    mixer.compose(false, ordinary, ordinary, l, r);
    assert(std::memcmp(l, zero, 5) == 0 && std::memcmp(r, zero, 5) == 0);
    hd(0xe1, 0x1e1, 1023, 1023, overlay);
    mixer.submit(overlay, overlay, 500);
    mixer.compose(true, overlay, overlay, l, r);
    assert(std::memcmp(l, overlay, 5) == 0); // saturated sum stays bounded
    std::puts("PASS: PCM channel/RMS/reset; NS2 independent audio/game sources, native bits, soft mix, silence/expiry and counters");
}
