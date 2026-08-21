// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "common/audio/audio_codec.h"
#include "common/audio/audio_output.h"
#include "common/audio/jitter_buffer.h"
#include "common/audio/resampler.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace vivora::client {

// Consumes PacketType::Audio wire packets, runs them through a jitter buffer,
// Opus-decodes at frame cadence, resamples to device rate, and pushes to the
// AudioOutput sink. Owns a dedicated decode/playback thread.
class AudioReceiver {
public:
    AudioReceiver();
    ~AudioReceiver();

    bool start(std::unique_ptr<audio::AudioOutput> output,
               int jitter_target_ms = 30);
    void stop();

    // Called from the network thread when a PacketType::Audio packet arrives.
    // `data` points to the Opus payload (packet payload, not the full wire).
    void feed(uint16_t seq, const uint8_t* data, size_t len);

    uint64_t packets_received() const { return packets_received_; }
    uint64_t packets_decoded()  const { return packets_decoded_;  }
    uint64_t plc_frames()       const { return plc_frames_;       }
    uint64_t fec_recovered()    const { return fec_recovered_;    }

    // In-stream menu controls (VIV-74).  Applied as a linear gain to the
    // decoded PCM just before it hits the output sink.  Thread-safe: set
    // from the UI thread, read on the audio worker thread.
    void  set_volume(float v);     // linear gain, clamped to [0,1]
    void  set_muted(bool m);
    float volume() const { return volume_.load(std::memory_order_relaxed); }
    bool  muted()  const { return muted_.load(std::memory_order_relaxed); }

private:
    void thread_proc();
    void push_pcm(const float* samples, uint32_t frames);

    std::atomic<bool> running_{false};
    std::thread worker_;

    audio::JitterBuffer jitter_;
    audio::OpusAudioDecoder decoder_;
    audio::Resampler resampler_;
    std::unique_ptr<audio::AudioOutput> output_;

    uint32_t device_rate_     = 0;
    uint16_t device_channels_ = 0;

    std::atomic<uint64_t> packets_received_{0};
    std::atomic<uint64_t> packets_decoded_{0};
    std::atomic<uint64_t> plc_frames_{0};
    std::atomic<uint64_t> fec_recovered_{0};
    std::atomic<uint64_t> empty_ticks_{0};

    // Output gain (VIV-74).  1.0 = unity (the common case, fast-pathed in
    // push_pcm with no copy).  gain_buf_ is scratch reused across ticks to
    // hold the scaled samples when gain != 1.
    std::atomic<float> volume_{1.0f};
    std::atomic<bool>  muted_{false};
    std::vector<float> gain_buf_;
};

} // namespace vivora::client
