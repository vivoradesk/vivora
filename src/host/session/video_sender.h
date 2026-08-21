// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "common/crypto/noise_nk.h"
#include "common/net/socket.h"
#include "common/net/frame_fragmenter.h"
#include "common/net/fec_codec.h"
#include "common/protocol/packet.h"
#include <array>
#include <cstdint>
#include <vector>

namespace vivora::host {

class PacedSender;  // host/session/paced_sender.h (VIV-82)

// Fragments an encoded frame and sends all packets over UDP.
// Keeps a ring buffer of recently-sent fragments so they can be retransmitted
// on client NACK (selective repeat). Generates XOR FEC parity packets.
//
// Supports multi-client: prepare_frame() fragments + FEC once, then
// send_prepared() sends to each destination.
class VideoSender {
public:
    // Retransmit buffer capacity (fragments). At 60fps + IDR bursts, ~2048
    // covers roughly 500ms of history.
    static constexpr size_t RETX_BUFFER_CAPACITY = 2048;
    // Max retransmits per poll cycle. Prevents NACK storms from starving
    // the capture/encode pipeline on the single-threaded host loop.
    static constexpr int MAX_RETX_PER_POLL = 30;

    explicit VideoSender(net::IUdpSocket& socket) : socket_(socket) {}

    // Route all wire sends through a decoupled paced sender (VIV-82) instead
    // of bursting straight to the socket.  Null restores the direct path
    // (used by transport_test).
    void set_paced_sender(PacedSender* p) { paced_ = p; }

    void reset_retx_budget() { retx_budget_ = MAX_RETX_PER_POLL; }

    // Toggle relay mode: every send_prepared() / NACK retx wraps the
    // wire packet in DBRL DATA and ships it to relay_addr instead of
    // the per-client `dest`.  Set once after HostSession's BIND succeeds.
    void set_relay_active(const net::SocketAddr& relay_addr,
                          const uint8_t alloc_id[8]);
    void clear_relay() { relay_active_ = false; }

    // Prepare a frame for sending: fragment, generate FEC, store in retx buffer.
    // Call once per frame, then send_prepared() for each destination.
    // fec_enabled=false skips FEC group accumulation/parity for this frame —
    // used by static-screen heartbeat where lost packets don't need recovery
    // (next heartbeat replaces) and putting them through FEC just inflates
    // the failure counter under WiFi loss without buying anything useful.
    void prepare_frame(const uint8_t* data, size_t data_len,
                       uint16_t frame_seq, uint32_t timestamp, bool keyframe,
                       bool fec_enabled = true);

    // Send the most recently prepared frame to |dest|.
    // `send_cs` (optional, non-null only after Noise handshake completes)
    // transport-encrypts each wire per-destination.  Passing nullptr keeps
    // the old plaintext path — used by transport_test and any pre-handshake
    // broadcast.  Returns number of packets sent, or -1 on error.
    int send_prepared(const net::SocketAddr& dest,
                      crypto::CipherState* send_cs = nullptr);

    // Send a RANGE [begin, end) of an explicit wire vector (seal per dest,
    // relay-wrap as needed).  Used by the keyframe send-pacer (VIV-82), which
    // drains a copy of the prepared wires a chunk per host-loop tick.
    int send_wire_range(const std::vector<std::vector<uint8_t>>& wires,
                        size_t begin, size_t end,
                        const net::SocketAddr& dest,
                        crypto::CipherState* send_cs);

    // The wires produced by the last prepare_frame() — the pacer copies these
    // for a keyframe so the next P-frame's prepare_frame() can overwrite them.
    const std::vector<std::vector<uint8_t>>& prepared_wires() const {
        return prepared_wires_;
    }

    // Force-emit parity for the in-progress FEC group, even if it hasn't
    // reached K data shards yet.  Replaces prepared_wires_ with parity-only
    // packets, which the caller then ships via send_prepared().  Used on
    // host idle ticks: when capture stops mid-group (e.g. user releases
    // a mouse drag and the desktop goes static with 4/10 P-frame
    // fragments queued), the client never gets parity to recover the
    // last frame, and the unrelated bits of UI state stay stuck.
    // Returns true if any parity packets were prepared.
    bool flush_pending_fec(uint16_t frame_seq, uint32_t timestamp);

