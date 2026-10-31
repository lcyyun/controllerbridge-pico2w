//
// Created by awalol on 2026/3/5.
//

#include "audio.h"
#include "bt.h"
#include "resample.h"
#include "tusb.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "opus.h"
#include "utils.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "pico/util/queue.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "config.h"
#include "state_mgr.h"
#include "usb.h"
#include "bridge_output_mic.h"
#include "bridge_audio_stats.h"
#if ENABLE_AUTO_PROFILE
#include "bridge_output_haptics.h"
#include "ns2/ns2_usb.h"
#endif

#define INPUT_CHANNELS    4
#define OUTPUT_CHANNELS   2
#define SAMPLE_SIZE       64
#define REPORT_SIZE       398
#define REPORT_ID         0x36
// #define VOLUME_GAIN       2
// #define BUFFER_LENGTH     48

using std::clamp;
using std::max;

static WDL_Resampler resampler;
static uint8_t reportSeqCounter = 0;
static uint8_t packetCounter = 0;
static bool plug_headset = false;
alignas(8) static uint32_t audio_core1_stack[8192];
queue_t audio_fifo;
static uint8_t opus_buf[200];
static bool opus_data_valid = false; // protected by opus_cs
critical_section_t opus_cs;
static std::atomic<uint32_t> audio_epoch{0};
static std::atomic<bool> audio_core_ready{false};
static std::atomic<bool> audio_initialized{false};
static uint audio_buf_pos = 0;
static int haptic_buf_pos = 0;
static BridgeAudioCounters audio_stats;
#if ENABLE_AUTO_PROFILE
static BridgeHapticsEnvelope ns2_envelope;
#endif

struct audio_raw_element {
    float data[512 * 2];
    uint32_t epoch;
};

struct mic_encoded_element { uint32_t epoch; uint8_t data[71]; };
static queue_t mic_fifo;
static critical_section_t mic_cs;
static BridgeMicRing mic_ring;
static std::atomic<uint32_t> mic_epoch{0};
static std::atomic<bool> mic_host_open{false}, mic_enabled{false};
static std::atomic<uint32_t> mic_received{0}, mic_dropped{0}, mic_decode_errors{0};
static std::atomic<uint32_t> mic_gain_q12{4096};
static std::atomic<bool> mic_muted{false};
static float mic_last_volume_db = 1000.0f;
static bool mic_last_connection = false;
static uint8_t mic_state_retries = 0, mic_state_sequence = 0;
static uint64_t mic_state_due_us = 0;
static void reset_speaker_buffers();

static void reset_mic_buffers() {
    mic_epoch.fetch_add(1, std::memory_order_acq_rel);
    if (!audio_initialized) return;
    while (queue_try_remove(&mic_fifo, nullptr)) {}
    critical_section_enter_blocking(&mic_cs);
    mic_ring.reset();
    tud_audio_clear_ep_in_ff();
    critical_section_exit(&mic_cs);
}

void audio_mic_set_open(bool open) {
    mic_host_open.store(open, std::memory_order_release);
    reset_mic_buffers();
}

bool audio_bt_input(const uint8_t *packet, size_t len) {
    const uint8_t *opus = nullptr;
    if (!bridge_mic_packet(packet, len, &opus)) return false;
    ++mic_received;
    if (!opus || !audio_initialized || !mic_enabled.load(std::memory_order_acquire)) {
        ++mic_dropped; return true;
    }
    mic_encoded_element element{};
    element.epoch = mic_epoch.load(std::memory_order_acquire);
    std::memcpy(element.data, opus, sizeof(element.data));
    if (queue_is_full(&mic_fifo) && queue_try_remove(&mic_fifo, nullptr)) ++mic_dropped;
    if (!queue_try_add(&mic_fifo, &element)) ++mic_dropped;
    __sev();
    return true;
}

