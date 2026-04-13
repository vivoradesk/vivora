#pragma once

#include "common/net/socket.h"
#include "common/net/frame_fragmenter.h"
#include "common/net/fec_codec.h"
#include "common/protocol/packet.h"
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace deskbeam::host {

// Fragments an encoded frame and sends all packets over UDP.
// Keeps a ring buffer of recently-sent fragments so they can be retransmitted
// on client NACK (selective repeat). Generates XOR FEC parity packets.
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

    // Send an encoded frame to the given destination.
    // Returns number of packets sent (including FEC), or -1 on error.
    int send_frame(const uint8_t* data, size_t data_len,
                   uint16_t frame_seq, uint32_t timestamp,
                   bool keyframe, const net::SocketAddr& dest);

    // Retransmit previously-sent fragments. Missing fragments (aged out of
    // the ring buffer) are silently skipped. Returns number actually resent.
    int handle_nack(uint16_t seq_no, const uint16_t* frag_indices, size_t count,
                    const net::SocketAddr& dest);

    // Adaptive FEC: update K based on client-reported loss rate.
    void update_fec_from_loss(float loss_rate);
    uint8_t fec_group_size() const { return fec_encoder_.group_size(); }
    float last_loss_rate() const { return last_loss_rate_; }

    // Feed RTT for proactive K lowering on WiFi stalls.
    void on_rtt(double rtt_ms);

    uint64_t packets_sent() const { return packets_sent_; }
    uint64_t bytes_sent() const { return bytes_sent_; }
    uint64_t retransmits() const { return retransmits_; }

private:
    // Retransmit buffer key: (seq_no << 16) | frag_index
    static uint32_t retx_key(uint16_t seq, uint16_t frag) {
        return (static_cast<uint32_t>(seq) << 16) | frag;
    }

    // Graduated relaxation: 3 -> 5 -> 10, one step at a time.
    // K=20 disabled: retransmits mask channel loss from FEC EWMA,
    // causing 10↔20 flapping. K=10 (10% overhead) is the stable
    // operating point for WiFi.
    static uint8_t next_relax_step(uint8_t current_k) {
        if (current_k < 5)  return 5;
        return 10;
    }

    void store_retx(uint32_t key, std::vector<uint8_t> wire);
    int send_wire(const std::vector<uint8_t>& wire, const net::SocketAddr& dest);

    net::IUdpSocket& socket_;
    net::FrameFragmenter fragmenter_;
    net::FecEncoder fec_encoder_;
    uint64_t packets_sent_ = 0;
    uint64_t bytes_sent_ = 0;
    uint64_t retransmits_ = 0;
    float last_loss_rate_ = 0.0f;

    // Adaptive K: hysteresis + cooldown to prevent oscillation.
    // After tightening K, we don't relax for RELAX_COOLDOWN reports (~5 sec).
    // After relaxing, a shorter cooldown prevents immediate re-tightening.
    uint8_t pending_k_ = 10;
    uint8_t pending_k_count_ = 0;
    uint16_t cooldown_ = 0;       // reports remaining before K can change again
    static constexpr uint8_t HYSTERESIS_DOWN = 2;    // tighten quickly
    static constexpr uint8_t HYSTERESIS_UP   = 8;    // relax slowly (~4 sec)
    static constexpr uint16_t TIGHTEN_COOLDOWN = 4;  // ~2 sec after tightening
    static constexpr uint16_t RELAX_COOLDOWN   = 20; // ~10 sec after relaxing

    // RTT-based proactive K lowering: spike detection.
    static constexpr double RTT_SPIKE_MS  = 30.0;   // force K=3 above this
    static constexpr double RTT_NORMAL_MS = 15.0;    // release lock below this
    static constexpr uint8_t RTT_NORMAL_CYCLES = 6;  // ~3 sec of normal RTT to unlock
    bool     rtt_locked_k3_ = false;
    uint8_t  rtt_normal_count_ = 0;
    uint8_t  pre_lock_k_ = 10;

    // Wire bytes stored by key for O(1) lookup; FIFO of keys for eviction order.
    std::deque<uint32_t> retx_fifo_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> retx_index_;
    int retx_budget_ = MAX_RETX_PER_POLL;
};

} // namespace deskbeam::host
