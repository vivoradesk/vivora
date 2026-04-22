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

        // Reusable scratch — avoids a per-packet allocation on the UDP
        // receive hot path (poll() is called from transport_test; the
        // production path uses ClientSession::handle_packet which has
        // its own scratch buffer).
        recovered_scratch_.clear();
        fec_decoder_.feed(buf, static_cast<size_t>(n), recovered_scratch_);

        for (const auto& rec_wire : recovered_scratch_) {
            if (rec_wire.size() >= protocol::PacketHeader::WIRE_SIZE) {
                auto rec_pkt = protocol::Packet::deserialize(
                    rec_wire.data(), rec_wire.size());
                assembler_.feed(rec_pkt);
            }
        }

        // Feed original packet to assembler (skip FEC parity packets)
        auto hdr = protocol::PacketHeader::deserialize(buf);
        if (!(hdr.flags & protocol::FLAG_FEC)) {
            auto packet = protocol::Packet::deserialize(buf, static_cast<size_t>(n));
            assembler_.feed(packet);
        }
    }

    return count;
}

} // namespace deskbeam::client
