#include <cassert>
#include <cstdio>
#include "bridge_output_mic.h"
#include "bridge_audio_stats.h"

int main() {
    uint8_t packet[79]{};
    const uint8_t *opus = nullptr;
    packet[0] = 0xa1; packet[1] = 0x31; packet[2] = 0;
    assert(!bridge_mic_packet(packet, sizeof(packet), &opus));
    packet[2] = 2;
    assert(bridge_mic_packet(packet, 74, &opus) && !opus); // consumed, malformed
    assert(bridge_mic_packet(packet, 75, &opus) && opus == packet + 4);
    packet[0] = 0xa3;
    assert(!bridge_mic_packet(packet, 75, &opus) && !opus);
    assert(!bridge_mic_packet(nullptr, 75, &opus));

    BridgeMicRing ring;
    int16_t mono[480], stereo[96];
    for (int i = 0; i < 480; ++i) mono[i] = static_cast<int16_t>(i - 240);
    ring.push(mono, 480);
    ring.packet(stereo, 4096, false);
    assert(ring.count() == 480 && stereo[0] == 0 && stereo[95] == 0); // prime20ms
    ring.push(mono, 480);
    ring.packet(stereo, 4096, false);
    assert(ring.count() == 912 && stereo[0] == -240 && stereo[1] == -240);
    assert(stereo[94] == -193 && stereo[95] == -193);
    ring.packet(stereo, 4096, true);
    assert(ring.count() == 864 && stereo[0] == 0 && stereo[95] == 0);
    for (int i = 0; i < 18; ++i) ring.packet(stereo, 4096, false);
    assert(ring.count() == 0 && ring.underflows == 0);
    ring.packet(stereo, 4096, false);
    assert(ring.underflows == 1 && stereo[0] == 0 && stereo[95] == 0);
    ring.push(mono, 480); ring.packet(stereo, 4096, false);
    assert(ring.count() == 480); // underflow re-primes
    ring.reset();
    for (int i = 0; i < 480; ++i) mono[i] = i & 1 ? -32768 : 32767;
    for (int i = 0; i < 5; ++i) ring.push(mono, 480);
    assert(ring.count() == 1920 && ring.overflow_frames == 480);
    ring.packet(stereo, 1048576, false);
    assert(stereo[0] == 32767 && stereo[2] == -32768);
    ring.reset(); ring.packet(stereo, 4096, false);
    assert(stereo[0] == 0 && ring.count() == 0);

    BridgeAudioCounters counters;
    counters.usb_out_complete(0); counters.usb_in_complete(0);
    counters.mic_queued(192, 192);
    auto stats = counters.snapshot();
    assert(stats.in_packets == 0 && stats.in_bytes == 0 && stats.in_errors == 0);
    counters.usb_out_complete(384); counters.usb_out_complete(7);
    counters.usb_in_complete(192); counters.usb_in_complete(3);
    counters.mic_queued(96, 192);
    counters.bt_queued(true); counters.bt_queued(false);
    stats = counters.snapshot();
    assert(stats.out_packets == 2 && stats.out_bytes == 391 && stats.out_errors == 1);
    assert(stats.in_packets == 2 && stats.in_bytes == 195 && stats.in_errors == 2);
    assert(stats.bt_reports == 1 && stats.bt_dropped == 1);
    assert(stats.haptic_blocks == 0 && stats.opus_errors == 0);
    std::puts("PASS: DS5 mic framing/ring/session reset; completed USB bytes and enqueue/error counters");
}
