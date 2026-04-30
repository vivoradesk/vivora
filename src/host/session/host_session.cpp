#include "host/session/host_session.h"
#include "common/crypto/host_identity.h"
#include "common/crypto/packet_crypto.h"
#include "common/net/stun_client.h"
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
    // Load or mint the host's long-term Curve25519 keypair BEFORE binding —
    // if crypto setup fails we don't want a half-initialised session.
    if (!crypto::load_or_create_host_identity(host_identity_, host_identity_path_)) {
        log::error("HostSession", "Failed to load or create host identity");
        return false;
    }
    log::info("HostSession", "Host public key: %s",
              crypto::hex_encode(host_identity_.public_key, 32).c_str());

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

    // STUN runs on the *same* socket so the NAT binding the STUN server
    // observes is exactly the binding any future peer will reach. No client
    // has connected yet — the socket is quiet. Failure is non-fatal: LAN
    // usage doesn't need a reflexive address and a misconfigured or
    // unreachable STUN server shouldn't block the session.
    if (stun_server_.ip != 0 && stun_server_.port != 0) {
        reflexive_addr_ = net::StunClient::discover(stun_server_, *socket_);
        if (reflexive_addr_.ip != 0) {
            log::info("HostSession",
                "Reflexive address: %u.%u.%u.%u:%u (share this with the peer)",
                (reflexive_addr_.ip >> 0) & 0xFF, (reflexive_addr_.ip >> 8) & 0xFF,
                (reflexive_addr_.ip >> 16) & 0xFF, (reflexive_addr_.ip >> 24) & 0xFF,
                reflexive_addr_.port);
        } else {
            log::warn("HostSession", "STUN discovery failed — reflexive address unknown");
        }
    }

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
                            uint16_t frame_seq, uint32_t timestamp, bool keyframe,
                            bool fec_enabled) {
    if (state_ != SessionState::Connected || !sender_ || clients_.empty())
        return -1;

    // Fragment + FEC once, then multicast the prepared wire packets.  Each
    // client seals the same plaintext wires through its own send_cs, so the
    // FEC plan is shared but the on-wire bytes differ per destination.
    sender_->prepare_frame(data, data_len, frame_seq, timestamp, keyframe, fec_enabled);

    int total = 0;
    for (auto& [addr, client] : clients_) {
        if (!client.handshake_complete) continue;
        int n = sender_->send_prepared(addr, &client.send_cs);
        if (n > 0) total += n;
    }
    return total;
}

