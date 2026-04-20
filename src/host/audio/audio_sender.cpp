#include "host/audio/audio_sender.h"
#include "common/protocol/packet.h"
#include "common/utils/log.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace deskbeam::host {

using namespace deskbeam::audio;

AudioSender::AudioSender(net::IUdpSocket& socket) : socket_(socket) {}

bool AudioSender::init(int bitrate_bps) {
    if (!encoder_.init(bitrate_bps)) return false;
    accum_.clear();
    accum_.reserve(FRAME_SAMPLES_STEREO * 4);
    start_us_ = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    return true;
}

void AudioSender::add_destination(const net::SocketAddr& dest) {
    std::lock_guard<std::mutex> lk(dests_mu_);
    if (std::find(dests_.begin(), dests_.end(), dest) == dests_.end()) {
        dests_.push_back(dest);
    }
}

void AudioSender::remove_destination(const net::SocketAddr& dest) {
    std::lock_guard<std::mutex> lk(dests_mu_);
    dests_.erase(std::remove(dests_.begin(), dests_.end(), dest), dests_.end());
}

size_t AudioSender::destination_count() const {
    std::lock_guard<std::mutex> lk(dests_mu_);
    return dests_.size();
}

bool AudioSender::ensure_resampler(uint32_t src_rate, uint16_t src_channels) {
    if (src_rate_ == src_rate && src_channels_ == src_channels) return true;
    src_rate_     = src_rate;
    src_channels_ = src_channels;
    if (!resampler_.init(TRANSPORT_CHANNELS,
                         static_cast<int>(src_rate),
                         TRANSPORT_SAMPLE_RATE)) {
        log::error("AudioSend", "resampler init failed");
        return false;
    }
    log::info("AudioSend", "pipeline: %u Hz / %u ch -> 48000 / 2",
              src_rate, src_channels);
    return true;
}

void AudioSender::convert_to_stereo(const float* in, uint32_t frames,
                                    uint16_t in_channels,
                                    std::vector<float>& out) {
    out.resize(static_cast<size_t>(frames) * TRANSPORT_CHANNELS);
    if (in_channels == TRANSPORT_CHANNELS) {
        std::memcpy(out.data(), in, out.size() * sizeof(float));
    } else if (in_channels == 1) {
        for (uint32_t i = 0; i < frames; ++i) {
            out[i * 2 + 0] = in[i];
            out[i * 2 + 1] = in[i];
        }
    } else {
        // >2 channels: take first two (simple; proper downmix later).
        for (uint32_t i = 0; i < frames; ++i) {
            out[i * 2 + 0] = in[i * in_channels + 0];
            out[i * 2 + 1] = in[i * in_channels + 1];
        }
    }
}

void AudioSender::feed(const float* samples, uint32_t frames,
                       uint32_t sample_rate, uint16_t channels) {
    if (!samples || frames == 0) return;
    if (!ensure_resampler(sample_rate, channels)) return;

    convert_to_stereo(samples, frames, channels, stereo_scratch_);

    if (resampler_.is_passthrough()) {
        accum_.insert(accum_.end(),
                      stereo_scratch_.begin(), stereo_scratch_.end());
    } else {
        // Output size grows by at most out_rate/in_rate + headroom.
        size_t max_out_frames = static_cast<size_t>(frames) *
                                TRANSPORT_SAMPLE_RATE / sample_rate + 64;
        resampled_.assign(max_out_frames * TRANSPORT_CHANNELS, 0.0f);
        unsigned int in_f  = frames;
        unsigned int out_f = static_cast<unsigned int>(max_out_frames);
        if (!resampler_.process(stereo_scratch_.data(), in_f,
                                resampled_.data(), out_f)) {
            log::warn("AudioSend", "resampler.process failed");
            return;
        }
        accum_.insert(accum_.end(),
                      resampled_.begin(),
                      resampled_.begin() + out_f * TRANSPORT_CHANNELS);
    }

    while (accum_.size() >= static_cast<size_t>(FRAME_SAMPLES_STEREO)) {
        emit_packet();
        accum_.erase(accum_.begin(), accum_.begin() + FRAME_SAMPLES_STEREO);
    }

    // Periodic stats every ~5 seconds.
    feeds_called_++;
    auto now = std::chrono::steady_clock::now();
    if (last_log_time_ == std::chrono::steady_clock::time_point{})
        last_log_time_ = now;
    auto elapsed = std::chrono::duration<double>(now - last_log_time_).count();
    if (elapsed >= 5.0) {
        uint64_t delta_pkts = packets_sent_ - last_log_pkts_;
        log::info("AudioSend", "stats: feeds=%llu, sent=%llu (+%llu in %.1fs = %.0f pkt/s), dests=%zu",
                  (unsigned long long)feeds_called_,
                  (unsigned long long)packets_sent_,
                  (unsigned long long)delta_pkts, elapsed,
                  delta_pkts / elapsed,
                  destination_count());
        last_log_pkts_ = packets_sent_;
        last_log_time_ = now;
    }
}

void AudioSender::emit_packet() {
    std::vector<uint8_t> opus_pkt;
    int enc = encoder_.encode(accum_.data(), opus_pkt);
    if (enc <= 0) return;

    // Build wire packet: PacketHeader + opus payload.
    std::vector<uint8_t> wire(protocol::PacketHeader::WIRE_SIZE + opus_pkt.size());
    protocol::PacketHeader h{};
    h.type        = protocol::PacketType::Audio;
    h.seq_no      = audio_seq_++;
    h.timestamp   = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) - start_us_;
    h.flags       = protocol::FLAG_NONE;
    h.payload_len = static_cast<uint16_t>(opus_pkt.size());
    h.serialize(wire.data());
    std::memcpy(wire.data() + protocol::PacketHeader::WIRE_SIZE,
                opus_pkt.data(), opus_pkt.size());

    std::vector<net::SocketAddr> snapshot;
    {
        std::lock_guard<std::mutex> lk(dests_mu_);
        snapshot = dests_;
    }
    for (const auto& dest : snapshot) {
        int n = socket_.send_to(wire.data(), wire.size(), dest);
        if (n > 0) {
            packets_sent_++;
            bytes_sent_ += static_cast<uint64_t>(n);
        } else {
            send_fail_count_++;
            if (send_fail_count_ <= 5 || (send_fail_count_ % 100) == 0) {
                log::warn("AudioSend",
                          "send_to failed (rc=%d) to %u.%u.%u.%u:%u (fail_total=%llu)",
                          n,
                          (dest.ip >> 0) & 0xFF, (dest.ip >> 8) & 0xFF,
                          (dest.ip >> 16) & 0xFF, (dest.ip >> 24) & 0xFF,
                          dest.port,
                          (unsigned long long)send_fail_count_);
            }
        }
    }
}

} // namespace deskbeam::host
