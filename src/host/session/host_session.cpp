#include "host/session/host_session.h"
#include "common/protocol/packet.h"
#include "common/protocol/input_event.h"
#include "common/utils/log.h"
#include <cstring>
#include <chrono>
#include <vector>

namespace deskbeam::host {

// Handshake magic: "DESKBEAM" + version byte
static const uint8_t HELLO_MAGIC[] = { 'D','E','S','K','B','E','A','M', 0x01 };
static const uint8_t HELLO_ACK[]   = { 'D','E','S','K','B','E','A','M', 0x01, 0x00 };

bool HostSession::start(uint16_t port) {
    socket_ = net::IUdpSocket::create();
    if (!socket_) return false;

    if (!socket_->bind(port)) {
        log::error("HostSession", "Failed to bind port %u", port);
        return false;
    }

    socket_->set_nonblocking(true);
    socket_->set_sendbuf(512 * 1024);
    socket_->set_recvbuf(1024 * 1024);

    sender_ = std::make_unique<VideoSender>(*socket_);
    input_injector_ = InputInjector::create();
    if (input_injector_ && pending_screen_w_ && pending_screen_h_) {
        input_injector_->set_screen_resolution(pending_screen_w_, pending_screen_h_);
    }
    state_ = SessionState::WaitingForClient;
    last_recv_time_ = Clock::now();
    last_ping_time_ = Clock::now();

    log::info("HostSession", "Listening on port %u", port);
    return true;
}

void HostSession::stop() {
    if (socket_) {
        socket_->close();
        socket_.reset();
    }
    sender_.reset();
    state_ = SessionState::Disconnected;
}

void HostSession::poll() {
    if (!socket_) return;

    // Reset retransmit budget so NACK handling doesn't starve capture.
    if (sender_) sender_->reset_retx_budget();

    uint8_t buf[RECV_BUF_SIZE];
    net::SocketAddr sender;

    for (;;) {
        int n = socket_->recv_from(buf, sizeof(buf), sender);
        if (n <= 0) break;
        handle_packet(buf, static_cast<size_t>(n), sender);
    }

    auto now = Clock::now();

    if (state_ == SessionState::Connected) {
        // Check disconnect timeout
        auto since_recv = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_recv_time_).count();
        if (since_recv > DISCONNECT_TIMEOUT_MS) {
            log::warn("HostSession", "Client timed out (%.0fms)", (double)since_recv);
            state_ = SessionState::Disconnected;
            return;
        }

        // Send periodic ping
        auto since_ping = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_ping_time_).count();
        if (since_ping > PING_INTERVAL_MS) {
            send_ping();
            last_ping_time_ = now;
        }

        // Feed RTT to sender for proactive K lowering on WiFi stalls.
        // Only on fresh pong (RTT changed) to avoid counting stale samples.
        if (sender_ && rtt_ms_ != last_rtt_sent_) {
            sender_->on_rtt(rtt_ms_);
            last_rtt_sent_ = rtt_ms_;
        }

        // Probe timeout: give up waiting if no result within deadline.
        if (probe_pending_) {
            auto since_probe = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - probe_sent_time_).count();
            if (since_probe > BW_PROBE_TIMEOUT_MS) {
                probe_pending_ = false;
                log::warn("HostSession", "BW probe timeout — using default ceiling");
            }
        }
    }
}

int HostSession::send_frame(const uint8_t* data, size_t data_len,
                            uint16_t frame_seq, uint32_t timestamp, bool keyframe) {
    if (state_ != SessionState::Connected || !sender_)
        return -1;
    return sender_->send_frame(data, data_len, frame_seq, timestamp, keyframe, client_addr_);
}

void HostSession::handle_packet(const uint8_t* data, size_t len, const net::SocketAddr& sender) {
    if (len < protocol::PacketHeader::WIRE_SIZE) return;

    auto header = protocol::PacketHeader::deserialize(data);
    const uint8_t* payload = data + protocol::PacketHeader::WIRE_SIZE;
    size_t payload_len = len - protocol::PacketHeader::WIRE_SIZE;

    last_recv_time_ = Clock::now();

    switch (header.type) {
        case protocol::PacketType::Control:
            handle_hello(payload, payload_len, sender);
            break;
        case protocol::PacketType::Pong:
            handle_pong(payload, payload_len);
            break;
        case protocol::PacketType::Input:
            handle_input(payload, payload_len);
            break;
        case protocol::PacketType::IdrRequest:
            if (state_ == SessionState::Connected) {
                idr_needed_ = true;
                log::info("HostSession", "Client requested IDR (frame loss recovery)");
            }
            break;
        case protocol::PacketType::NackRequest:
            if (state_ == SessionState::Connected && sender_ && payload_len >= 3) {
                uint16_t seq = payload[0] | (payload[1] << 8);
                uint8_t count = payload[2];
                if (payload_len >= 3u + count * 2u) {
                    std::vector<uint16_t> indices(count);
                    for (uint8_t i = 0; i < count; ++i) {
                        indices[i] = payload[3 + i * 2] | (payload[4 + i * 2] << 8);
                    }
                    sender_->handle_nack(seq, indices.data(), count, client_addr_);
                }
            }
            break;
        case protocol::PacketType::FecReport:
            if (state_ == SessionState::Connected && sender_ && payload_len >= 4) {
                float loss_rate;
                std::memcpy(&loss_rate, payload, 4);
                sender_->update_fec_from_loss(loss_rate);
            }
            break;
        case protocol::PacketType::BwProbeAck:
            if (state_ == SessionState::Connected) {
                handle_bw_probe_ack(payload, payload_len);
            }
            break;
        default:
            break;
    }
}