    // Convenience: prepare + send to a single destination (backwards compat).
    int send_frame(const uint8_t* data, size_t data_len,
                   uint16_t frame_seq, uint32_t timestamp,
                   bool keyframe, const net::SocketAddr& dest,
                   crypto::CipherState* send_cs = nullptr);

    // Retransmit previously-sent fragments. Missing fragments (aged out of
    // the ring buffer) are silently skipped. Returns number actually resent.
    // `send_cs` follows the same rules as send_prepared().
    int handle_nack(uint16_t seq_no, const uint16_t* frag_indices, size_t count,
                    const net::SocketAddr& dest,
                    crypto::CipherState* send_cs = nullptr);

    // Adaptive FEC: update M from two complementary client signals.
    //   loss_rate    — slow EWMA, drives a baseline ladder (M=2..4).
    //   delta_failed — fast event signal (FEC groups exceeded M since
    //                  last report).  Any positive value immediately
    //                  bumps M; sustained zeros taper it back down.
    void update_fec_from_loss(float loss_rate, uint32_t delta_failed = 0);
    uint8_t fec_group_size()   const { return fec_encoder_.group_size(); }
    uint8_t fec_parity_count() const { return fec_encoder_.parity_count(); }
    float last_loss_rate() const { return last_loss_rate_; }

    // Per-frame pooled FEC (VIV-82): one RS group per frame with parity as a
    // PERCENTAGE of the frame (pooled, burst-resilient) instead of fixed-K=10
    // rolling groups.  Gated; off = legacy behaviour.
    void set_per_frame_fec(bool on) { per_frame_fec_ = on; fec_encoder_.set_ranged(on); }
    bool per_frame_fec() const { return per_frame_fec_; }

    // Legacy FEC group size.  Bigger K recovers a bigger burst at the SAME
    // overhead % (M scales with K) — see host_session start() (VIV-82).
    void set_fec_group_size(uint8_t k) { fec_encoder_.set_group_size(k); }

    // Burst-resilient interleaving depth (VIV-82).  >1 splits each frame into D
    // groups and transmits them round-robin, so a consecutive wire burst hits D
    // groups by ~1/D each instead of wiping one whole group.  1 = off (legacy
    // send order).  Env VIVORA_FEC_INTERLEAVE.
    void set_fec_interleave(uint8_t d) { fec_interleave_ = d < 1 ? 1 : d; }
    uint8_t fec_interleave() const { return fec_interleave_; }

    // Current redundancy overhead as a percentage — used by the host-loop wire
    // carve-out (encoder_bps = wire * 100 / (100 + pct)).  Per-frame mode: the
    // steady percentage; legacy: 100*M/K (equivalent to the old K/(K+M) carve).
    uint32_t fec_overhead_pct() const {
        if (per_frame_fec_) return static_cast<uint32_t>(current_fec_pct());
        // VIV-88: interleaved mode sizes every group off the ladder percentage,
        // so report the ladder rather than 100*M/K of whichever group happened
        // to be built last.  The instantaneous ratio swings frame to frame once
        // the small-group parity floor kicks in (a 2-packet frame carries M=2 =
        // "100% overhead"), and feeding that swing to the wire carve-out
        // re-carves the encoder several times a second — the exact flapping
        // VIV-84 had to fix.  Tiny frames are a rounding error in bytes, so the
        // ladder is also the more honest estimate of what the wire carries.
        if (fec_interleave_ > 1) return static_cast<uint32_t>(current_fec_pct());
        uint8_t k = fec_encoder_.group_size();
        uint8_t m = fec_encoder_.parity_count();
        return k ? static_cast<uint32_t>(100u * m / k) : 0;
    }

    // Feed RTT for proactive K lowering on WiFi stalls.
    void on_rtt(double rtt_ms);

    // Diagnostic: force M to a fixed value and disable adaptive updates.
    // Set m=0 to clear and restore adaptive behavior. Keyframe boost still
    // applies on top of force_m (but bounded by KEYFRAME_M_MAX).
    void set_force_m(uint8_t m);
    uint8_t force_m() const { return force_m_; }

    uint64_t packets_sent() const { return packets_sent_; }
    uint64_t bytes_sent() const { return bytes_sent_; }
    uint64_t retransmits() const { return retransmits_; }

private:
    // Retransmit buffer key: (seq_no << 16) | frag_index
    static uint32_t retx_key(uint16_t seq, uint16_t frag) {
        return (static_cast<uint32_t>(seq) << 16) | frag;
    }

