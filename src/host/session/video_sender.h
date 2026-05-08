#pragma once

#include "common/crypto/noise_nk.h"
#include "common/net/socket.h"
#include "common/net/frame_fragmenter.h"
#include "common/net/fec_codec.h"
#include "common/protocol/packet.h"
#include <array>
#include <cstdint>
#include <vector>

namespace deskbeam::host {

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

    void store_retx(uint32_t key, const std::vector<uint8_t>& wire);
    // Linear-scan lookup over the ring.  RETX_BUFFER_CAPACITY (2048) element
    // comparisons are faster than an unordered_map miss chain once you
    // account for cache behavior, and NACK lookup is a cold path (~30/poll).
    const std::vector<uint8_t>* find_retx(uint32_t key) const;

    net::IUdpSocket& socket_;

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
    // Cap raised from 6 to 7 after smoke logs showed 4 of 16 failures
    // happening at M=6 with "need 1 more" — exactly one more parity
    // would have saved them.  Cost is modest (M=7 means 30% encoder
    // bitrate carved for FEC vs 25% at M=6).  Higher than 7 buys
    // sharply diminishing returns and bursts that big are NACK
    // territory rather than FEC.
    static constexpr uint8_t FAILURE_DRIVEN_M_MAX = 7;
    uint64_t packets_sent_ = 0;
    uint64_t bytes_sent_ = 0;
    uint64_t retransmits_ = 0;
    float last_loss_rate_ = 0.0f;

    // Prepared wire packets (data + FEC) ready for send_prepared().
    std::vector<std::vector<uint8_t>> prepared_wires_;

    // Adaptive M: hysteresis + cooldown.  Raise fast, lower slow.
    uint8_t  pending_relax_count_ = 0;
    uint16_t cooldown_ = 0;
    static constexpr uint8_t  HYSTERESIS_UP     = 8;
    static constexpr uint16_t TIGHTEN_COOLDOWN  = 4;
    static constexpr uint16_t RELAX_COOLDOWN    = 20;

    // Keyframe parity boost: steady-state M is tuned for P-frame size;
    // a keyframe is ~40 fragments and losing one group kills the whole
    // frame, triggering a decoder-reject cascade that lasts until the
    // next IDR arrives. Boost+6 with cap=12 covers WiFi bursts up to
    // 8 packets even when steady-state M=2, while ratched-up M=7 reaches
    // the cap. Keyframes fire ~once per recovery so the extra parity
    // is essentially free on average bandwidth.
    static constexpr uint8_t KEYFRAME_M_BOOST = 6;
    static constexpr uint8_t KEYFRAME_M_MAX   = 12;

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

} // namespace deskbeam::host
