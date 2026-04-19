#include "host/session/host_session.h"
#include "common/protocol/packet.h"
#include "common/protocol/input_event.h"
#include "common/utils/log.h"
#include <algorithm>
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

    // Audio socket on port + 1.
    audio_socket_ = net::IUdpSocket::create();
    if (audio_socket_ && audio_socket_->bind(port + 1)) {
        audio_socket_->set_nonblocking(true);
        audio_socket_->set_sendbuf(256 * 1024);
        audio_socket_->set_recvbuf(256 * 1024);
        audio_sender_ = std::make_unique<AudioSender>(*audio_socket_);
        if (!audio_sender_->init(128000)) {
            log::warn("HostSession", "Audio encoder init failed — audio disabled");
            audio_sender_.reset();
            audio_socket_.reset();
        } else {
            log::info("HostSession", "Audio listening on port %u", port + 1);
        }
    } else {
        log::warn("HostSession", "Failed to bind audio port %u", port + 1);
        audio_socket_.reset();
    }

    input_injector_ = InputInjector::create();
    if (input_injector_ && pending_screen_w_ && pending_screen_h_) {
        input_injector_->set_screen_resolution(pending_screen_w_, pending_screen_h_);
    }
    state_ = SessionState::WaitingForClient;
    clients_.clear();
    new_client_flag_ = false;

    log::info("HostSession", "Listening on port %u", port);
    return true;
}

void HostSession::stop() {
    if (socket_) {
        socket_->close();
        socket_.reset();
    }
    if (audio_socket_) {
        audio_socket_->close();
        audio_socket_.reset();
    }
    audio_sender_.reset();
    sender_.reset();
    clients_.clear();
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

    // Drain audio socket (reserved for future mic direction; ignored for now).
    if (audio_socket_) {
        net::SocketAddr a_sender;
        for (;;) {
            int n = audio_socket_->recv_from(buf, sizeof(buf), a_sender);
            if (n <= 0) break;
        }
    }

    auto now = Clock::now();

    // Per-client housekeeping: timeouts, pings, RTT feed, probe timeout.
    std::vector<net::SocketAddr> timed_out;
    for (auto& [addr, client] : clients_) {
        auto since_recv = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - client.last_recv_time).count();
        if (since_recv > DISCONNECT_TIMEOUT_MS) {
            timed_out.push_back(addr);
            continue;
        }

        // Send periodic ping.
        auto since_ping = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - client.last_ping_time).count();
        if (since_ping > PING_INTERVAL_MS) {
            send_ping(client);
            client.last_ping_time = now;
        }

        // Feed RTT to sender for proactive K lowering (use worst RTT).
        if (sender_ && client.rtt_ms != client.last_rtt_sent) {
            // RTT feed uses worst-case — handled in rtt_ms() aggregate.
            client.last_rtt_sent = client.rtt_ms;
        }

        // Probe timeout.
        if (client.probe_pending) {
            auto since_probe = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - client.probe_sent_time).count();
            if (since_probe > BW_PROBE_TIMEOUT_MS) {
                client.probe_pending = false;
                log::warn("HostSession", "BW probe timeout for client");
            }
        }
    }

    // Remove timed-out clients.
    for (const auto& addr : timed_out) {
        log::warn("HostSession", "Client %u.%u.%u.%u:%u timed out",
            (addr.ip >> 0) & 0xFF, (addr.ip >> 8) & 0xFF,
            (addr.ip >> 16) & 0xFF, (addr.ip >> 24) & 0xFF, addr.port);
        clients_.erase(addr);
    }

    // Feed worst-case RTT to sender.
    if (sender_ && !clients_.empty()) {
        sender_->on_rtt(rtt_ms());
    }

    // Update aggregate state.
    if (clients_.empty() && state_ == SessionState::Connected) {
        log::info("HostSession", "All clients disconnected");
        state_ = SessionState::Disconnected;
    }
}