    // Per-frame pooled FEC packetization (VIV-82): one RS group per frame
    // (split into <=255-packet groups), parity = current_fec_pct()% of K.
    void prepare_frame_per_frame(const uint8_t* data, size_t data_len,
                                 uint16_t frame_seq, uint32_t timestamp,
                                 bool keyframe, bool fec_enabled);

    // Interleaved FEC packetization (VIV-82): D contiguous-key groups sent
    // round-robin for burst resilience.  See set_fec_interleave().
    void prepare_frame_interleaved(const uint8_t* data, size_t data_len,
                                   uint16_t frame_seq, uint32_t timestamp,
                                   bool keyframe);

    void store_retx(uint32_t key, const std::vector<uint8_t>& wire);
    // Linear-scan lookup over the ring.  RETX_BUFFER_CAPACITY (2048) element
    // comparisons are faster than an unordered_map miss chain once you
    // account for cache behavior, and NACK lookup is a cold path (~30/poll).
    const std::vector<uint8_t>* find_retx(uint32_t key) const;

    net::IUdpSocket& socket_;
    PacedSender*     paced_ = nullptr;  // VIV-82: when set, sends route here

    // Relay state.  When relay_active_, every dest passed to send_prepared
    // is ignored at the wire layer (peer is implied by the binding) and
    // the wire packet goes wrapped in DBRL DATA to relay_addr_.
    bool            relay_active_ = false;
    net::SocketAddr relay_addr_{};
    uint8_t         relay_alloc_id_[8] = {};
    net::FrameFragmenter fragmenter_;
    net::FecEncoder fec_encoder_;

    // Failure-driven adaptive M.  Climbs immediately on FEC failures;
    // decays slowly so it stays elevated through WiFi-instability
    // windows that arrive in clusters with multi-second clean lulls.
    // Combined as max() with the loss-EWMA ladder so we can never run
    // BELOW what the slow signal recommends.  Capped at
    // FAILURE_DRIVEN_M_MAX so wire overhead doesn't explode (the
    // bitrate controller carves M out of the wire budget — encoder
    // shrinks instead, total wire stays constant).
    uint8_t failure_driven_m_  = 0;
    int     clean_streak_      = 0;
    // Sticky session floor — once we've ever seen a failure, this latches
    // to 1 so failure_driven_m_ never decays back below it.  Smoke logs
    // showed 9/11 failures still happening at M=2 because the link has
    // bursty quiet periods longer than the 15 s decay window — letting M
    // fall back to baseline gets caught flat-footed by the next cluster.
    bool     ever_failed_       = false;
    // 30 ticks × 500 ms = 15 s of *uninterrupted* clean before we step
    // M down by one.  Was 2.5 s — too short, M decayed back to baseline
    // between WiFi loss clusters and got caught flat-footed by the next
    // burst.  15 s correlates with how WiFi RF environments cycle.
    static constexpr int     CLEAN_DECAY_TICKS    = 30;
    // Cap chosen as M=3*K (= 30 for K=10).  With K data + M parity, the
    // group recovers as long as ANY K of K+M packets arrive — so
    // M=3K gives parity ratio M/(K+M) = 0.75 = survives roughly 75%
    // wire loss before recovery probability collapses.  Cost: the
    // host_loop carve-out (`encoder_bps = wire * K/(K+M)`) shrinks the
    // encoder to 25% of wire budget when fully ramped, so a 10 Mbps
    // wire becomes 2.5 Mbps video — still usable for 1080p HEVC.
    // Only reached when failure_driven_m_ climbs all the way up under
    // sustained loss, never the steady-state target on a clean link.
    static constexpr uint8_t FAILURE_DRIVEN_M_MAX = 30;
    uint64_t packets_sent_ = 0;
    uint64_t bytes_sent_ = 0;
    uint64_t retransmits_ = 0;
    float last_loss_rate_ = 0.0f;

