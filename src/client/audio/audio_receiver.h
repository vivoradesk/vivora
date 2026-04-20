#pragma once

#include "common/audio/audio_codec.h"
#include "common/audio/audio_output.h"
#include "common/audio/jitter_buffer.h"
#include "common/audio/resampler.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace deskbeam::client {

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
};

} // namespace deskbeam::client
