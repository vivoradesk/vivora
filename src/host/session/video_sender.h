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

    // Prepare a frame for sending: fragment, generate FEC, store in retx buffer.
    // Call once per frame, then send_prepared() for each destination.
    void prepare_frame(const uint8_t* data, size_t data_len,
                       uint16_t frame_seq, uint32_t timestamp, bool keyframe);

    // Send the most recently prepared frame to |dest|.
    // `send_cs` (optional, non-null only after Noise handshake completes)
    // transport-encrypts each wire per-destination.  Passing nullptr keeps
    // the old plaintext path — used by transport_test and any pre-handshake
    // broadcast.  Returns number of packets sent, or -1 on error.
    int send_prepared(const net::SocketAddr& dest,
                      crypto::CipherState* send_cs = nullptr);

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

    // Adaptive FEC: update M (parity count) based on client-reported loss rate.
    void update_fec_from_loss(float loss_rate);
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
    net::FrameFragmenter fragmenter_;
    net::FecEncoder fec_encoder_;
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
    // frame.  Add extra parity just for the keyframe's groups, capped
    // at KEYFRAME_M_MAX to keep recovery math bounded.
    static constexpr uint8_t KEYFRAME_M_BOOST = 2;
    static constexpr uint8_t KEYFRAME_M_MAX   = 8;

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
