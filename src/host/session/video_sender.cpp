// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "host/session/video_sender.h"
#include "host/session/paced_sender.h"
#include "common/net/relay_protocol.h"
#include <cstring>
#include "common/crypto/packet_crypto.h"
#include "common/utils/log.h"
#include <algorithm>
#include <chrono>

namespace vivora::host {

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
                                bool keyframe, bool fec_enabled)
{
    prepared_wires_.clear();

    if (per_frame_fec_) {
        prepare_frame_per_frame(data, data_len, frame_seq, timestamp,
                                keyframe, fec_enabled);
        return;
    }

    // Burst-resilient interleaving (VIV-82): split the frame into D groups with
    // consecutive keys, then transmit their packets round-robin so a consecutive
    // wire burst hits D groups by ~1/D each (recoverable) instead of wiping one
    // group whole.  Decoder is unchanged — it matches by key, order-independent.
    if (fec_interleave_ > 1 && fec_enabled) {
        prepare_frame_interleaved(data, data_len, frame_seq, timestamp, keyframe);
        return;
    }

    // A keyframe spans ~40 UDP fragments; losing a single FEC group worth
    // of packets stalls the stream until the next IDR retry.  Flush the
    // in-progress P-frame group at current M, then temporarily boost M
    // for the keyframe's own groups.  20-40% extra parity on the rare
    // keyframe costs almost nothing on average bitrate but buys real
    // burst-loss resilience when it matters most.
    uint8_t saved_m = 0;
    if (keyframe && fec_enabled) {
        auto fec_wires = fec_encoder_.flush(frame_seq, timestamp);
        for (auto& w : fec_wires) prepared_wires_.push_back(std::move(w));
        saved_m = fec_encoder_.parity_count();
        uint8_t kf_m = saved_m + KEYFRAME_M_BOOST;
        if (kf_m > KEYFRAME_M_MAX) kf_m = KEYFRAME_M_MAX;
        fec_encoder_.set_parity_count(kf_m);
    }

    // fec_enabled=false marks this frame as heartbeat — propagate to
    // fragmenter so each wire packet carries FLAG_HEARTBEAT and the
    // client can ignore them for adaptive-framerate metrics.
    auto packets = fragmenter_.fragment(data, data_len, frame_seq, timestamp,
                                         keyframe, /*heartbeat=*/!fec_enabled);

    uint16_t frag_idx = 0;
    for (auto& pkt : packets) {
        auto wire = pkt.serialize();

        if (fec_enabled) {
            // Feed FEC directly from the local wire — no need to bounce through
            // the retx ring and re-lookup just to get the same bytes back.
            auto fec_wires = fec_encoder_.feed(wire.data(), wire.size(),
                                               frame_seq, timestamp);
            uint32_t key = retx_key(frame_seq, frag_idx);
            store_retx(key, wire);
            prepared_wires_.push_back(std::move(wire));
            for (auto& w : fec_wires) prepared_wires_.push_back(std::move(w));
        } else {
            // No-FEC path (heartbeat): no parity, no retx ring write.
            // A lost heartbeat packet is replaced by the next one ~16ms
            // later, no recovery needed.
            prepared_wires_.push_back(std::move(wire));
        }

        frag_idx++;
    }

    // Flush FEC group at end of keyframe for tighter protection, then
    // restore the steady-state M so P-frames don't pay boosted overhead.
    if (keyframe && fec_enabled) {
        auto fec_wires = fec_encoder_.flush(frame_seq, timestamp);
        for (auto& w : fec_wires) prepared_wires_.push_back(std::move(w));
        fec_encoder_.set_parity_count(saved_m);
    }

    packets_sent_ += static_cast<uint64_t>(frag_idx);
}

