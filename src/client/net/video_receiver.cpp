#include "client/net/video_receiver.h"

namespace deskbeam::client {

int VideoReceiver::poll() {
    uint8_t buf[RECV_BUF_SIZE];
    net::SocketAddr sender;
    int count = 0;

    for (;;) {
        int n = socket_.recv_from(buf, sizeof(buf), sender);
        if (n <= 0) break;  // no more data or error

        bytes_received_ += n;
        packets_received_++;
        count++;

        if (static_cast<size_t>(n) < protocol::PacketHeader::WIRE_SIZE)
            continue; // runt packet

        auto packet = protocol::Packet::deserialize(buf, static_cast<size_t>(n));
        assembler_.feed(packet);
    }

    return count;
}

} // namespace deskbeam::client
