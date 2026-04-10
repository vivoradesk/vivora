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

    explicit VideoSender(net::IUdpSocket& socket) : socket_(socket) {}

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

    uint64_t packets_sent() const { return packets_sent_; }
    uint64_t bytes_sent() const { return bytes_sent_; }
    uint64_t retransmits() const { return retransmits_; }

private:
    // Retransmit buffer key: (seq_no << 16) | frag_index
    static uint32_t retx_key(uint16_t seq, uint16_t frag) {
        return (static_cast<uint32_t>(seq) << 16) | frag;
    }

    void store_retx(uint32_t key, std::vector<uint8_t> wire);
    int send_wire(const std::vector<uint8_t>& wire, const net::SocketAddr& dest);

    net::IUdpSocket& socket_;
    net::FrameFragmenter fragmenter_;
    net::FecEncoder fec_encoder_;
    uint64_t packets_sent_ = 0;
    uint64_t bytes_sent_ = 0;
    uint64_t retransmits_ = 0;

    // Adaptive K hysteresis: change only after 3 consecutive same-target reports
    uint8_t pending_k_ = 10;
    uint8_t pending_k_count_ = 0;
    static constexpr uint8_t HYSTERESIS_COUNT = 3;

    // Wire bytes stored by key for O(1) lookup; FIFO of keys for eviction order.
    std::deque<uint32_t> retx_fifo_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> retx_index_;
};

} // namespace deskbeam::host