void VideoSender::prepare_frame_per_frame(const uint8_t* data, size_t data_len,
                                          uint16_t frame_seq, uint32_t timestamp,
                                          bool keyframe, bool fec_enabled)
{
    auto packets = fragmenter_.fragment(data, data_len, frame_seq, timestamp,
                                        keyframe, /*heartbeat=*/!fec_enabled);
    const int N = static_cast<int>(packets.size());
    uint16_t frag_idx = 0;

    if (!fec_enabled) {
        // Heartbeat: no parity — a lost one is replaced by the next ~16ms later.
        for (auto& pkt : packets) {
            prepared_wires_.push_back(pkt.serialize());
            ++frag_idx;
        }
        packets_sent_ += static_cast<uint64_t>(frag_idx);
        return;
    }

    // Pool parity over the whole frame: M = pct% of K, so a contiguous burst is
    // covered by the frame's shared redundancy instead of overflowing one small
    // fixed-K group.  Keyframes take the max (losing one kills the whole GOP).
    // GF(256) caps a group at K+M <= 255, so split large frames into groups.
    int pct = current_fec_pct();
    if (keyframe && pct < 75) pct = 75;
    // Ranged FEC keeps the parity packet a fixed size, so K can span the whole
    // frame (pooled parity).  Cap K so K + M stays within GF(256)'s 255 shards
    // at this parity %: K*(1 + pct/100) <= 255.  Bigger frames split.
    const int MAX_K = (255 * 100) / (100 + pct);

    int idx = 0;
    while (idx < N) {
        const int K = std::min(N - idx, MAX_K);
        int M = (K * pct + 99) / 100;          // ceil(K * pct / 100)
        if (M < 1) M = 1;
        if (K + M > 255) M = 255 - K;
        fec_encoder_.set_group_size(static_cast<uint8_t>(K));
        fec_encoder_.set_parity_count(static_cast<uint8_t>(M));

        for (int j = 0; j < K; ++j) {
            auto wire = packets[idx + j].serialize();
            auto fec_wires = fec_encoder_.feed(wire.data(), wire.size(),
                                               frame_seq, timestamp);
            store_retx(retx_key(frame_seq, frag_idx), wire);
            prepared_wires_.push_back(std::move(wire));
            for (auto& w : fec_wires) prepared_wires_.push_back(std::move(w));
            ++frag_idx;
        }
        idx += K;
    }
    packets_sent_ += static_cast<uint64_t>(frag_idx);
}