int HostSession::flush_video_fec(uint16_t frame_seq, uint32_t timestamp) {
    if (state_ != SessionState::Connected || !sender_ || clients_.empty())
        return 0;
    if (!sender_->flush_pending_fec(frame_seq, timestamp)) return 0;
    int total = 0;
    for (auto& [addr, client] : clients_) {
        if (!client.handshake_complete) continue;
        int n = sender_->send_prepared(addr, &client.send_cs);
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

uint16_t HostSession::min_perf_target_fps() const {
    if (clients_.empty()) return 60;
    uint16_t lowest = 60;
    for (const auto& [addr, client] : clients_) {
        if (client.perf_target_fps < lowest) lowest = client.perf_target_fps;
    }
    if (lowest < 15) lowest = 15;
    return lowest;
}

// ── Packet handling ──────────────────────────────────────────────────

ClientInfo* HostSession::find_client(const net::SocketAddr& addr) {
    auto it = clients_.find(addr);
    return it != clients_.end() ? &it->second : nullptr;
}

void HostSession::handle_packet(const uint8_t* data, size_t len, const net::SocketAddr& sender) {
    if (len < protocol::PacketHeader::WIRE_SIZE) return;

    auto header = protocol::PacketHeader::deserialize(data);

    ClientInfo* client = find_client(sender);
    if (client) client->last_recv_time = Clock::now();

    // Control packets carry Noise handshake frames and travel in the clear.
    // handle_hello() is responsible for validating / installing cipher state.
    if (header.type == protocol::PacketType::Control) {
        const uint8_t* payload = data + protocol::PacketHeader::WIRE_SIZE;
        const size_t   payload_len = len - protocol::PacketHeader::WIRE_SIZE;
        handle_hello(payload, payload_len, sender);
        return;
    }

    // Every other packet type MUST be sealed.  Drop anything from an unknown
    // sender or a client whose handshake hasn't finished — there's nothing
    // safe we can do with pre-handshake traffic.
    if (!client || !client->handshake_complete) return;

    uint8_t opened[RECV_BUF_SIZE];
    size_t  opened_len = crypto::open_packet(data, len, client->recv_cs, opened);
    if (opened_len == 0) {
        // AEAD rejected the packet (wrong key, tamper, replay). UDP reordering
        // can push a late original past a retx-with-newer-nonce and get it
        // dropped here — that's harmless because the newer packet already
        // filled the fragment slot.  Keep quiet to avoid flooding the log.
        return;
    }

    auto h = protocol::PacketHeader::deserialize(opened);
    const uint8_t* payload     = opened + protocol::PacketHeader::WIRE_SIZE;
    const size_t   payload_len = opened_len - protocol::PacketHeader::WIRE_SIZE;

    switch (h.type) {
        case protocol::PacketType::Pong:
            handle_pong(payload, payload_len, sender);
            break;
        case protocol::PacketType::Input:
            handle_input(payload, payload_len);
            break;
        case protocol::PacketType::IdrRequest:
            client->idr_needed = true;
            log::info("HostSession", "Client requested IDR (frame loss recovery)");
            break;
        case protocol::PacketType::NackRequest:
            if (sender_ && payload_len >= 3) {
                uint16_t seq = payload[0] | (payload[1] << 8);
                uint8_t count = payload[2];
                if (payload_len >= 3u + count * 2u) {
                    std::vector<uint16_t> indices(count);
                    for (uint8_t i = 0; i < count; ++i) {
                        indices[i] = payload[3 + i * 2] | (payload[4 + i * 2] << 8);
                    }
                    sender_->handle_nack(seq, indices.data(), count,
                                         client->addr, &client->send_cs);
                }
            }
            break;
        case protocol::PacketType::FecReport:
            if (sender_ && payload_len >= 4) {
                float    loss_rate;
                uint32_t delta_failed = 0;
                std::memcpy(&loss_rate, payload, 4);
                if (payload_len >= 8) {
                    std::memcpy(&delta_failed, payload + 4, 4);
                }
                client->loss_rate = loss_rate;
                sender_->update_fec_from_loss(loss_rate, delta_failed);
            }
            break;
        case protocol::PacketType::PerfReport:
            // Phase B: just absorb the report and log.  Adaptive-fps
            // application (Phase C) will route into the encoder via
            // a HostPlatform method.
            if (payload_len >= 4) {
                uint16_t target_fps = static_cast<uint16_t>(payload[0])
                                    | (static_cast<uint16_t>(payload[1]) << 8);
                uint8_t  reject_pct = payload[2];
                uint8_t  drop_pct   = payload[3];
                client->perf_target_fps = target_fps;
                client->perf_reject_pct = reject_pct;
                client->perf_drop_pct   = drop_pct;
                log::info("HostSession",
                          "PerfReport: target_fps=%u reject=%u%% drop=%u%%",
                          target_fps, reject_pct, drop_pct);
            }
            break;
        case protocol::PacketType::BwProbeAck:
            handle_bw_probe_ack(payload, payload_len, sender);
            break;
        default:
            break;
    }
}

bool HostSession::send_sealed(ClientInfo& client, const std::vector<uint8_t>& wire) {
    uint8_t sealed[RECV_BUF_SIZE];
    size_t  sealed_len = crypto::seal_packet(wire.data(), wire.size(),
                                             client.send_cs, sealed);
    if (sealed_len == 0) {
        log::warn("HostSession", "seal_packet failed (nonce exhausted?)");
        return false;
    }
    int r = socket_->send_to(sealed, sealed_len, client.addr);
    return r >= 0;
}

void HostSession::handle_hello(const uint8_t* payload, size_t len,
                               const net::SocketAddr& sender) {
    // The payload is a full Noise_NK msg1: 32 ephemeral pubkey + encrypted
    // payload + 16 tag.  The encrypted payload carries the same info the
    // legacy plaintext HELLO did: HELLO_MAGIC + optional client audio port.
    //
    // If the client retries HELLO (because msg2 was dropped), we always
    // build a fresh HandshakeStateNK — the client will have picked a new
    // ephemeral too, so the old handshake's state is worthless.
    auto handshake = std::make_unique<crypto::HandshakeStateNK>();
    if (!handshake->init_responder(host_identity_)) return;

    uint8_t inner[128] = {};
    int inner_len = handshake->read_message(payload, len, inner, sizeof(inner));
    if (inner_len < 0) {
        log::warn("HostSession", "Noise msg1 rejected from %u.%u.%u.%u:%u",
                  (sender.ip >> 0) & 0xFF, (sender.ip >> 8) & 0xFF,
                  (sender.ip >> 16) & 0xFF, (sender.ip >> 24) & 0xFF, sender.port);
        return;
    }
    if (inner_len < static_cast<int>(sizeof(HELLO_MAGIC))) return;
    if (std::memcmp(inner, HELLO_MAGIC, sizeof(HELLO_MAGIC)) != 0) return;

    uint16_t client_audio_port = 0;
    if (inner_len >= static_cast<int>(sizeof(HELLO_MAGIC)) + 2) {
        client_audio_port = static_cast<uint16_t>(inner[sizeof(HELLO_MAGIC)])
            | (static_cast<uint16_t>(inner[sizeof(HELLO_MAGIC) + 1]) << 8);
    }

    // Build msg2: HELLO_ACK || codec byte, encrypted inside the Noise frame.
    uint8_t ack_inner[32];
    std::memcpy(ack_inner, HELLO_ACK, sizeof(HELLO_ACK));
    ack_inner[sizeof(HELLO_ACK)] = static_cast<uint8_t>(codec_);
    const size_t ack_inner_len = sizeof(HELLO_ACK) + 1;

    uint8_t msg2_wire[128];
    size_t msg2_len = handshake->write_message(ack_inner, ack_inner_len,
                                               msg2_wire, sizeof(msg2_wire));
    if (msg2_len == 0) {
        log::warn("HostSession", "Noise msg2 write failed");
        return;
    }

    protocol::Packet ack;
    ack.header.type = protocol::PacketType::Control;
    ack.header.seq_no = 0;
    ack.header.timestamp = 0;
    ack.header.flags = 0;
    ack.payload.assign(msg2_wire, msg2_wire + msg2_len);
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

    // Derive transport cipher pairs — main (video/control) and audio — from
    // the same Noise HKDF.  The handshake object can go away now; keys are
    // committed to CipherStates.
    if (!handshake->finalize(client.send_cs,       client.recv_cs,
                             client.audio_send_cs, client.audio_recv_cs)) {
        log::error("HostSession", "Noise finalize failed — transport not keyed");
        clients_.erase(sender);
        return;
    }
    client.handshake.reset();  // not needed anymore
    client.handshake_complete = true;

    state_ = SessionState::Connected;
    new_client_flag_ = true;

    // Register audio destination if the client sent its audio port.
    if (audio_sender_ && client_audio_port != 0) {
        net::SocketAddr audio_dest{};
        audio_dest.ip   = sender.ip;
        audio_dest.port = client_audio_port;
        audio_sender_->add_destination(audio_dest, &client.audio_send_cs);
        log::info("HostSession", "Audio destination registered: %u.%u.%u.%u:%u",
                  (audio_dest.ip >> 0) & 0xFF, (audio_dest.ip >> 8) & 0xFF,
                  (audio_dest.ip >> 16) & 0xFF, (audio_dest.ip >> 24) & 0xFF,
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
    send_sealed(client, wire);
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
        if (!client.handshake_complete) continue;
        send_sealed(client, wire);
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
        if (!client.handshake_complete) continue;
        send_sealed(client, wire);
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
            if (!client.handshake_complete) continue;
            send_sealed(client, wire);
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
        send_sealed(client, wire);
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

std::string HostSession::host_public_key_hex() const {
    return crypto::hex_encode(host_identity_.public_key, 32);
}

} // namespace deskbeam::host
