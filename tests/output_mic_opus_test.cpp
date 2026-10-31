#include <cassert>
#include <cstdio>
#include "opus.h"
#include "bridge_output_mic.h"

int main() {
    int error = 0;
    OpusEncoder *encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
    assert(encoder && error == OPUS_OK);
    OpusDecoder *decoder = opus_decoder_create(48000, 1, &error);
    assert(decoder && error == OPUS_OK);
    assert(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(56800)) == OPUS_OK);
    assert(opus_encoder_ctl(encoder, OPUS_SET_VBR(0)) == OPUS_OK);
    int16_t pcm[480], decoded[480], stereo[96];
    for (int i = 0; i < 480; ++i) pcm[i] = static_cast<int16_t>(((i % 48) - 24) * 600);
    uint8_t packet[75] = {0xa1, 0x31, 2, 0};
    int bytes = opus_encode(encoder, pcm, 480, packet + 4, 71);
    assert(bytes > 0 && bytes <= 71);
    assert(opus_packet_pad(packet + 4, bytes, 71) == OPUS_OK);
    const uint8_t *frame = nullptr;
    assert(bridge_mic_packet(packet, sizeof(packet), &frame) && frame == packet + 4);
    assert(opus_decode(decoder, frame, 71, decoded, 480, 0) == 480);
    long long energy = 0;
    for (auto sample : decoded) energy += static_cast<long long>(sample) * sample;
    assert(energy > 0);
    BridgeMicRing ring;
    ring.push(decoded, 480);
    assert(opus_decode(decoder, frame, 71, decoded, 480, 0) == 480);
    ring.push(decoded, 480);
    ring.packet(stereo, 4096, false);
    assert(ring.count() == 912);
    for (int i = 0; i < 48; ++i) assert(stereo[i * 2] == stereo[i * 2 + 1]);
    uint8_t malformed[71] = {3, 0}; // invalid code-3 Opus packet with zero frames
    assert(opus_decode(decoder, malformed, 71, decoded, 480, 0) < 0);
    assert(opus_decoder_ctl(decoder, OPUS_RESET_STATE) == OPUS_OK);
    assert(opus_decode(decoder, frame, 71, decoded, 480, 0) == 480);
    // Fixed-size radio payloads must use Opus packet padding, not zero-fill.
    assert(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(12800)) == OPUS_OK);
    bytes = opus_encode(encoder, pcm, 480, packet + 4, 71);
    assert(bytes > 0 && bytes < 71);
    assert(opus_packet_pad(packet + 4, bytes, 71) == OPUS_OK);
    assert(opus_decode(decoder, packet + 4, 71, decoded, 480, 0) == 480);
    opus_decoder_destroy(decoder); opus_encoder_destroy(encoder);
    std::puts("PASS: vendored Opus real71-byte mic framing/decode, stereo USB ring, malformed packet, decoder reset");
}