void VideoSender::prepare_frame_interleaved(const uint8_t* data, size_t data_len,
                                            uint16_t frame_seq, uint32_t timestamp,
                                            bool keyframe)
{
    auto packets = fragmenter_.fragment(data, data_len, frame_seq, timestamp,
                                        keyframe, /*heartbeat=*/false);
    const int N = static_cast<int>(packets.size());
    if (N == 0) return;

    // Parity budget: same % as the non-interleaved path (keyframes take the max
    // — losing one kills the whole GOP).  Interleaving lets a LOWER M cover the
    // same burst, but we keep the % so overhead is unchanged (carve-out invariant).
    int pct = current_fec_pct();
    if (keyframe && pct < 75) pct = 75;

    // Group count: the interleave depth, more if the frame is big enough that
    // N/D would exceed the encoder's 128-shard group cap — but never so many
    // that groups go DEGENERATE.  Without the MIN_GROUP_K floor a small frame
    // (static desktop at low bitrate ≈ 6-12 packets) split into D=6 groups
    // gives K=1..2 shards each, every one with its own ceil(K*pct) parity:
    // ~100% wire overhead AND fragile groups that FAIL on a couple of stray
    // WiFi drops → adaptive M flaps at "0%" loss → the carve-out starves the
    // encoder at ~4 Mbps (seen Win host → Mac WiFi client, VIV-84).  Frames
    // too small to split get one group: a burst that covers the whole frame
    // kills it regardless of interleaving — that's frame-loss recovery's job.
    static constexpr int MIN_GROUP_K = 8;
    int G = fec_interleave_;
    const int max_groups = (N / MIN_GROUP_K) > 0 ? (N / MIN_GROUP_K) : 1;
    if (G > max_groups) G = max_groups;
    while ((N + G - 1) / G > 128) ++G;   // keep per-group K <= 128

    fec_encoder_.set_ranged(true);       // consecutive keys per group → tiny header

    struct GroupWires {
        std::vector<std::vector<uint8_t>> data;
        std::vector<std::vector<uint8_t>> parity;
    };
    std::vector<GroupWires> groups;
    groups.reserve(G);

    int idx = 0;
    for (int g = 0; g < G; ++g) {
        // Distribute the remainder across the first groups so sizes differ by ≤1.
        const int K = (N - idx) / (G - g);
        int M = (K * pct + 99) / 100;    // ceil(K * pct / 100)
        // VIV-88: the percentage collapses on tiny groups.  A near-idle desktop
        // encodes 2-3 packet frames, which stay one group (below MIN_GROUP_K),
        // and ceil(2 * 46%) = 1 — a single lost packet then kills the frame
        // while the loss EWMA reads 10%.  Parity is cheapest exactly there (the
        // whole frame is a couple of KB), so give every group a small absolute
        // floor rather than trusting the ratio.  Only bites at K <= 4; a normal
        // K=8 group already gets 2 at the ladder's 25% rung.
        if (M < MIN_PARITY_SHARDS) M = MIN_PARITY_SHARDS;
        if (K + M > 255) M = 255 - K;
        fec_encoder_.set_group_size(static_cast<uint8_t>(K));
        fec_encoder_.set_parity_count(static_cast<uint8_t>(M));

        GroupWires gw;
        gw.data.reserve(K);
        for (int j = 0; j < K; ++j) {
            const int fi = idx + j;
            auto wire = packets[fi].serialize();
            store_retx(retx_key(frame_seq, static_cast<uint16_t>(fi)), wire);
            auto fec_wires = fec_encoder_.feed(wire.data(), wire.size(),
                                               frame_seq, timestamp);
            gw.data.push_back(std::move(wire));
            for (auto& w : fec_wires) gw.parity.push_back(std::move(w));
        }
        // The group closes exactly at its K-th packet, so feed() already emitted
        // the parity; flush() as a defensive no-op in case K was clamped.
        if (gw.parity.empty()) {
            auto fw = fec_encoder_.flush(frame_seq, timestamp);
            for (auto& w : fw) gw.parity.push_back(std::move(w));
        }
        groups.push_back(std::move(gw));
        idx += K;
    }

    // Transpose: round-robin the data packets across groups (col 0 of every
    // group, then col 1, …), then the parity the same way.  Now consecutive wire
    // packets belong to different groups, so a burst spreads across all of them.
    size_t max_d = 0;
    for (auto& g : groups) max_d = std::max(max_d, g.data.size());
    for (size_t j = 0; j < max_d; ++j)
        for (auto& g : groups)
            if (j < g.data.size()) prepared_wires_.push_back(std::move(g.data[j]));

    size_t max_p = 0;
    for (auto& g : groups) max_p = std::max(max_p, g.parity.size());
    for (size_t j = 0; j < max_p; ++j)
        for (auto& g : groups)
            if (j < g.parity.size()) prepared_wires_.push_back(std::move(g.parity[j]));

    packets_sent_ += static_cast<uint64_t>(N);
}

bool VideoSender::flush_pending_fec(uint16_t frame_seq, uint32_t timestamp) {
    auto fec_wires = fec_encoder_.flush(frame_seq, timestamp);
    if (fec_wires.empty()) return false;
    prepared_wires_.clear();
    for (auto& w : fec_wires) prepared_wires_.push_back(std::move(w));
    return true;
}

