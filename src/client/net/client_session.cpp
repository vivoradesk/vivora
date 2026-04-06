#include "client/net/client_session.h"
#include "common/protocol/packet.h"
#include "common/utils/log.h"
#include <cstring>
#include <chrono>

namespace deskbeam::client {

static const uint8_t HELLO_MAGIC[] = { 'D','E','S','K','B','E','A','M', 0x01 };
static const uint8_t HELLO_ACK[]   = { 'D','E','S','K','B','E','A','M', 0x01, 0x00 };

bool ClientSession::start(const char* host_ip, uint16_t port) {
    socket_ = net::IUdpSocket::create();
    if (!socket_) return false;

    // Bind to any port
    if (!socket_->bind(0)) {
        log::error("ClientSession", "Failed to bind");
        return false;
    }

    socket_->set_nonblocking(true);
    socket_->set_recvbuf(1024 * 1024);

    host_addr_.ip = net::parse_ip(host_ip);
    host_addr_.port = port;

    if (host_addr_.ip == 0) {
        log::error("ClientSession", "Invalid host IP: %s", host_ip);
        return false;
    }

    receiver_ = std::make_unique<VideoReceiver>(*socket_);
    state_ = SessionState::Connecting;
    connect_start_ = Clock::now();
    last_hello_time_ = {};
    last_recv_time_ = Clock::now();

    log::info("ClientSession", "Connecting to %s:%u", host_ip, port);
    send_hello();
    return true;
}

void ClientSession::stop() {
    if (socket_) {
        socket_->close();
        socket_.reset();
    }
    receiver_.reset();
    state_ = SessionState::Disconnected;
}

void ClientSession::poll() {
    if (!socket_) return;

    uint8_t buf[RECV_BUF_SIZE];
    net::SocketAddr sender;

    for (;;) {
        int n = socket_->recv_from(buf, sizeof(buf), sender);
        if (n <= 0) break;
        last_recv_time_ = Clock::now();
        handle_packet(buf, static_cast<size_t>(n));
    }

    auto now = Clock::now();

    if (state_ == SessionState::Connecting) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - connect_start_).count();
        if (elapsed > CONNECT_TIMEOUT_MS) {
            log::error("ClientSession", "Connection timed out");
            state_ = SessionState::Disconnected;
            return;
        }
        auto since_hello = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_hello_time_).count();
        if (since_hello > HELLO_RETRY_MS) {
            send_hello();
        }
    }

    if (state_ == SessionState::Connected) {
        auto since_recv = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_recv_time_).count();
        if (since_recv > DISCONNECT_TIMEOUT_MS) {
            log::warn("ClientSession", "Host timed out");
            state_ = SessionState::Disconnected;
            return;
        }

        // NACK processing: ask host to retransmit missing fragments.
        // gap_ms: how long to wait before NACKing gaps within a frame (small,
        //   ~2x RTT floor or 4ms — fragments arrive within 1ms on LAN).
        // rate_limit_ms: minimum interval between re-requesting the same fragment,
        //   roughly 1.5x measured RTT, floored at 8ms.
        if (receiver_) {
            int64_t gap_ms = 4;
            int64_t rl_ms = rtt_ms_ > 0 ? static_cast<int64_t>(rtt_ms_ * 1.5) : 8;
            if (rl_ms < 8) rl_ms = 8;
            auto batches = receiver_->collect_nacks(gap_ms, rl_ms);
            for (const auto& b : batches) {
                send_nack(b.seq_no, b.frag_indices.data(), b.frag_indices.size());
            }
        }
    }
}

bool ClientSession::pop_frame(net::AssembledFrame& frame) {
    if (!receiver_) return false;
    return receiver_->pop_frame(frame);
}

void ClientSession::handle_packet(const uint8_t* data, size_t len) {
    if (len < protocol::PacketHeader::WIRE_SIZE) return;

    auto header = protocol::PacketHeader::deserialize(data);
    const uint8_t* payload = data + protocol::PacketHeader::WIRE_SIZE;
    size_t payload_len = len - protocol::PacketHeader::WIRE_SIZE;

    switch (header.type) {
        case protocol::PacketType::Control:
            handle_control(payload, payload_len);
            break;
        case protocol::PacketType::Ping:
            handle_ping(payload, payload_len);
            break;
        case protocol::PacketType::Video: {
            auto packet = protocol::Packet::deserialize(data, len);
            receiver_->feed(packet);
            break;
        }
        default:
            break;
    }
}

