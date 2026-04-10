#pragma once

#include "common/net/socket.h"
#include "common/net/frame_assembler.h"
#include "common/net/fec_codec.h"
#include "common/protocol/packet.h"
#include <cstdint>

namespace deskbeam::client {

// Polls UDP socket for video packets, reassembles frames.
// FEC decoder sits between the socket and the assembler: raw wire packets
// are fed to FecDecoder first, which may recover lost packets; recovered
// packets are then fed to the assembler alongside normal ones.
class VideoReceiver {
public:
    explicit VideoReceiver(net::IUdpSocket& socket) : socket_(socket) {}

    // Poll socket and feed any received packets to the assembler.
    // Returns number of packets received this call.
    int poll();

    // Feed raw wire bytes through FEC decoder (may recover lost packets).
    void fec_feed(const uint8_t* wire, size_t len,
                  std::vector<std::vector<uint8_t>>& recovered) {
        fec_decoder_.feed(wire, len, recovered);
    }

    // Feed an already-deserialized video packet to the assembler.
    bool feed(const protocol::Packet& packet) { return assembler_.feed(packet); }

    // Pop next complete reassembled frame. Returns false if none available.
    bool pop_frame(net::AssembledFrame& frame) { return assembler_.pop_frame(frame); }

    // Collect pending fragments that need retransmit (see FrameAssembler::collect_nacks).
    std::vector<net::NackBatch> collect_nacks(int64_t gap_ms, int64_t rate_limit_ms) {
        return assembler_.collect_nacks(gap_ms, rate_limit_ms);
    }

    // Packet loss rate from FEC decoder (EWMA, 0.0–1.0).
    float loss_rate() const { return fec_decoder_.loss_rate(); }

    uint64_t packets_received() const { return packets_received_; }
    uint64_t bytes_received() const { return bytes_received_; }
    uint64_t frames_completed() const { return assembler_.frames_completed(); }
    uint64_t frames_dropped() const { return assembler_.frames_dropped(); }

private:
    net::IUdpSocket& socket_;
    net::FecDecoder fec_decoder_;
    net::FrameAssembler assembler_;
    uint64_t packets_received_ = 0;
    uint64_t bytes_received_ = 0;

    static constexpr size_t RECV_BUF_SIZE = 2048;
};

} // namespace deskbeam::client
