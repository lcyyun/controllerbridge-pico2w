//
// Created by awalol on 2026/3/5.
//

#ifndef DS5_BRIDGE_AUDIO_H
#define DS5_BRIDGE_AUDIO_H
#include <cstddef>
#include <cstdint>

void audio_init();
void audio_loop();
void core1_entry();
void set_headset(bool state);
// Drop queued samples and prevent in-flight Core 1 audio from a prior
// connection/USB role being replayed into the new session.
void audio_reset_session(bool close_capture = true);
bool audio_bt_input(const uint8_t *packet, size_t len);
void audio_mic_set_open(bool open);
void audio_mic_service(bool eligible);
struct AudioMicStats {
    bool host_open, enabled;
    uint32_t received, dropped, decode_errors, underflows, overflow_frames;
};
void audio_mic_get_stats(AudioMicStats *out);
// USB/codec counters are cumulative since boot; NS2 counters since scheduler
// initialization (also performed by settings reset). USB packet/byte counters
// describe completed transfers; BT reports describe accepted enqueues.
struct AudioStats {
    uint32_t out_packets, out_bytes, out_errors, in_packets, in_bytes, in_errors;
    uint32_t bt_reports, bt_dropped, opus_errors, haptic_blocks;
    uint32_t ns2_updates, ns2_stops, ns2_mixed_ticks;
    bool speaker_open, mic_open, ns2_active;
};
void audio_get_stats(AudioStats *out);
void audio_speaker_set_open(bool open);
#if ENABLE_AUTO_PROFILE
void audio_loop_ns2_haptics();
#endif

#endif //DS5_BRIDGE_AUDIO_H