void audio_mic_service(bool eligible) {
    const bool connected = bt_classic_connected();
    const bool enable = eligible && connected && mic_host_open.load(std::memory_order_acquire);
    if (enable != mic_enabled.load(std::memory_order_relaxed) || (connected && !mic_last_connection)) {
        mic_enabled.store(enable, std::memory_order_release);
        reset_mic_buffers();
        mic_state_retries = 3;
        mic_state_due_us = 0;
    }
    mic_last_connection = connected;
    const float db = std::isfinite(volume[1]) ? clamp(volume[1], -96.0f, 48.0f) : 0.0f;
    if (db != mic_last_volume_db) {
        mic_gain_q12.store(static_cast<uint32_t>(powf(10.0f, db / 20.0f) * 4096.0f), std::memory_order_relaxed);
        mic_last_volume_db = db;
    }
    mic_muted.store(mute[1] != 0, std::memory_order_relaxed);
    const uint64_t now = time_us_64();
    if (connected && mic_state_retries && now >= mic_state_due_us) {
        uint8_t report[142]{};
        report[0] = 0x32; report[1] = (mic_state_sequence++ & 15) << 4;
        report[2] = 0x91; report[3] = 1; report[4] = enable ? 3 : 2;
        const bool queued = bt_try_write(report, sizeof(report));
        audio_stats.bt_queued(queued);
        if (queued) --mic_state_retries;
        mic_state_due_us = now + 250000u;
    }
}

void audio_mic_get_stats(AudioMicStats *out) {
    if (!out) return;
    out->host_open = mic_host_open.load(); out->enabled = mic_enabled.load();
    out->received = mic_received.load(); out->dropped = mic_dropped.load();
    out->decode_errors = mic_decode_errors.load();
    out->underflows = out->overflow_frames = 0;
    if (audio_initialized) {
        critical_section_enter_blocking(&mic_cs);
        out->underflows = mic_ring.underflows; out->overflow_frames = mic_ring.overflow_frames;
        critical_section_exit(&mic_cs);
    }
}

void audio_get_stats(AudioStats *out) {
    if (!out) return;
    *out = audio_stats.snapshot();
    out->mic_open = mic_host_open.load(std::memory_order_relaxed);
#if ENABLE_AUTO_PROFILE
    Ns2AudioStats ns2{};
    ns2_usb_get_audio_stats(&ns2);
    out->ns2_active = ns2.active; out->ns2_updates = ns2.updates;
    out->ns2_stops = ns2.stops; out->ns2_mixed_ticks = ns2.mixed_ticks;
#endif
}

void audio_speaker_set_open(bool open) {
    audio_stats.speaker_open.store(open, std::memory_order_relaxed);
    if (!open) reset_speaker_buffers();
}

bool tud_audio_rx_done_isr(uint8_t, uint16_t received, uint8_t, uint8_t, uint8_t) {
    audio_stats.usb_out_complete(received);
    return true;
}

bool tud_audio_tx_done_isr(uint8_t, uint16_t sent, uint8_t, uint8_t, uint8_t alternate) {
    audio_stats.usb_in_complete(sent);
    if (!alternate || !audio_initialized) return true;
    int16_t stereo[96]{};
    critical_section_enter_blocking(&mic_cs);
    mic_ring.packet(stereo, mic_gain_q12.load(std::memory_order_relaxed),
                    mic_muted.load(std::memory_order_relaxed) || !mic_enabled.load(std::memory_order_relaxed));
    critical_section_exit(&mic_cs);
    audio_stats.mic_queued(tud_audio_write(stereo, sizeof(stereo)), sizeof(stereo));
    return true;
}

static bool decode_mic_frame(OpusDecoder *decoder, uint32_t &decoder_epoch) {
    mic_encoded_element element{};
    if (!queue_try_remove(&mic_fifo, &element)) return false;
    const uint32_t current = mic_epoch.load(std::memory_order_acquire);
    if (!decoder || element.epoch != current || !mic_enabled.load(std::memory_order_acquire)) {
        ++mic_dropped; return true;
    }
    if (decoder_epoch != current) { opus_decoder_ctl(decoder, OPUS_RESET_STATE); decoder_epoch = current; }
    int16_t mono[480];
    const int decoded = opus_decode(decoder, element.data, sizeof(element.data), mono, 480, 0);
    if (decoded <= 0) { ++mic_decode_errors; ++audio_stats.opus_errors; return true; }
    critical_section_enter_blocking(&mic_cs);
    if (element.epoch == mic_epoch.load(std::memory_order_acquire) && mic_enabled.load(std::memory_order_acquire))
        mic_ring.push(mono, static_cast<size_t>(decoded));
    else ++mic_dropped;
    critical_section_exit(&mic_cs);
    return true;
}

