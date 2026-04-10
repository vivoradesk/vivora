#include "host/session/video_sender.h"
#include "common/utils/log.h"
#include <algorithm>

namespace deskbeam::host {

void VideoSender::store_retx(uint32_t key, std::vector<uint8_t> wire) {
    retx_index_[key] = std::move(wire);
    retx_fifo_.push_back(key);
    while (retx_fifo_.size() > RETX_BUFFER_CAPACITY) {
        retx_index_.erase(retx_fifo_.front());
        retx_fifo_.pop_front();
    }
}

int VideoSender::send_wire(const std::vector<uint8_t>& wire,
                           const net::SocketAddr& dest) {
    int r = socket_.send_to(wire.data(), wire.size(), dest);
    if (r > 0) bytes_sent_ += r;
    return r;
}

int VideoSender::send_frame(const uint8_t* data, size_t data_len,
                            uint16_t frame_seq, uint32_t timestamp,
                            bool keyframe, const net::SocketAddr& dest)
{
    // Flush any in-progress FEC group before a keyframe so that the
    // keyframe starts a fresh group (with potentially lower K).
    if (keyframe) {
        std::vector<uint8_t> fec_wire;
        if (fec_encoder_.flush(frame_seq, timestamp, fec_wire)) {
            if (send_wire(fec_wire, dest) < 0)
                log::warn("VideoSender", "FEC flush send failed");
            else {
                packets_sent_++;
                // FEC packets not stored in retx — they're regeneratable
            }
        }
    }

    auto packets = fragmenter_.fragment(data, data_len, frame_seq, timestamp, keyframe);

    int sent = 0;
    uint16_t frag_idx = 0;
    for (auto& pkt : packets) {
        auto wire = pkt.serialize();

        if (send_wire(wire, dest) < 0) {
            log::error("VideoSender", "send_to failed at packet %d/%zu", sent, packets.size());
            return -1;
        }
        sent++;

        // Store in retransmit buffer
        uint32_t key = retx_key(frame_seq, frag_idx);
        store_retx(key, wire);  // wire is copied here, we need it for FEC below

        // Feed to FEC encoder — may produce a parity packet
        std::vector<uint8_t> fec_wire;
        const auto& retx_wire = retx_index_[key]; // use the stored copy
        if (fec_encoder_.feed(retx_wire.data(), retx_wire.size(),
                              frame_seq, timestamp, fec_wire)) {
            if (send_wire(fec_wire, dest) >= 0) {
                packets_sent_++;
                sent++;
            }
        }

        frag_idx++;
    }

    // Flush FEC group at end of keyframe for tighter protection.
    if (keyframe) {
        std::vector<uint8_t> fec_wire;
        if (fec_encoder_.flush(frame_seq, timestamp, fec_wire)) {
            if (send_wire(fec_wire, dest) >= 0) {
                packets_sent_++;
                sent++;
            }
        }
    }

    packets_sent_ += static_cast<uint64_t>(frag_idx); // data packets only
    return sent;
}

int VideoSender::handle_nack(uint16_t seq_no, const uint16_t* frag_indices, size_t count,
                             const net::SocketAddr& dest)
{
    int resent = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t key = retx_key(seq_no, frag_indices[i]);
        auto it = retx_index_.find(key);
        if (it == retx_index_.end()) continue; // aged out or never existed

        int r = socket_.send_to(it->second.data(), it->second.size(), dest);
        if (r < 0) continue;
        bytes_sent_ += r;
        packets_sent_++;
        retransmits_++;
        resent++;
    }
    return resent;
}

void VideoSender::update_fec_from_loss(float loss_rate) {
    // Map loss rate to target K
    uint8_t target_k;
    if (loss_rate < 0.01f)       target_k = 20;
    else if (loss_rate < 0.03f)  target_k = 10;
    else if (loss_rate < 0.05f)  target_k = 5;
    else                         target_k = 3;

    // Hysteresis: only change K after HYSTERESIS_COUNT consecutive same-target reports
    if (target_k == pending_k_) {
        ++pending_k_count_;
    } else {
        pending_k_ = target_k;
        pending_k_count_ = 1;
    }

    if (pending_k_count_ >= HYSTERESIS_COUNT && target_k != fec_encoder_.group_size()) {
        log::info("FEC", "Adaptive K: %d -> %d (loss=%.1f%%)",
                  fec_encoder_.group_size(), target_k, loss_rate * 100.0f);
        fec_encoder_.set_group_size(target_k);
    }
}

} // namespace deskbeam::host
