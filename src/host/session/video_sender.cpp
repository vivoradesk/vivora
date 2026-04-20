#include "host/session/video_sender.h"
#include "common/utils/log.h"
#include <algorithm>

namespace deskbeam::host {

void VideoSender::store_retx(uint32_t key, const std::vector<uint8_t>& wire) {
    auto& slot = retx_ring_[retx_write_cursor_ % RETX_BUFFER_CAPACITY];
    slot.key = key;
    slot.wire = wire;
    ++retx_write_cursor_;
}

const std::vector<uint8_t>* VideoSender::find_retx(uint32_t key) const {
    for (const auto& slot : retx_ring_) {
        if (!slot.wire.empty() && slot.key == key) return &slot.wire;
    }
    return nullptr;
}

void VideoSender::prepare_frame(const uint8_t* data, size_t data_len,
                                uint16_t frame_seq, uint32_t timestamp,
                                bool keyframe)
{
    prepared_wires_.clear();

    // A keyframe spans ~40 UDP fragments; losing a single FEC group worth
    // of packets stalls the stream until the next IDR retry.  Flush the
    // in-progress P-frame group at current M, then temporarily boost M
    // for the keyframe's own groups.  20-40% extra parity on the rare
    // keyframe costs almost nothing on average bitrate but buys real
    // burst-loss resilience when it matters most.
    uint8_t saved_m = 0;
    if (keyframe) {
        auto fec_wires = fec_encoder_.flush(frame_seq, timestamp);
        for (auto& w : fec_wires) prepared_wires_.push_back(std::move(w));
        saved_m = fec_encoder_.parity_count();
        uint8_t kf_m = saved_m + KEYFRAME_M_BOOST;
        if (kf_m > KEYFRAME_M_MAX) kf_m = KEYFRAME_M_MAX;
        fec_encoder_.set_parity_count(kf_m);
    }

    auto packets = fragmenter_.fragment(data, data_len, frame_seq, timestamp, keyframe);

    uint16_t frag_idx = 0;
    for (auto& pkt : packets) {
        auto wire = pkt.serialize();

        // Feed FEC directly from the local wire — no need to bounce through
        // the retx ring and re-lookup just to get the same bytes back.
        uint32_t key = retx_key(frame_seq, frag_idx);
        auto fec_wires = fec_encoder_.feed(wire.data(), wire.size(),
                                           frame_seq, timestamp);

        // Copy into the retx ring for future NACK service, then move the
        // original into prepared_wires_ to avoid a third copy.
        store_retx(key, wire);
        prepared_wires_.push_back(std::move(wire));
        for (auto& w : fec_wires) prepared_wires_.push_back(std::move(w));

        frag_idx++;
    }

    // Flush FEC group at end of keyframe for tighter protection, then
    // restore the steady-state M so P-frames don't pay boosted overhead.
    if (keyframe) {
        auto fec_wires = fec_encoder_.flush(frame_seq, timestamp);
        for (auto& w : fec_wires) prepared_wires_.push_back(std::move(w));
        fec_encoder_.set_parity_count(saved_m);
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
        const std::vector<uint8_t>* stored = find_retx(key);
        if (!stored) continue;

        // Send a copy with FLAG_RETX set; leave the stored wire byte
        // untouched so future NACKs for the same fragment still see a
        // clean original and so the receiver's fresh_received accounting
        // doesn't get poisoned by sticky flags.
        nack_send_buf_.assign(stored->begin(), stored->end());
        if (nack_send_buf_.size() > 7) {
            nack_send_buf_[7] |= protocol::FLAG_RETX;
        }

        int r = socket_.send_to(nack_send_buf_.data(), nack_send_buf_.size(), dest);
        if (r < 0) continue;
        bytes_sent_ += r;
        packets_sent_++;
        retransmits_++;
        retx_budget_--;
        resent++;
    }
    return resent;
}

void VideoSender::set_force_m(uint8_t m) {
    force_m_ = m;
    if (m > 0) {
        steady_m_ = m;
        rtt_locked_ = false;
        rtt_normal_count_ = 0;
        pending_relax_count_ = 0;
        cooldown_ = 0;
        fec_encoder_.set_parity_count(m);
        log::info("FEC", "Force M=%u (diagnostic, adaptive disabled)",
                  static_cast<unsigned>(m));
    } else {
        log::info("FEC", "Force M cleared, adaptive restored");
    }
}

void VideoSender::update_fec_from_loss(float loss_rate) {
    last_loss_rate_ = loss_rate;

    if (force_m_ > 0) return;

    if (cooldown_ > 0) {
        --cooldown_;
        return;
    }

    // K stays at default 10 (good batching).  M scales with observed loss:
    // each extra parity shard covers one additional burst-loss per K-group.
    // M floor = 2 on WiFi — even at 0% EWMA, burst-loss micro-events
    // cost more than the 20% parity overhead.
    uint8_t target_m;
    if      (loss_rate < 0.03f) target_m = 2;
    else if (loss_rate < 0.05f) target_m = 3;
    else                        target_m = 4;

    // Use the tracked steady-state M rather than whatever the encoder
    // currently has — keyframe boost temporarily raises the encoder's M
    // for its own groups, and the RTT lock can pin it higher still.
    uint8_t current_m = steady_m_;

    if (target_m > current_m) {
        log::info("FEC", "Adaptive M: %d -> %d (loss=%.1f%%, tighten)",
                  current_m, target_m, loss_rate * 100.0f);
        steady_m_ = target_m;
        if (!rtt_locked_) fec_encoder_.set_parity_count(target_m);
        cooldown_ = TIGHTEN_COOLDOWN;
        pending_relax_count_ = 0;
        return;
    }

    if (rtt_locked_ && target_m < current_m) {
        // Don't let relax pressure accumulate while the RTT lock is
        // holding M up — otherwise a single relax step would fire
        // immediately on unlock, undoing the protection the lock just
        // bought us.
        pending_relax_count_ = 0;
        return;
    }

    if (target_m < current_m) {
        if (++pending_relax_count_ >= HYSTERESIS_UP) {
            uint8_t next_m = current_m - 1;
            log::info("FEC", "Adaptive M: %d -> %d (loss=%.1f%%, relax step)",
                      current_m, next_m, loss_rate * 100.0f);
            steady_m_ = next_m;
            if (!rtt_locked_) fec_encoder_.set_parity_count(next_m);
            cooldown_ = RELAX_COOLDOWN;
            pending_relax_count_ = 0;
        }
    } else {
        pending_relax_count_ = 0;
    }
}

void VideoSender::on_rtt(double rtt_ms) {
    if (force_m_ > 0) return;
    if (rtt_ms > RTT_SPIKE_MS && !rtt_locked_) {
        // Snapshot the loss-driven steady-state M, not whatever the
        // encoder happens to report — the keyframe path may have it
        // transiently boosted, which would mislabel the log and, if
        // any future code uses pre_lock_m_ to restore, pin M high.
        pre_lock_m_ = steady_m_;
        if (steady_m_ < RTT_SPIKE_M) {
            fec_encoder_.set_parity_count(RTT_SPIKE_M);
        }
        rtt_locked_ = true;
        rtt_normal_count_ = 0;
        cooldown_ = TIGHTEN_COOLDOWN;
        log::info("FEC", "RTT spike %.0fms -> raise M to %d (was %d)",
                  rtt_ms, static_cast<int>(RTT_SPIKE_M), pre_lock_m_);
    } else if (rtt_locked_) {
        if (rtt_ms < RTT_NORMAL_MS) {
            if (++rtt_normal_count_ >= RTT_NORMAL_CYCLES) {
                rtt_locked_ = false;
                rtt_normal_count_ = 0;
                // On unlock, fall back to the loss-driven steady state.
                fec_encoder_.set_parity_count(steady_m_);
                log::info("FEC", "RTT stable %.0fms -> unlock M (back to %d)",
                          rtt_ms, static_cast<int>(steady_m_));
            }
        } else {
            rtt_normal_count_ = 0;
        }
    }
}

} // namespace deskbeam::host