void set_headset(bool state) {
    plug_headset = state;
}

void audio_reset_session(bool close_capture) {
    if (close_capture) audio_stats.speaker_open.store(false, std::memory_order_relaxed);
    if (close_capture) audio_mic_set_open(false); else reset_mic_buffers();
    reset_speaker_buffers();
}

static void reset_speaker_buffers() {
    audio_epoch.fetch_add(1, std::memory_order_acq_rel);
    audio_buf_pos = 0;
    haptic_buf_pos = 0;
    reportSeqCounter = 0;
    packetCounter = 0;
    plug_headset = false;
#if ENABLE_AUTO_PROFILE
    ns2_envelope.reset();
    ns2_usb_set_audio_motors(0, 0);
#endif
    if (!audio_initialized) return;
    const uint32_t irq_state = save_and_disable_interrupts();
    tud_audio_clear_ep_out_ff();
    restore_interrupts(irq_state);
    resampler.Reset();
#if !DISABLE_SPEAKER_PROC
    while (queue_try_remove(&audio_fifo, nullptr)) {}
    critical_section_enter_blocking(&opus_cs);
    std::memset(opus_buf, 0, sizeof(opus_buf));
    opus_data_valid = false;
    critical_section_exit(&opus_cs);
#endif
}

#if ENABLE_AUTO_PROFILE
void audio_loop_ns2_haptics() {
    if (!tud_audio_available()) return;
    int16_t pcm[192];
    const size_t frames = tud_audio_read(pcm, sizeof(pcm)) / (4 * sizeof(int16_t));
    uint16_t left = 0, right = 0;
    if (ns2_envelope.feed(pcm, frames, get_config().haptics_gain, left, right)) {
        ++audio_stats.haptic_blocks;
        ns2_usb_set_audio_motors(left, right);
    }
}
#endif

