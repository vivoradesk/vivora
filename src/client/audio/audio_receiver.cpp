#include "client/audio/audio_receiver.h"
#include "common/utils/log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace deskbeam::client {

using namespace deskbeam::audio;

AudioReceiver::AudioReceiver() = default;
AudioReceiver::~AudioReceiver() { stop(); }

bool AudioReceiver::start(std::unique_ptr<AudioOutput> output,
                          int jitter_target_ms) {
    if (running_.load()) return true;
    if (!output) {
        log::error("AudioRecv", "no output provided");
        return false;
    }
    output_ = std::move(output);

    if (!output_->start(TRANSPORT_SAMPLE_RATE, TRANSPORT_CHANNELS)) {
        log::error("AudioRecv", "output start failed");
        output_.reset();
        return false;
    }
    device_rate_     = output_->sample_rate();
    device_channels_ = output_->channels();

    if (!decoder_.init()) return false;
    if (!jitter_.init(FRAME_MS, jitter_target_ms, 200)) return false;

    if (!resampler_.init(TRANSPORT_CHANNELS,
                         TRANSPORT_SAMPLE_RATE,
                         static_cast<int>(device_rate_))) {
        log::error("AudioRecv", "resampler init failed");
        return false;
    }
    log::info("AudioRecv", "pipeline: 48000 / 2 -> %u Hz / %u ch, jitter=%dms",
              device_rate_, device_channels_, jitter_target_ms);

    running_.store(true);
    worker_ = std::thread(&AudioReceiver::thread_proc, this);
    return true;
}

void AudioReceiver::stop() {
    if (!running_.load() && !worker_.joinable()) return;
    running_.store(false);
    if (worker_.joinable()) worker_.join();
    if (output_) { output_->stop(); output_.reset(); }
}

void AudioReceiver::feed(uint16_t seq, const uint8_t* data, size_t len) {
    packets_received_.fetch_add(1, std::memory_order_relaxed);
    jitter_.push(seq, data, len);
}

void AudioReceiver::push_pcm(const float* samples, uint32_t frames) {
    if (!output_) return;
    // Channel convert 2ch -> device_channels_ if needed.
    if (device_channels_ == TRANSPORT_CHANNELS) {
        output_->write(samples, frames);
        return;
    }
    std::vector<float> conv(static_cast<size_t>(frames) * device_channels_);
    if (device_channels_ == 1) {
        for (uint32_t i = 0; i < frames; ++i) {
            conv[i] = 0.5f * (samples[i * 2 + 0] + samples[i * 2 + 1]);
        }
    } else {
        // >2 channels: L, R, then zeros.
        for (uint32_t i = 0; i < frames; ++i) {
            conv[i * device_channels_ + 0] = samples[i * 2 + 0];
            conv[i * device_channels_ + 1] = samples[i * 2 + 1];
            for (uint16_t c = 2; c < device_channels_; ++c) {
                conv[i * device_channels_ + c] = 0.0f;
            }
        }
    }
    output_->write(conv.data(), frames);
}

void AudioReceiver::thread_proc() {
    using clock = std::chrono::steady_clock;
    const auto tick = std::chrono::milliseconds(FRAME_MS);
    auto next = clock::now() + tick;

    std::vector<float> pcm(FRAME_SAMPLES_STEREO);
    std::vector<uint8_t> opus_payload;
    std::vector<float> out_buf;

    auto last_stats = clock::now();
    uint64_t prev_recv = 0, prev_dec = 0, prev_plc = 0, prev_empty = 0;

    while (running_.load()) {
        std::this_thread::sleep_until(next);
        next += tick;

        uint16_t seq = 0;
        JitterBuffer::Status st = jitter_.pop(opus_payload, seq);
        int decoded = 0;
        if (st == JitterBuffer::Status::Data) {
            decoded = decoder_.decode(opus_payload.data(),
                                      static_cast<int>(opus_payload.size()),
                                      pcm.data(), false);
            packets_decoded_.fetch_add(1, std::memory_order_relaxed);
        } else if (st == JitterBuffer::Status::Missing) {
            // PLC: pass nullptr to get Opus's concealment.
            decoded = decoder_.decode(nullptr, 0, pcm.data(), false);
            plc_frames_.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Empty — still prebuffering. Skip this tick.
            empty_ticks_.fetch_add(1, std::memory_order_relaxed);
            // Still log stats on Empty path so we get visibility.
            auto now_s = clock::now();
            if (std::chrono::duration<double>(now_s - last_stats).count() >= 5.0) {
                uint64_t r = packets_received_.load(), d = packets_decoded_.load();
                uint64_t p = plc_frames_.load(), e = empty_ticks_.load();
                log::info("AudioRecv", "stats: recv=%llu(+%llu) dec=%llu(+%llu) plc=%llu(+%llu) empty=%llu(+%llu)",
                          (unsigned long long)r, (unsigned long long)(r - prev_recv),
                          (unsigned long long)d, (unsigned long long)(d - prev_dec),
                          (unsigned long long)p, (unsigned long long)(p - prev_plc),
                          (unsigned long long)e, (unsigned long long)(e - prev_empty));
                prev_recv = r; prev_dec = d; prev_plc = p; prev_empty = e;
                last_stats = now_s;
            }
            continue;
        }

        // Periodic stats every ~5 seconds.
        auto now_stats = clock::now();
        if (std::chrono::duration<double>(now_stats - last_stats).count() >= 5.0) {
            uint64_t r = packets_received_.load(), d = packets_decoded_.load();
            uint64_t p = plc_frames_.load(), e = empty_ticks_.load();
            log::info("AudioRecv", "stats: recv=%llu(+%llu) dec=%llu(+%llu) plc=%llu(+%llu) empty=%llu(+%llu)",
                      (unsigned long long)r, (unsigned long long)(r - prev_recv),
                      (unsigned long long)d, (unsigned long long)(d - prev_dec),
                      (unsigned long long)p, (unsigned long long)(p - prev_plc),
                      (unsigned long long)e, (unsigned long long)(e - prev_empty));
            prev_recv = r; prev_dec = d; prev_plc = p; prev_empty = e;
            last_stats = now_stats;
        }
        if (decoded <= 0) continue;

        if (resampler_.is_passthrough()) {
            push_pcm(pcm.data(), decoded);
        } else {
            size_t max_out = static_cast<size_t>(decoded) *
                             device_rate_ / TRANSPORT_SAMPLE_RATE + 64;
            out_buf.assign(max_out * TRANSPORT_CHANNELS, 0.0f);
            unsigned int in_f  = decoded;
            unsigned int out_f = static_cast<unsigned int>(max_out);
            if (resampler_.process(pcm.data(), in_f,
                                   out_buf.data(), out_f)) {
                push_pcm(out_buf.data(), out_f);
            }
        }
    }
}

} // namespace deskbeam::client
