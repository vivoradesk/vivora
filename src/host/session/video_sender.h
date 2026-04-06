#pragma once

#include "common/net/socket.h"
#include "common/net/frame_fragmenter.h"
#include "common/protocol/packet.h"
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace deskbeam::host {

// Fragments an encoded frame and sends all packets over UDP.
// Keeps a ring buffer of recently-sent fragments so they can be retransmitted
// on client NACK (selective repeat).
class VideoSender {
public:
    // Retransmit buffer capacity (fragments). At 60fps + IDR bursts, ~2048
    // covers roughly 500ms of history.
    static constexpr size_t RETX_BUFFER_CAPACITY = 2048;

    explicit VideoSender(net::IUdpSocket& socket) : socket_(socket) {}

    // Send an encoded frame to the given destination.
    // Returns number of packets sent, or -1 on error.
    int send_frame(const uint8_t* data, size_t data_len,
                   uint16_t frame_seq, uint32_t timestamp,
                   bool keyframe, const net::SocketAddr& dest);

    // Retransmit previously-sent fragments. Missing fragments (aged out of
    // the ring buffer) are silently skipped. Returns number actually resent.
    int handle_nack(uint16_t seq_no, const uint16_t* frag_indices, size_t count,
                    const net::SocketAddr& dest);

    uint64_t packets_sent() const { return packets_sent_; }
    uint64_t bytes_sent() const { return bytes_sent_; }
    uint64_t retransmits() const { return retransmits_; }

private:
    // Retransmit buffer key: (seq_no << 16) | frag_index
    static uint32_t retx_key(uint16_t seq, uint16_t frag) {
        return (static_cast<uint32_t>(seq) << 16) | frag;
    }

    net::IUdpSocket& socket_;
    net::FrameFragmenter fragmenter_;
    uint64_t packets_sent_ = 0;
    uint64_t bytes_sent_ = 0;
    uint64_t retransmits_ = 0;

    // Wire bytes stored by key for O(1) lookup; FIFO of keys for eviction order.
    std::deque<uint32_t> retx_fifo_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> retx_index_;
};

} // namespace deskbeam::host
