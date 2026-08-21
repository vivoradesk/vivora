// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

namespace vivora::audio {

// Fixed transport parameters across the whole pipeline. Resampler on both
// sides converts device-native rates to/from these.
inline constexpr int TRANSPORT_SAMPLE_RATE = 48000;
inline constexpr int TRANSPORT_CHANNELS    = 2;
inline constexpr int FRAME_MS              = 10;
inline constexpr int FRAME_SAMPLES         = TRANSPORT_SAMPLE_RATE * FRAME_MS / 1000; // 480
inline constexpr int FRAME_SAMPLES_STEREO  = FRAME_SAMPLES * TRANSPORT_CHANNELS;      // 960
inline constexpr int MAX_PACKET_BYTES      = 1275; // Opus hard max per RFC 6716

class OpusAudioEncoder {
public:
    OpusAudioEncoder();
    ~OpusAudioEncoder();
    OpusAudioEncoder(const OpusAudioEncoder&) = delete;
    OpusAudioEncoder& operator=(const OpusAudioEncoder&) = delete;

    // bitrate_bps: e.g. 128000. Call after construction or to change on the fly.
    bool init(int bitrate_bps);
    void set_bitrate(int bitrate_bps);

    // pcm: interleaved float [-1..1], length must be FRAME_SAMPLES_STEREO.
    // Returns bytes written to out, or -1 on error.
    int encode(const float* pcm, std::vector<uint8_t>& out);

private:
    OpusEncoder* enc_ = nullptr;
};

class OpusAudioDecoder {
public:
    OpusAudioDecoder();
    ~OpusAudioDecoder();
    OpusAudioDecoder(const OpusAudioDecoder&) = delete;
    OpusAudioDecoder& operator=(const OpusAudioDecoder&) = delete;

    bool init();

    // data==nullptr triggers PLC (packet loss concealment). Writes exactly
    // FRAME_SAMPLES_STEREO floats into pcm_out.
    // fec=true decodes the in-band FEC copy embedded in the *next* packet
    // (passed via `data`) to reconstruct a previously lost frame.
    // Returns samples_per_channel written, or -1 on error.
    int decode(const uint8_t* data, int len, float* pcm_out, bool fec = false);

private:
    OpusDecoder* dec_ = nullptr;
};

} // namespace vivora::audio