int HostSession::send_frame(const uint8_t* data, size_t data_len,
                            uint16_t frame_seq, uint32_t timestamp, bool keyframe) {
    if (state_ != SessionState::Connected || !sender_ || clients_.empty())
        return -1;

    // Fragment + FEC once, then multicast the prepared wire packets.
    sender_->prepare_frame(data, data_len, frame_seq, timestamp, keyframe);

    int total = 0;
    for (const auto& [addr, client] : clients_) {
        int n = sender_->send_prepared(addr);
        if (n > 0) total += n;
    }
    return total;
}

// ── Aggregate queries ────────────────────────────────────────────────

bool HostSession::idr_needed() const {
    for (const auto& [addr, client] : clients_) {
        if (client.idr_needed) return true;
    }
    return false;
}

void HostSession::clear_idr_needed() {
    for (auto& [addr, client] : clients_) {
        client.idr_needed = false;
    }
}

double HostSession::rtt_ms() const {
    double worst = 0.0;
    for (const auto& [addr, client] : clients_) {
        if (client.rtt_ms > worst) worst = client.rtt_ms;
    }
    return worst;
}

float HostSession::last_loss_rate() const {
    float worst = 0.0f;
    if (sender_) return sender_->last_loss_rate();
    for (const auto& [addr, client] : clients_) {
        if (client.loss_rate > worst) worst = client.loss_rate;
    }
    return worst;
}

uint32_t HostSession::probe_bw_bps() const {
    uint32_t lowest = 0;
    for (const auto& [addr, client] : clients_) {
        if (client.probe_bw_bps > 0) {
            if (lowest == 0 || client.probe_bw_bps < lowest)
                lowest = client.probe_bw_bps;
        }
    }
    return lowest;
}

bool HostSession::probe_pending() const {
    for (const auto& [addr, client] : clients_) {
        if (client.probe_pending) return true;
    }
    return false;
}

// ── Packet handling ──────────────────────────────────────────────────

ClientInfo* HostSession::find_client(const net::SocketAddr& addr) {
    auto it = clients_.find(addr);
    return it != clients_.end() ? &it->second : nullptr;
}

void HostSession::handle_packet(const uint8_t* data, size_t len, const net::SocketAddr& sender) {
    if (len < protocol::PacketHeader::WIRE_SIZE) return;

    auto header = protocol::PacketHeader::deserialize(data);
    const uint8_t* payload = data + protocol::PacketHeader::WIRE_SIZE;
    size_t payload_len = len - protocol::PacketHeader::WIRE_SIZE;

    // Update last_recv for known clients.
    if (auto* c = find_client(sender)) {
        c->last_recv_time = Clock::now();
    }

    switch (header.type) {
        case protocol::PacketType::Control:
            handle_hello(payload, payload_len, sender);
            break;
        case protocol::PacketType::Pong:
            handle_pong(payload, payload_len, sender);
            break;
        case protocol::PacketType::Input:
            handle_input(payload, payload_len);
            break;
        case protocol::PacketType::IdrRequest:
            if (auto* c = find_client(sender)) {
                c->idr_needed = true;
                log::info("HostSession", "Client requested IDR (frame loss recovery)");
            }
            break;
        case protocol::PacketType::NackRequest:
            if (sender_ && payload_len >= 3) {
                if (auto* c = find_client(sender)) {
                    uint16_t seq = payload[0] | (payload[1] << 8);
                    uint8_t count = payload[2];
                    if (payload_len >= 3u + count * 2u) {
                        std::vector<uint16_t> indices(count);
                        for (uint8_t i = 0; i < count; ++i) {
                            indices[i] = payload[3 + i * 2] | (payload[4 + i * 2] << 8);
                        }
                        sender_->handle_nack(seq, indices.data(), count, c->addr);
                    }
                }
            }
            break;
        case protocol::PacketType::FecReport:
            if (sender_ && payload_len >= 4) {
                if (auto* c = find_client(sender)) {
                    float loss_rate;
                    std::memcpy(&loss_rate, payload, 4);
                    c->loss_rate = loss_rate;
                    sender_->update_fec_from_loss(loss_rate);
                }
            }
            break;
        case protocol::PacketType::BwProbeAck:
            handle_bw_probe_ack(payload, payload_len, sender);
            break;
        default:
            break;
    }
}

