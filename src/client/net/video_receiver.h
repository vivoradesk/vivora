#pragma once

#include "common/net/socket.h"
#include "common/net/frame_assembler.h"
#include "common/protocol/packet.h"
#include <cstdint>

namespace deskbeam::client {

// Polls UDP socket for video packets, reassembles frames.
class VideoReceiver {
public:
    explicit VideoReceiver(net::IUdpSocket& socket) : socket_(socket) {}

    // Poll socket and feed any received packets to the assembler.
    // Returns number of packets received this call.
    int poll();

    // Feed an already-deserialized video packet to the assembler.
    bool feed(const protocol::Packet& packet) { return assembler_.feed(packet); }

    // Pop next complete reassembled frame. Returns false if none available.
    bool pop_frame(net::AssembledFrame& frame) { return assembler_.pop_frame(frame); }

    // Collect pending fragments that need retransmit (see FrameAssembler::collect_nacks).
    std::vector<net::NackBatch> collect_nacks(int64_t gap_ms, int64_t rate_limit_ms) {
        return assembler_.collect_nacks(gap_ms, rate_limit_ms);
    }

    uint64_t packets_received() const { return packets_received_; }
    uint64_t bytes_received() const { return bytes_received_; }
    uint64_t frames_completed() const { return assembler_.frames_completed(); }
    uint64_t frames_dropped() const { return assembler_.frames_dropped(); }

private:
    net::IUdpSocket& socket_;
    net::FrameAssembler assembler_;
    uint64_t packets_received_ = 0;
    uint64_t bytes_received_ = 0;

    static constexpr size_t RECV_BUF_SIZE = 2048;
};

} // namespace deskbeam::client