int VideoSender::send_wire_range(const std::vector<std::vector<uint8_t>>& wires,
                                 size_t begin, size_t end,
                                 const net::SocketAddr& dest,
                                 crypto::CipherState* send_cs) {
    // Max sealed wire: ~1460B (FEC parity + 24B AEAD).  2048 is plenty and
    // lives on the stack so there's no allocation on the hot path.
    uint8_t sealed[2048];
    int sent = 0;
    for (size_t wi = begin; wi < end; ++wi) {
        const auto& wire = wires[wi];
        const uint8_t* out_data;
        size_t         out_len;
        if (send_cs) {
            out_len = crypto::seal_packet(wire.data(), wire.size(), *send_cs, sealed);
            if (out_len == 0) {
                log::error("VideoSender", "seal_packet failed (nonce exhausted?)");
                return -1;
            }
            out_data = sealed;
        } else {
            out_data = wire.data();
            out_len  = wire.size();
        }
        int r;
        // Wrap in DBRL only when the destination peer is the relay-routed
        // sentinel (HostSession marker for clients that arrived via the
        // relay).  Direct LAN peers go straight to dest, even when the
        // host is BIND'd at the relay for other potential clients.
        // See HostSession::transport_send for the same pattern.
        if (relay_active_ && dest == relay_addr_) {
            namespace rly = net::relay;
            uint8_t wrap[rly::MAX_DATA_PACKET];
            const size_t wn = rly::encode_data(wrap, sizeof(wrap),
                                               relay_alloc_id_, out_data, out_len);
            if (wn == 0) return -1;
            r = paced_ ? paced_->send_to(wrap, wn, relay_addr_)
                       : socket_.send_to(wrap, wn, relay_addr_);
        } else {
            r = paced_ ? paced_->send_to(out_data, out_len, dest)
                       : socket_.send_to(out_data, out_len, dest);
        }
        if (r < 0) {
            // Throttled — see PosixUdpSocket::send_to.  The socket layer
            // already logs the OS-level error rate-limited; here we just
            // surface the application-level batch hint at the same cadence
            // so logs make sense without drowning anyone out.
            static auto last_log = std::chrono::steady_clock::now() - std::chrono::seconds(2);
            static uint64_t suppressed = 0;
            const auto now = std::chrono::steady_clock::now();
            if (now - last_log >= std::chrono::seconds(1)) {
                log::error("VideoSender",
                           "send_to failed at packet %d/%zu (x%llu suppressed)",
                           sent, end,
                           static_cast<unsigned long long>(suppressed));
                last_log = now;
                suppressed = 0;
            } else {
                ++suppressed;
            }
            return -1;
        }
        bytes_sent_ += r;
        sent++;
    }
    return sent;
}

int VideoSender::send_prepared(const net::SocketAddr& dest,
                               crypto::CipherState* send_cs) {
    return send_wire_range(prepared_wires_, 0, prepared_wires_.size(),
                           dest, send_cs);
}

int VideoSender::send_frame(const uint8_t* data, size_t data_len,
                            uint16_t frame_seq, uint32_t timestamp,
                            bool keyframe, const net::SocketAddr& dest,
                            crypto::CipherState* send_cs)
{
    prepare_frame(data, data_len, frame_seq, timestamp, keyframe);
    return send_prepared(dest, send_cs);
}