    // Per-frame FEC (VIV-82).  current_fec_pct() maps the adaptive M ladder
    // (steady_m_) to a redundancy percentage: ~25% floor, ramping to a 75% cap
    // as failure-driven M climbs under burst loss.
    bool per_frame_fec_ = false;
    uint8_t fec_interleave_ = 1;   // VIV-82 burst interleaving depth; 1 = off
    // VIV-88: smallest parity a group may carry, whatever the ratio says.  The
    // percentage ladder is meaningless at K=2-4 (a static desktop's frames),
    // where it rounds down to a single parity shard and one lost packet costs
    // the frame.  Two shards there cost a couple of KB and buy the group a
    // second life — and give the client's targeted rescue something to work on.
    static constexpr int MIN_PARITY_SHARDS = 2;
    int current_fec_pct() const {
        int p = static_cast<int>(steady_m_) * 10;
        return p < 25 ? 25 : (p > 75 ? 75 : p);
    }

    // Prepared wire packets (data + FEC) ready for send_prepared().
    std::vector<std::vector<uint8_t>> prepared_wires_;

    // Adaptive M: hysteresis + cooldown.  Raise fast, lower slow.
    uint8_t  pending_relax_count_ = 0;
    uint16_t cooldown_ = 0;
    // Hysteresis: how many "relax pressure" reports before stepping
    // M down by one.  Was 8 (~4s/step at 500ms reports) which left M
    // pinned high for a minute after loss ended.  2 = ~1s/step, fast
    // enough to recover quality within a few seconds of the link
    // cleaning up; still buffers single-tick noise.
    static constexpr uint8_t  HYSTERESIS_UP     = 2;
    static constexpr uint16_t TIGHTEN_COOLDOWN  = 4;
    static constexpr uint16_t RELAX_COOLDOWN    = 20;

    // Keyframe parity boost: steady-state M is tuned for P-frame size;
    // a keyframe is ~40 fragments and losing one group kills the whole
    // frame, triggering a decoder-reject cascade that lasts until the
    // next IDR arrives.  Math at 50% loss with K=10:
    //   M=12 (boost+6 over baseline) → per-group recovery 74% → 5-group
    //                                   IDR success 22% → user sees freeze
    //                                   averaging 3-5 seconds per recovery
    //   M=24 (boost+14)              → per-group 95% → 5-group success 77%
    //                                   → most IDRs land on first try
    // Cost: keyframe wire grows (1+M/K)x = 3.4x for ONE frame, but
    // keyframes fire every few seconds so average wire is barely
    // touched.  P-frames keep their own much lower steady-state M
    // (the failure-driven ladder caps at 7-16 depending on loss rate).
    static constexpr uint8_t KEYFRAME_M_BOOST = 14;
    static constexpr uint8_t KEYFRAME_M_MAX   = 24;

    // RTT-based proactive M raise: spike detection.
    // 50ms threshold — tight enough to catch early WiFi congestion, while
    // RTT_NORMAL_MS=25 keeps normal jitter out of the trigger band.
    static constexpr double  RTT_SPIKE_MS     = 50.0;
    static constexpr double  RTT_NORMAL_MS    = 25.0;
    static constexpr uint8_t RTT_NORMAL_CYCLES = 20;
    static constexpr uint8_t RTT_SPIKE_M      = 3;
    bool     rtt_locked_  = false;
    uint8_t  rtt_normal_count_ = 0;
    uint8_t  pre_lock_m_ = 2;

    // Steady-state M as decided by the loss-adaptive loop.  Separate from
    // whatever the FEC encoder currently has set, because the keyframe
    // path temporarily bumps the encoder's M for its own groups — reading
    // the encoder during that window would give a boosted value.
    // Updated only from update_fec_from_loss().
    uint8_t  steady_m_ = 2;

    // Diagnostic override: when non-zero, freezes M at this value.
    // update_fec_from_loss() and on_rtt() both become no-ops.
    uint8_t  force_m_ = 0;

    // Flat ring of retx slots.  Write cursor walks mod capacity — newest
    // overwrite oldest automatically, so FIFO eviction is free.  Vacant
    // slots (pre-wrap) are identified by an empty wire vector.
    struct RetxSlot {
        uint32_t key = 0;
        std::vector<uint8_t> wire;  // empty == vacant
    };
    std::array<RetxSlot, RETX_BUFFER_CAPACITY> retx_ring_{};
    size_t retx_write_cursor_ = 0;
    int retx_budget_ = MAX_RETX_PER_POLL;

    // Scratch buffer for NACK retransmissions — holds a copy of the stored
    // wire with FLAG_RETX set, so the stored wire stays pristine.
    std::vector<uint8_t> nack_send_buf_;
};

} // namespace vivora::host