void HostSession::handle_hello(const uint8_t* payload, size_t len,
                               const net::SocketAddr& sender) {
    if (len < sizeof(HELLO_MAGIC)) return;
    if (std::memcmp(payload, HELLO_MAGIC, sizeof(HELLO_MAGIC)) != 0) return;

    // Send ACK
    protocol::Packet ack;
    ack.header.type = protocol::PacketType::Control;
    ack.header.seq_no = 0;
    ack.header.timestamp = 0;
    ack.header.flags = 0;
    ack.payload.assign(HELLO_ACK, HELLO_ACK + sizeof(HELLO_ACK));
    ack.header.payload_len = static_cast<uint16_t>(ack.payload.size());

    auto wire = ack.serialize();
    socket_->send_to(wire.data(), wire.size(), sender);

    client_addr_ = sender;
    state_ = SessionState::Connected;
    idr_needed_ = true;
    send_bw_probe();
    log::info("HostSession", "Client connected from %u.%u.%u.%u:%u",
        (sender.ip >> 0) & 0xFF, (sender.ip >> 8) & 0xFF,
        (sender.ip >> 16) & 0xFF, (sender.ip >> 24) & 0xFF, sender.port);
}

void HostSession::handle_pong(const uint8_t* payload, size_t len) {
    if (len < 4) return;
    uint32_t seq = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24);
    if (seq == ping_seq_) {
        auto now = Clock::now();
        rtt_ms_ = std::chrono::duration<double, std::milli>(now - ping_sent_time_).count();
    }
}

void HostSession::send_ping() {
    ping_seq_++;
    ping_sent_time_ = Clock::now();

    protocol::Packet ping;
    ping.header.type = protocol::PacketType::Ping;
    ping.header.seq_no = 0;
    ping.header.timestamp = 0;
    ping.header.flags = 0;
    // Payload: seq(4) | rtt_us(4 LE). Client uses rtt_us to tune NACK timing.
    uint32_t rtt_us = static_cast<uint32_t>(rtt_ms_ * 1000.0);
    ping.payload.resize(8);
    ping.payload[0] = static_cast<uint8_t>(ping_seq_ & 0xFF);
    ping.payload[1] = static_cast<uint8_t>((ping_seq_ >> 8) & 0xFF);
    ping.payload[2] = static_cast<uint8_t>((ping_seq_ >> 16) & 0xFF);
    ping.payload[3] = static_cast<uint8_t>((ping_seq_ >> 24) & 0xFF);
    ping.payload[4] = static_cast<uint8_t>(rtt_us & 0xFF);
    ping.payload[5] = static_cast<uint8_t>((rtt_us >> 8) & 0xFF);
    ping.payload[6] = static_cast<uint8_t>((rtt_us >> 16) & 0xFF);
    ping.payload[7] = static_cast<uint8_t>((rtt_us >> 24) & 0xFF);
    ping.header.payload_len = 8;

    auto wire = ping.serialize();
    socket_->send_to(wire.data(), wire.size(), client_addr_);
}

void HostSession::send_bw_probe() {
    probe_id_++;
    probe_pending_ = true;
    probe_bw_bps_ = 0;
    probe_sent_time_ = Clock::now();

    for (uint8_t i = 0; i < BW_PROBE_COUNT; ++i) {
        protocol::Packet pkt;
        pkt.header.type = protocol::PacketType::BwProbe;
        pkt.header.seq_no = 0;
        pkt.header.timestamp = 0;
        pkt.header.flags = 0;
        // Payload: probe_id(2B LE) + index(1B) + count(1B) + padding
        pkt.payload.resize(BW_PROBE_SIZE, 0);
        pkt.payload[0] = static_cast<uint8_t>(probe_id_ & 0xFF);
        pkt.payload[1] = static_cast<uint8_t>((probe_id_ >> 8) & 0xFF);
        pkt.payload[2] = i;
        pkt.payload[3] = BW_PROBE_COUNT;
        pkt.header.payload_len = BW_PROBE_SIZE;

        auto wire = pkt.serialize();
        socket_->send_to(wire.data(), wire.size(), client_addr_);
    }
    log::info("HostSession", "Sent BW probe (%d x %dB), id=%u",
              BW_PROBE_COUNT, BW_PROBE_SIZE, probe_id_);
}

void HostSession::handle_bw_probe_ack(const uint8_t* payload, size_t len) {
    if (len < 8) return;
    uint16_t ack_id = payload[0] | (static_cast<uint16_t>(payload[1]) << 8);
    if (ack_id != probe_id_) return;

    uint32_t bw_bps = payload[4]
                    | (static_cast<uint32_t>(payload[5]) << 8)
                    | (static_cast<uint32_t>(payload[6]) << 16)
                    | (static_cast<uint32_t>(payload[7]) << 24);
    probe_bw_bps_ = bw_bps;
    probe_pending_ = false;
    uint8_t received = payload[2];
    log::info("HostSession", "BW probe result: %u kbps (%u/%u received)",
              bw_bps / 1000, received, BW_PROBE_COUNT);
}

void HostSession::handle_input(const uint8_t* payload, size_t len) {
    if (state_ != SessionState::Connected) return;

    protocol::InputEvent event;
    if (protocol::InputEvent::deserialize(payload, len, event)) {
        if (input_injector_) input_injector_->inject(event);
    }
}

} // namespace deskbeam::host