void HostSession::handle_hello(const uint8_t* payload, size_t len,
                               const net::SocketAddr& sender) {
    if (len < sizeof(HELLO_MAGIC)) return;
    if (std::memcmp(payload, HELLO_MAGIC, sizeof(HELLO_MAGIC)) != 0) return;

    // Optional extension: hello payload may carry client audio port (u16 LE)
    // right after the magic. Older clients without audio omit these bytes.
    uint16_t client_audio_port = 0;
    if (len >= sizeof(HELLO_MAGIC) + 2) {
        client_audio_port = static_cast<uint16_t>(payload[sizeof(HELLO_MAGIC)])
            | (static_cast<uint16_t>(payload[sizeof(HELLO_MAGIC) + 1]) << 8);
    }

    // Send ACK.  Legacy ACK is 10 bytes (magic + 0x01 + 0x00).  We append a
    // codec byte (0=H.264, 1=HEVC) so the client can initialise the correct
    // decoder.  Older clients ignore trailing bytes, so this stays wire-compat.
    protocol::Packet ack;
    ack.header.type = protocol::PacketType::Control;
    ack.header.seq_no = 0;
    ack.header.timestamp = 0;
    ack.header.flags = 0;
    ack.payload.assign(HELLO_ACK, HELLO_ACK + sizeof(HELLO_ACK));
    ack.payload.push_back(static_cast<uint8_t>(codec_));
    ack.header.payload_len = static_cast<uint16_t>(ack.payload.size());

    auto wire = ack.serialize();
    socket_->send_to(wire.data(), wire.size(), sender);

    // Add or re-arm client.
    auto now = Clock::now();
    auto& client = clients_[sender];
    client.addr           = sender;
    client.connected_time = now;
    client.last_recv_time = now;
    client.last_ping_time = now;
    client.idr_needed     = true;
    client.probe_bw_bps   = 0;
    client.probe_pending  = false;

    state_ = SessionState::Connected;
    new_client_flag_ = true;

    // Register audio destination if the client sent its audio port.
    if (audio_sender_ && client_audio_port != 0) {
        net::SocketAddr audio_dest{};
        audio_dest.ip   = sender.ip;
        audio_dest.port = client_audio_port;
        audio_sender_->add_destination(audio_dest);
        log::info("HostSession", "Audio destination registered: port %u",
                  client_audio_port);
    }

    // Send BW probe to the new client.
    send_bw_probe(client);

    log::info("HostSession", "Client connected from %u.%u.%u.%u:%u (%zu total)",
        (sender.ip >> 0) & 0xFF, (sender.ip >> 8) & 0xFF,
        (sender.ip >> 16) & 0xFF, (sender.ip >> 24) & 0xFF, sender.port,
        clients_.size());
}

void HostSession::handle_pong(const uint8_t* payload, size_t len,
                              const net::SocketAddr& sender) {
    if (len < 4) return;
    auto* client = find_client(sender);
    if (!client) return;

    uint32_t seq = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24);
    if (seq == client->ping_seq) {
        auto now = Clock::now();
        client->rtt_ms = std::chrono::duration<double, std::milli>(now - client->ping_sent_time).count();
    }
}