void audio_loop() {
    // 1. 读取 USB 音频数据
    if (!tud_audio_available()) return;

    int16_t raw[192];
    uint32_t bytes_read = tud_audio_read(raw, sizeof(raw)); // 每次读入 384 bytes
    int frames = bytes_read / (INPUT_CHANNELS * sizeof(int16_t));
    if (frames == 0) {
        return;
    }

    static float audio_buf[512 * 2];
    // 2. 从4ch中提取ch3/ch4，转换为float输入重采样器
    WDL_ResampleSample *in_buf;
    int nframes = resampler.ResamplePrepare(frames, OUTPUT_CHANNELS, &in_buf);

    const float audio_gain = mute[0] ? 0.0f : powf(10.0f, get_config().speaker_volume / 20.0f);
    const float haptics_gain = get_config().haptics_gain;
    for (int i = 0; i < nframes; i++) {
 #if !DISABLE_SPEAKER_PROC       
        audio_buf[audio_buf_pos++] = raw[i * INPUT_CHANNELS] / 32768.0f * audio_gain;
        audio_buf[audio_buf_pos++] = raw[i * INPUT_CHANNELS + 1] / 32768.0f * audio_gain;
        if (audio_buf_pos == 512 * 2) {
            static audio_raw_element element{};
            memcpy(element.data, audio_buf, 512 * 2 * 4);
            element.epoch = audio_epoch.load(std::memory_order_acquire);
            if (queue_is_full(&audio_fifo)) {
                queue_try_remove(&audio_fifo,NULL);
            }
            if (!queue_try_add(&audio_fifo, &element)) {
                printf("[Audio] Warning: audio_fifo add failed\n");
            }
            __sev();
            audio_buf_pos = 0;
        }
#endif
        in_buf[i * 2] = static_cast<WDL_ResampleSample>(clamp(raw[i * INPUT_CHANNELS + 2] / 32768.0f * haptics_gain,
                                                              -1.0f, 1.0f));
        in_buf[i * 2 + 1] = static_cast<WDL_ResampleSample>(clamp(raw[i * INPUT_CHANNELS + 3] / 32768.0f * haptics_gain,
                                                                  -1.0f, 1.0f));
    }

    // 3. 48kHz -> 3kHz 重采样
    static WDL_ResampleSample out_buf[SAMPLE_SIZE]; // 64 floats = 32帧 × 2ch
    const int out_frames = resampler.ResampleOut(out_buf, nframes, nframes / 4, OUTPUT_CHANNELS);

    static int8_t haptic_buf[SAMPLE_SIZE];

    // 4. 转换为int8并缓冲，满64字节即组包发送
    for (int i = 0; i < out_frames; i++) {
        int val_l = static_cast<int>(out_buf[i * 2] * 127.0f);
        int val_r = static_cast<int>(out_buf[i * 2 + 1] * 127.0f);
        haptic_buf[haptic_buf_pos++] = (int8_t) clamp(val_l, -128, 127); // 似乎clamp有点多余？还是以防万一吧
        haptic_buf[haptic_buf_pos++] = (int8_t) clamp(val_r, -128, 127);

        if (haptic_buf_pos != SAMPLE_SIZE) {
            continue;
        }
        ++audio_stats.haptic_blocks;
        uint8_t pkt[REPORT_SIZE]{};
        pkt[0] = REPORT_ID;
        pkt[1] = reportSeqCounter << 4;
        reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
        pkt[2] = 0x11 | 0 << 6 | 1 << 7;
        pkt[3] = 7;
        pkt[4] = mic_enabled.load(std::memory_order_relaxed) ? 0xff : 0xfe;
        const auto buf_len = get_config().audio_buffer_length;
        pkt[5] = buf_len;
        pkt[6] = buf_len;
        pkt[7] = buf_len;
        pkt[8] = buf_len; // 这 4 个字节的作用未知，调整没有效果
        pkt[9] = buf_len; // audio buffer length 只有调整这个字节生效。
        pkt[10] = packetCounter++;
        // SetStateData
        pkt[11] = 0x10 | 0 << 6 | 1 << 7;
        pkt[12] = 63;
        state_set(pkt + 13,63);
        // Haptics Audio Data
        pkt[76] = 0x12 | 0 << 6 | 1 << 7;
        pkt[77] = SAMPLE_SIZE;
        memcpy(pkt + 78, haptic_buf, SAMPLE_SIZE);
#if !DISABLE_SPEAKER_PROC
        // Speaker Audio Data
        pkt[142] = (plug_headset ? 0x16 : 0x13) | 0 << 6 | 1 << 7; // Speaker: 0x13
        // L Headset Mono: 0x14
        // L Headset R Speaker: 0x15
        // Headset: 0x16
        pkt[143] = 200;
        critical_section_enter_blocking(&opus_cs);
        const bool valid_opus = opus_data_valid;
        if (valid_opus) memcpy(pkt + 144, opus_buf, 200);
        critical_section_exit(&opus_cs);
        // Do not transmit uninitialised or failed codec output. The next
        // complete audio/haptic frame resumes once the encoder has valid data.
        if (!valid_opus) { haptic_buf_pos = 0; continue; }
#endif

        audio_stats.bt_queued(bt_try_write(pkt, sizeof(pkt)));
        haptic_buf_pos = 0;
    }
}

void audio_init() {
    resampler.SetMode(true, 0, false);
    resampler.SetRates(48000, 3000);
    resampler.SetFeedMode(true);
    resampler.Prealloc(2, 24, 6);
    queue_init(&mic_fifo, sizeof(mic_encoded_element), 4);
    critical_section_init(&mic_cs);
 #if !DISABLE_SPEAKER_PROC
    queue_init(&audio_fifo, sizeof(audio_raw_element), 2);
    critical_section_init(&opus_cs);
#endif
    multicore_launch_core1_with_stack(core1_entry, audio_core1_stack, sizeof(audio_core1_stack));
    // Boot-only rendezvous: TLV/config flash writes must never assume Core 1
    // is safe before its lockout handler has been installed.
    const uint64_t deadline = time_us_64() + 1000000u;
    while (!audio_core_ready.load(std::memory_order_acquire)) {
        if (time_us_64() >= deadline) panic("Audio core flash safety init timed out");
        sleep_ms(1);
    }
    audio_initialized = true;
}