int VideoSender::handle_nack(uint16_t seq_no, const uint16_t* frag_indices, size_t count,
                             const net::SocketAddr& dest,
                             crypto::CipherState* send_cs)
{
    uint8_t sealed[2048];
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

        const uint8_t* out_data;
        size_t         out_len;
        if (send_cs) {
            out_len = crypto::seal_packet(nack_send_buf_.data(), nack_send_buf_.size(),
                                          *send_cs, sealed);
            if (out_len == 0) continue;
            out_data = sealed;
        } else {
            out_data = nack_send_buf_.data();
            out_len  = nack_send_buf_.size();
        }
        int r;
        if (relay_active_ && dest == relay_addr_) {
            namespace rly = net::relay;
            uint8_t wrap[rly::MAX_DATA_PACKET];
            const size_t wn = rly::encode_data(wrap, sizeof(wrap),
                                               relay_alloc_id_, out_data, out_len);
            if (wn == 0) return -1;
            r = paced_ ? paced_->send_to(wrap, wn, relay_addr_)
                       : socket_.send_to(wrap, wn, relay_addr_);
        } else {
            r = paced_ ? paced_->send_to(out_data, out_len, dest)
                       : socket_.send_to(out_data, out_len, dest);
        }
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

void VideoSender::update_fec_from_loss(float loss_rate, uint32_t delta_failed) {
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
    // Baseline loss-rate ladder.  Stays modest (cap 4) because the
    // EWMA signal is too slow to react to bursts.  failure_driven_m_
    // below provides the fast event-driven response.
    uint8_t loss_m;
    if      (loss_rate < 0.03f) loss_m = 2;
    else if (loss_rate < 0.05f) loss_m = 3;
    else                        loss_m = 4;

    // Failure-driven: any reported FEC failure since last report bumps
    // M by 1 immediately.  CLEAN_DECAY_TICKS clean reports lower it.
    // The bitrate controller carves FEC overhead out of the wire
    // budget (host_loop applies `encoder_bps = wire * K/(K+M)`), so
    // raising M no longer increases total wire — encoder simply
    // shrinks proportionally.  This unblocks aggressive M growth on
    // bursty links.
    // Cap failure_driven_m_ based on sustained loss rate.  Two
    // experiments on 2026-05-25 showed aggressive M growth (15+) at
    // moderate loss shrinks the encoder so much (≤33% wire) that the
    // picture becomes a slideshow — and audio still glitches because
    // Opus uses in-band FEC, not Reed-Solomon, so the host's M ladder
    // doesn't help audio at all.  Settled on a modest ladder that
    // keeps the encoder fat enough for a usable picture; recovery at
    // 50%+ loss is *fundamentally* limited by UDP+RS math, not by
    // how high we let M climb.
    uint8_t failure_cap;
    if      (loss_rate < 0.10f) failure_cap = 7;   // ~58% encoder wire
    else if (loss_rate < 0.30f) failure_cap = 9;   // ~53%
    else if (loss_rate < 0.50f) failure_cap = 12;  // ~45%
    else                        failure_cap = 16;  // ~38% (still readable)

    if (delta_failed > 0) {
        // Heavier bumps for severe bursts (3+ fails in one 500ms window
        // typically means the link is degrading, not a stray drop).  A
        // single fail still bumps by 1; 3+ bumps by 2 to react faster.
        uint8_t bump = (delta_failed >= 3) ? 2 : 1;
        if (failure_driven_m_ + bump > failure_cap)
            failure_driven_m_ = failure_cap;
        else
            failure_driven_m_ += bump;
        clean_streak_ = 0;
        ever_failed_   = true;
    } else {
        if (++clean_streak_ >= CLEAN_DECAY_TICKS) {
            // Sticky floor: never decay below 1 once any failure has
            // ever occurred.  The link has demonstrated it can burst —
            // keeping a permanent +1 over the loss-EWMA baseline (so
            // effective M >= 3 instead of 2) is cheap (10% extra wire)
            // and stops the "flat-footed at M=2" failure pattern.
            uint8_t floor = ever_failed_ ? 1 : 0;
            if (failure_driven_m_ > floor) --failure_driven_m_;
            clean_streak_ = 0;
        }
    }
    // Also clamp DOWN to the loss-rate-derived cap — if the link
    // recovered enough that loss_rate dropped, immediately allow
    // M to settle to a lower headroom rather than waiting on the
    // slow CLEAN_DECAY_TICKS decay.
    if (failure_driven_m_ > failure_cap)
        failure_driven_m_ = failure_cap;

    uint8_t target_m = std::max(loss_m, failure_driven_m_);
    if (target_m > FAILURE_DRIVEN_M_MAX) target_m = FAILURE_DRIVEN_M_MAX;

    // Cap FEC overhead as a % of the data group.  Without this the legacy path
    // (100*M/K, uncapped) let M ramp to 20-30 = 200-300% overhead, carving the
    // encoder down to a third of the wire (4M video on a 12M wire) — and it
    // STILL didn't stop the bursts.  Blanket parity past ~50% is wasteful;
    // bigger bursts are NACK's job.  Tunable via VIVORA_FEC_MAX_PCT (VIV-82).
    static const int max_overhead_pct = [] {
        const char* e = std::getenv("VIVORA_FEC_MAX_PCT");
        int v = e ? std::atoi(e) : 75;  // 75% (was 50): NACK is too slow at 60fps
                                        // to cover bursts, so let FEC parity go
                                        // higher.  The bitrate carve-out already
                                        // shrinks the encoder to hold the wire
                                        // budget constant, so this trades video
                                        // detail (not extra wire) for burst
                                        // coverage (VIV-82).
        return v < 10 ? 10 : (v > 200 ? 200 : v);
    }();
    const uint8_t k = fec_encoder_.group_size();
    if (k > 0) {
        int cap = k * max_overhead_pct / 100;
        if (cap < 1) cap = 1;
        if (target_m > cap) target_m = static_cast<uint8_t>(cap);
    }

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

void VideoSender::set_relay_active(const net::SocketAddr& relay_addr,
                                   const uint8_t alloc_id[8]) {
    relay_addr_   = relay_addr;
    std::memcpy(relay_alloc_id_, alloc_id, 8);
    relay_active_ = true;
}

} // namespace vivora::host