void HostSession::send_ping(ClientInfo& client) {
    client.ping_seq++;
    client.ping_sent_time = Clock::now();

    protocol::Packet ping;
    ping.header.type = protocol::PacketType::Ping;
    ping.header.seq_no = 0;
    ping.header.timestamp = 0;
    ping.header.flags = 0;
    // Payload: seq(4) | rtt_us(4 LE). Client uses rtt_us to tune NACK timing.
    uint32_t rtt_us = static_cast<uint32_t>(client.rtt_ms * 1000.0);
    ping.payload.resize(8);
    ping.payload[0] = static_cast<uint8_t>(client.ping_seq & 0xFF);
    ping.payload[1] = static_cast<uint8_t>((client.ping_seq >> 8) & 0xFF);
    ping.payload[2] = static_cast<uint8_t>((client.ping_seq >> 16) & 0xFF);
    ping.payload[3] = static_cast<uint8_t>((client.ping_seq >> 24) & 0xFF);
    ping.payload[4] = static_cast<uint8_t>(rtt_us & 0xFF);
    ping.payload[5] = static_cast<uint8_t>((rtt_us >> 8) & 0xFF);
    ping.payload[6] = static_cast<uint8_t>((rtt_us >> 16) & 0xFF);
    ping.payload[7] = static_cast<uint8_t>((rtt_us >> 24) & 0xFF);
    ping.header.payload_len = 8;

    auto wire = ping.serialize();
    socket_->send_to(wire.data(), wire.size(), client.addr);
}

void HostSession::send_cursor_position(const protocol::CursorPositionMessage& msg) {
    if (!socket_ || clients_.empty()) return;
    protocol::Packet pkt;
    pkt.header.type        = protocol::PacketType::CursorPosition;
    pkt.header.seq_no      = 0;
    pkt.header.timestamp   = 0;
    pkt.header.flags       = 0;
    pkt.payload            = msg.serialize();
    pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());
    auto wire = pkt.serialize();
    for (auto& [addr, client] : clients_) {
        socket_->send_to(wire.data(), wire.size(), client.addr);
    }
}

void HostSession::send_stream_info(uint16_t width, uint16_t height) {
    if (!socket_ || clients_.empty()) return;
    protocol::StreamInfoMessage msg;
    msg.width  = width;
    msg.height = height;

    protocol::Packet pkt;
    pkt.header.type        = protocol::PacketType::StreamInfo;
    pkt.header.seq_no      = 0;
    pkt.header.timestamp   = 0;
    pkt.header.flags       = 0;
    pkt.payload            = msg.serialize();
    pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());
    auto wire = pkt.serialize();
    for (auto& [addr, client] : clients_) {
        socket_->send_to(wire.data(), wire.size(), client.addr);
    }
    log::info("HostSession", "Sent StreamInfo %ux%u to %zu client(s)",
              width, height, clients_.size());
}

void HostSession::send_cursor_shape(const protocol::CursorShapeMessage& msg) {
    if (!socket_ || clients_.empty()) return;

    // A typical 32x32 BGRA cursor is 4KB; large high-DPI shapes can hit
    // tens of KB. A single UDP packet gets IP-fragmented by the kernel,
    // and home WiFi gear commonly drops fragmented UDP — so we fragment
    // at the application layer and let the client reassemble.
    //
    // Per-fragment payload layout:
    //   [shape_id:4B LE][frag_idx:1B][frag_count:1B][chunk bytes...]
    // Each chunk carries a slice of the serialized CursorShapeMessage.
    const std::vector<uint8_t> body = msg.serialize();
    constexpr size_t CHUNK_SIZE = 1200;       // safely under typical MTU
    constexpr size_t FRAG_HEADER = 6;         // shape_id(4) + idx(1) + count(1)

    size_t total_chunks = (body.size() + CHUNK_SIZE - 1) / CHUNK_SIZE;
    if (total_chunks == 0) total_chunks = 1;
    if (total_chunks > 255) {
        // Shouldn't happen for sane cursors (<=256x256 caps at ~256KB).
        log::warn("HOST", "Cursor shape too large to fragment: %zu bytes", body.size());
        return;
    }

    for (size_t i = 0; i < total_chunks; ++i) {
        const size_t off = i * CHUNK_SIZE;
        const size_t len = std::min(CHUNK_SIZE, body.size() - off);

        protocol::Packet pkt;
        pkt.header.type      = protocol::PacketType::CursorShape;
        pkt.header.seq_no    = 0;
        pkt.header.timestamp = 0;
        pkt.header.flags     = 0;
        pkt.payload.resize(FRAG_HEADER + len);
        uint8_t* p = pkt.payload.data();
        std::memcpy(p, &msg.shape_id, 4);
        p[4] = static_cast<uint8_t>(i);
        p[5] = static_cast<uint8_t>(total_chunks);
        if (len > 0) std::memcpy(p + FRAG_HEADER, body.data() + off, len);
        pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());

        auto wire = pkt.serialize();
        for (auto& [addr, client] : clients_) {
            socket_->send_to(wire.data(), wire.size(), client.addr);
        }
    }
}