void ClientSession::handle_control(const uint8_t* payload, size_t len) {
    if (len < sizeof(HELLO_ACK)) return;
    if (std::memcmp(payload, HELLO_ACK, sizeof(HELLO_ACK)) != 0) return;

    if (state_ == SessionState::Connecting) {
        state_ = SessionState::Connected;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - connect_start_).count();
        log::info("ClientSession", "Connected (handshake took %lldms)", elapsed);
    }
}

void ClientSession::handle_ping(const uint8_t* payload, size_t len) {
    // Reply with Pong, echoing the ping payload
    protocol::Packet pong;
    pong.header.type = protocol::PacketType::Pong;
    pong.header.seq_no = 0;
    pong.header.timestamp = 0;
    pong.header.flags = 0;
    pong.payload.assign(payload, payload + len);
    pong.header.payload_len = static_cast<uint16_t>(len);

    auto wire = pong.serialize();
    socket_->send_to(wire.data(), wire.size(), host_addr_);
}

void ClientSession::send_hello() {
    protocol::Packet hello;
    hello.header.type = protocol::PacketType::Control;
    hello.header.seq_no = 0;
    hello.header.timestamp = 0;
    hello.header.flags = 0;
    hello.payload.assign(HELLO_MAGIC, HELLO_MAGIC + sizeof(HELLO_MAGIC));
    hello.header.payload_len = static_cast<uint16_t>(hello.payload.size());

    auto wire = hello.serialize();
    socket_->send_to(wire.data(), wire.size(), host_addr_);
    last_hello_time_ = Clock::now();
}

void ClientSession::send_input(const protocol::InputEvent& event) {
    if (state_ != SessionState::Connected || !socket_) return;

    auto payload = event.serialize();

    protocol::Packet pkt;
    pkt.header.type = protocol::PacketType::Input;
    pkt.header.seq_no = 0;
    pkt.header.timestamp = 0;
    pkt.header.flags = 0;
    pkt.payload = std::move(payload);
    pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());

    auto wire = pkt.serialize();
    socket_->send_to(wire.data(), wire.size(), host_addr_);
}

void ClientSession::send_nack(uint16_t seq_no, const uint16_t* frag_indices, size_t count) {
    if (state_ != SessionState::Connected || !socket_ || count == 0 || count > 255) return;

    protocol::Packet pkt;
    pkt.header.type = protocol::PacketType::NackRequest;
    pkt.header.seq_no = 0;
    pkt.header.timestamp = 0;
    pkt.header.flags = 0;
    pkt.payload.resize(3 + count * 2);
    pkt.payload[0] = static_cast<uint8_t>(seq_no & 0xFF);
    pkt.payload[1] = static_cast<uint8_t>((seq_no >> 8) & 0xFF);
    pkt.payload[2] = static_cast<uint8_t>(count);
    for (size_t i = 0; i < count; ++i) {
        pkt.payload[3 + i * 2]     = static_cast<uint8_t>(frag_indices[i] & 0xFF);
        pkt.payload[3 + i * 2 + 1] = static_cast<uint8_t>((frag_indices[i] >> 8) & 0xFF);
    }
    pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());

    auto wire = pkt.serialize();
    socket_->send_to(wire.data(), wire.size(), host_addr_);
}

void ClientSession::request_idr() {
    if (state_ != SessionState::Connected || !socket_) return;

    protocol::Packet pkt;
    pkt.header.type = protocol::PacketType::IdrRequest;
    pkt.header.seq_no = 0;
    pkt.header.timestamp = 0;
    pkt.header.flags = 0;
    pkt.header.payload_len = 0;

    auto wire = pkt.serialize();
    socket_->send_to(wire.data(), wire.size(), host_addr_);
}

uint64_t ClientSession::frames_dropped() const {
    return receiver_ ? receiver_->frames_dropped() : 0;
}

} // namespace deskbeam::client