static OpusEncoder *encoder;
static WDL_Resampler resampler_audio;

void core1_entry() {
    if (!flash_safe_execute_core_init()) panic("Audio core flash safety init failed");
    audio_core_ready.store(true, std::memory_order_release);
    int decoder_error = 0;
    OpusDecoder *mic_decoder = opus_decoder_create(48000, 1, &decoder_error);
    if (decoder_error != OPUS_OK || !mic_decoder) { ++mic_decode_errors; ++audio_stats.opus_errors; }
    uint32_t decoder_epoch = UINT32_MAX;
#if !DISABLE_SPEAKER_PROC
    int error = 0;
    encoder = opus_encoder_create(48000, 2,OPUS_APPLICATION_AUDIO, &error);
    if (error != 0 || !encoder) {
        ++audio_stats.opus_errors;
        printf("[Audio] OpusEncoder create failed\n");
    }
    if (encoder) {
        opus_encoder_ctl(encoder,OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
        opus_encoder_ctl(encoder,OPUS_SET_BITRATE(200 * 8 * 100));
        opus_encoder_ctl(encoder,OPUS_SET_VBR(false));
        opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY(0)); // max 4
    }
    resampler_audio.SetMode(true, 0, false);
    resampler_audio.SetRates(51200, 48000);
    resampler_audio.SetFeedMode(true);
    resampler_audio.Prealloc(2, 512, 480);
    uint32_t encoder_epoch = audio_epoch.load(std::memory_order_acquire);
#endif

    while (true) {
        const bool mic_work = decode_mic_frame(mic_decoder, decoder_epoch);
#if !DISABLE_SPEAKER_PROC
        static audio_raw_element audio_element{};
        if (!queue_try_remove(&audio_fifo, &audio_element)) {
            if (!mic_work) __wfe();
            continue;
        }
        if (!encoder) continue;
        const uint32_t current_epoch = audio_epoch.load(std::memory_order_acquire);
        if (audio_element.epoch != current_epoch) continue;
        if (encoder_epoch != current_epoch) {
            resampler_audio.Reset();
            opus_encoder_ctl(encoder, OPUS_RESET_STATE);
            encoder_epoch = current_epoch;
        }
        // 将 512 frames 重采样成 480 frames 以解决噪音问题。感谢 @Junhoo
        WDL_ResampleSample *in_buf;
        int nframes = resampler_audio.ResamplePrepare(512, 2, &in_buf);
        for (int i = 0; i < nframes * 2; i++) {
            in_buf[i] = audio_element.data[i];
        }
        static WDL_ResampleSample out_buf[480 * 2];
        resampler_audio.ResampleOut(out_buf, nframes, 480, 2);

        static uint8_t out[200];
        const int encoded = opus_encode_float(encoder, out_buf, 480, out, sizeof(out));
        const bool valid = encoded > 0 && encoded <= static_cast<int>(sizeof(out)) &&
            (encoded == static_cast<int>(sizeof(out)) ||
             opus_packet_pad(out, encoded, sizeof(out)) == OPUS_OK);
        critical_section_enter_blocking(&opus_cs);
        if (audio_element.epoch == audio_epoch.load(std::memory_order_acquire)) {
            opus_data_valid = valid;
            if (valid) memcpy(opus_buf, out, sizeof(opus_buf));
            else std::memset(opus_buf, 0, sizeof(opus_buf));
        }
        critical_section_exit(&opus_cs);
        if (!valid) {
            ++audio_stats.opus_errors;
            printf("[Audio] Opus encode/padding failed: %d\n", encoded);
        }
#else
        if (!mic_work) __wfe();
#endif
    }
}