void HostSession::send_bw_probe(ClientInfo& client) {
    client.probe_id++;
    client.probe_pending = true;
    client.probe_bw_bps = 0;
    client.probe_sent_time = Clock::now();

    for (uint16_t i = 0; i < BW_PROBE_COUNT; ++i) {
        protocol::Packet pkt;
        pkt.header.type = protocol::PacketType::BwProbe;
        pkt.header.seq_no = 0;
        pkt.header.timestamp = 0;
        pkt.header.flags = 0;
        // Payload: probe_id(2B LE) + index(2B LE) + count(2B LE) + padding
        pkt.payload.resize(BW_PROBE_SIZE, 0);
        pkt.payload[0] = static_cast<uint8_t>(client.probe_id & 0xFF);
        pkt.payload[1] = static_cast<uint8_t>((client.probe_id >> 8) & 0xFF);
        pkt.payload[2] = static_cast<uint8_t>(i & 0xFF);
        pkt.payload[3] = static_cast<uint8_t>((i >> 8) & 0xFF);
        pkt.payload[4] = static_cast<uint8_t>(BW_PROBE_COUNT & 0xFF);
        pkt.payload[5] = static_cast<uint8_t>((BW_PROBE_COUNT >> 8) & 0xFF);
        pkt.header.payload_len = BW_PROBE_SIZE;

        auto wire = pkt.serialize();
        socket_->send_to(wire.data(), wire.size(), client.addr);
    }
    log::info("HostSession", "Sent BW probe (%d x %dB), id=%u",
              BW_PROBE_COUNT, BW_PROBE_SIZE, client.probe_id);
}

void HostSession::handle_bw_probe_ack(const uint8_t* payload, size_t len,
                                      const net::SocketAddr& sender) {
    if (len < 8) return;
    auto* client = find_client(sender);
    if (!client) return;

    uint16_t ack_id = payload[0] | (static_cast<uint16_t>(payload[1]) << 8);
    if (ack_id != client->probe_id) return;

    uint32_t bw_bps = payload[4]
                    | (static_cast<uint32_t>(payload[5]) << 8)
                    | (static_cast<uint32_t>(payload[6]) << 16)
                    | (static_cast<uint32_t>(payload[7]) << 24);
    client->probe_bw_bps = bw_bps;
    client->probe_pending = false;
    uint16_t received = payload[2] | (static_cast<uint16_t>(payload[3]) << 8);
    log::info("HostSession", "BW probe result: %u kbps (%u/%u received)",
              bw_bps / 1000, received, BW_PROBE_COUNT);
}

void HostSession::handle_input(const uint8_t* payload, size_t len) {
    // Accept input from any connected client.
    protocol::InputEvent event;
    if (protocol::InputEvent::deserialize(payload, len, event)) {
        if (input_injector_) input_injector_->inject(event);
    }
}

} // namespace deskbeam::host
