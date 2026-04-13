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

void VideoSender::prepare_frame(const uint8_t* data, size_t data_len,
                                uint16_t frame_seq, uint32_t timestamp,
                                bool keyframe)
{
    prepared_wires_.clear();

    // Flush any in-progress FEC group before a keyframe so that the
    // keyframe starts a fresh group (with potentially lower K).
    if (keyframe) {
        std::vector<uint8_t> fec_wire;
        if (fec_encoder_.flush(frame_seq, timestamp, fec_wire)) {
            prepared_wires_.push_back(std::move(fec_wire));
        }
    }

    auto packets = fragmenter_.fragment(data, data_len, frame_seq, timestamp, keyframe);

    uint16_t frag_idx = 0;
    for (auto& pkt : packets) {
        auto wire = pkt.serialize();

        // Store in retransmit buffer.
        uint32_t key = retx_key(frame_seq, frag_idx);
        store_retx(key, wire);  // wire is copied here

        prepared_wires_.push_back(wire);

        // Feed to FEC encoder — may produce a parity packet.
        std::vector<uint8_t> fec_wire;
        const auto& retx_wire = retx_index_[key];
        if (fec_encoder_.feed(retx_wire.data(), retx_wire.size(),
                              frame_seq, timestamp, fec_wire)) {
            prepared_wires_.push_back(std::move(fec_wire));
        }

        frag_idx++;
    }

    // Flush FEC group at end of keyframe for tighter protection.
    if (keyframe) {
        std::vector<uint8_t> fec_wire;
        if (fec_encoder_.flush(frame_seq, timestamp, fec_wire)) {
            prepared_wires_.push_back(std::move(fec_wire));
        }
    }

    packets_sent_ += static_cast<uint64_t>(frag_idx);
}

int VideoSender::send_prepared(const net::SocketAddr& dest) {
    int sent = 0;
    for (const auto& wire : prepared_wires_) {
        int r = socket_.send_to(wire.data(), wire.size(), dest);
        if (r < 0) {
            log::error("VideoSender", "send_to failed at packet %d/%zu", sent, prepared_wires_.size());
            return -1;
        }
        bytes_sent_ += r;
        sent++;
    }
    return sent;
}

int VideoSender::send_frame(const uint8_t* data, size_t data_len,
                            uint16_t frame_seq, uint32_t timestamp,
                            bool keyframe, const net::SocketAddr& dest)
{
    prepare_frame(data, data_len, frame_seq, timestamp, keyframe);
    return send_prepared(dest);
}

int VideoSender::handle_nack(uint16_t seq_no, const uint16_t* frag_indices, size_t count,
                             const net::SocketAddr& dest)
{
    int resent = 0;
    for (size_t i = 0; i < count; ++i) {
        if (retx_budget_ <= 0) break;

        uint32_t key = retx_key(seq_no, frag_indices[i]);
        auto it = retx_index_.find(key);
        if (it == retx_index_.end()) continue;

        // Mark the wire as a retransmission so the receiver's FEC EWMA
        // can distinguish it from an original transmission.
        if (it->second.size() > 7) {
            it->second[7] |= protocol::FLAG_RETX;
        }

        int r = socket_.send_to(it->second.data(), it->second.size(), dest);
        if (r < 0) continue;
        bytes_sent_ += r;
        packets_sent_++;
        retransmits_++;
        retx_budget_--;
        resent++;
    }
    return resent;
}

void VideoSender::update_fec_from_loss(float loss_rate) {
    last_loss_rate_ = loss_rate;

    if (cooldown_ > 0) {
        --cooldown_;
        return;
    }

    uint8_t target_k;
    if (loss_rate < 0.02f)       target_k = 10;
    else if (loss_rate < 0.04f)  target_k = 5;
    else                         target_k = 3;

    uint8_t current_k = fec_encoder_.group_size();

    if (target_k < current_k) {
        log::info("FEC", "Adaptive K: %d -> %d (loss=%.1f%%, tighten)",
                  current_k, target_k, loss_rate * 100.0f);
        fec_encoder_.set_group_size(target_k);
        cooldown_ = TIGHTEN_COOLDOWN;
        pending_k_ = target_k;
        pending_k_count_ = 0;
        return;
    }

    if (rtt_locked_k3_ && target_k > current_k) return;

    if (target_k > current_k) {
        ++pending_k_count_;

        if (pending_k_count_ >= HYSTERESIS_UP) {
            uint8_t next_k = next_relax_step(current_k);
            log::info("FEC", "Adaptive K: %d -> %d (loss=%.1f%%, relax step)",
                      current_k, next_k, loss_rate * 100.0f);
            fec_encoder_.set_group_size(next_k);
            cooldown_ = RELAX_COOLDOWN;
            pending_k_count_ = 0;
        }
    }
}

void VideoSender::on_rtt(double rtt_ms) {
    if (rtt_ms > RTT_SPIKE_MS && !rtt_locked_k3_) {
        pre_lock_k_ = fec_encoder_.group_size();
        fec_encoder_.set_group_size(3);
        rtt_locked_k3_ = true;
        rtt_normal_count_ = 0;
        cooldown_ = TIGHTEN_COOLDOWN;
        log::info("FEC", "RTT spike %.0fms -> force K=3 (was %d)", rtt_ms, pre_lock_k_);
    } else if (rtt_locked_k3_) {
        if (rtt_ms < RTT_NORMAL_MS) {
            if (++rtt_normal_count_ >= RTT_NORMAL_CYCLES) {
                rtt_locked_k3_ = false;
                rtt_normal_count_ = 0;
                log::info("FEC", "RTT stable %.0fms -> unlock K (was locked at 3)", rtt_ms);
            }
        } else {
            rtt_normal_count_ = 0;
        }
    }
}

} // namespace deskbeam::host
