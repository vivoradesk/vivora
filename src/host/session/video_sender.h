#pragma once

#include "common/net/socket.h"
#include "common/net/frame_fragmenter.h"
#include "common/protocol/packet.h"
#include <cstdint>

namespace deskbeam::host {

// Fragments an encoded frame and sends all packets over UDP.
class VideoSender {
public:
    explicit VideoSender(net::IUdpSocket& socket) : socket_(socket) {}

    // Send an encoded frame to the given destination.
    // Returns number of packets sent, or -1 on error.
    int send_frame(const uint8_t* data, size_t data_len,
                   uint16_t frame_seq, uint32_t timestamp,
                   bool keyframe, const net::SocketAddr& dest);

    uint64_t packets_sent() const { return packets_sent_; }
    uint64_t bytes_sent() const { return bytes_sent_; }

private:
    net::IUdpSocket& socket_;
    net::FrameFragmenter fragmenter_;
    uint64_t packets_sent_ = 0;
    uint64_t bytes_sent_ = 0;
};

} // namespace deskbeam::host
