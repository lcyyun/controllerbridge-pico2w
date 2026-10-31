#ifndef BRIDGE_AUDIO_STATS_H
#define BRIDGE_AUDIO_STATS_H

#include <atomic>
#include "audio.h"

// Used by USB ISR, Core 1 codec and the main loop. No allocation or locks in
// completion callbacks. Counters intentionally survive connection/role resets.
struct BridgeAudioCounters {
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    std::atomic<uint32_t> out_packets{0}, out_bytes{0}, out_errors{0};
    std::atomic<uint32_t> in_packets{0}, in_bytes{0}, in_errors{0};
    std::atomic<uint32_t> bt_reports{0}, bt_dropped{0}, opus_errors{0}, haptic_blocks{0};
    std::atomic<bool> speaker_open{false};

    void usb_out_complete(uint16_t bytes) {
        if (!bytes) return;
        out_packets.fetch_add(1, std::memory_order_relaxed);
        out_bytes.fetch_add(bytes, std::memory_order_relaxed);
        if (bytes % 8) out_errors.fetch_add(1, std::memory_order_relaxed);
    }
    void usb_in_complete(uint16_t bytes) {
        if (!bytes) return;
        in_packets.fetch_add(1, std::memory_order_relaxed);
        in_bytes.fetch_add(bytes, std::memory_order_relaxed);
        if (bytes % 4) in_errors.fetch_add(1, std::memory_order_relaxed);
    }
    void mic_queued(uint16_t bytes, uint16_t requested) {
        if (bytes != requested) in_errors.fetch_add(1, std::memory_order_relaxed);
    }
    void bt_queued(bool accepted) {
        if (accepted) bt_reports.fetch_add(1, std::memory_order_relaxed);
        else bt_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    AudioStats snapshot() const {
        AudioStats out{};
        out.out_packets = out_packets.load(std::memory_order_relaxed);
        out.out_bytes = out_bytes.load(std::memory_order_relaxed);
        out.out_errors = out_errors.load(std::memory_order_relaxed);
        out.in_packets = in_packets.load(std::memory_order_relaxed);
        out.in_bytes = in_bytes.load(std::memory_order_relaxed);
        out.in_errors = in_errors.load(std::memory_order_relaxed);
        out.bt_reports = bt_reports.load(std::memory_order_relaxed);
        out.bt_dropped = bt_dropped.load(std::memory_order_relaxed);
        out.opus_errors = opus_errors.load(std::memory_order_relaxed);
        out.haptic_blocks = haptic_blocks.load(std::memory_order_relaxed);
        // audio_get_stats fills NS2 fields from the motor scheduler's counters.
        out.speaker_open = speaker_open.load(std::memory_order_relaxed);
        return out;
    }
};
#endif
