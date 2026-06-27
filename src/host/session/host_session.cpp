#include "host/session/host_session.h"
#include "common/crypto/host_identity.h"
#include "common/crypto/packet_crypto.h"
#include "common/net/relay_protocol.h"
#include "common/net/rendezvous_protocol.h"
#include "common/net/stun_client.h"
#include "common/protocol/packet.h"
#include "common/protocol/input_event.h"
#include "common/utils/log.h"
#include "common/utils/peer_code.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>

namespace vivora::host {

// Handshake magic: "VIVORA" + version byte
static const uint8_t HELLO_MAGIC[] = { 'D','E','S','K','B','E','A','M', 0x01 };
static const uint8_t HELLO_ACK[]   = { 'D','E','S','K','B','E','A','M', 0x01, 0x00 };

bool HostSession::start(uint16_t port) {
    // Load or mint the host's long-term Curve25519 keypair BEFORE binding —
    // if crypto setup fails we don't want a half-initialised session.
    if (!crypto::load_or_create_host_identity(host_identity_, host_identity_path_)) {
        log::error("HostSession", "Failed to load or create host identity");
        return false;
    }
    {
        const std::string code = peer_code::encode(host_identity_.public_key);
        log::info("HostSession", "Peer code: %s   (share this with your peer)",
                  code.c_str());
        log::info("HostSession", "Host public key: %s",
                  crypto::hex_encode(host_identity_.public_key, 32).c_str());
    }

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
    // Per-frame pooled FEC (VIV-82): one RS group per frame, parity as a % of
    // the frame — burst-resilient.  Opt-in while we validate; default legacy.
    if (const char* p = std::getenv("VIVORA_FEC_PERFRAME")) {
        if (std::atoi(p) != 0) {
            sender_->set_per_frame_fec(true);
            log::info("HostSession", "Per-frame pooled FEC enabled (VIV-82)");
        }
    }
    if (const char* p = std::getenv("VIVORA_KF_PACE")) {
        if (std::atoi(p) != 0) {
            kf_pace_enabled_ = true;
            log::info("HostSession", "Keyframe send-pacing enabled (VIV-82)");
        }
    }

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

    // Rendezvous registration — same socket as the future video traffic so
    // the binding the rdv server sees is the same one peers will hit.
    if (rendezvous_addr_.ip != 0) {
        send_rendezvous_register();
        last_rdv_send_ = Clock::now();
    }

    // Relay BIND (manual mode for v1) — must come AFTER VideoSender exists
    // because relay_bind_blocking flips its relay_active_ flag on success.
    if (relay_session_set_ && relay_addr_.ip != 0) {
        if (!relay_bind_blocking()) {
            log::error("HostSession", "Relay required but BIND failed — refusing to start");
            return false;
        }
    }

    // Paced sender (VIV-82): created last, after the synchronous STUN / relay
    // BIND exchanges, so those keep using the socket directly while it was
    // quiet.  From here on every sealed wire (video + control) goes out spread
    // by the send thread instead of as a WiFi-dropping micro-burst.  Default
    // 100 µs/packet (~88 Mbps cap, measured loss-free on WiFi); 0 = off.
    int pace_gap_us = 0;  // default off until the pacer is validated (VIV-82)
    if (const char* p = std::getenv("VIVORA_SEND_PACING_US")) pace_gap_us = std::atoi(p);
    if (pace_gap_us > 0) {
        paced_sender_ = std::make_unique<PacedSender>(*socket_, pace_gap_us);
        sender_->set_paced_sender(paced_sender_.get());
        log::info("HostSession", "Paced sender enabled (gap=%d us)", pace_gap_us);
    }

    return true;
}

void HostSession::stop() {
    // Stop the paced send thread (joins) before closing the socket it sends
    // on (VIV-82).
    paced_sender_.reset();
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

        // VIV-53 approval gate: latch transitions Pending → Approved /
        // Rejected without re-querying the mutex on every send_frame.
        if (approval_gate_ && client.handshake_complete && !client.approved) {
            const uint64_t key = host::HostApprovalGate::make_key(addr.ip, addr.port);
            const auto s = approval_gate_->get_state(key);
            if (s == host::ApprovalState::Approved) {
                client.approved = true;
                client.grant = approval_gate_->get_grant(key);   // VIV-60
                log::info("HostSession",
                    "Client approved (input=%d clipboard=%d file=%d)",
                    client.grant.input, client.grant.clipboard,
                    client.grant.file_transfer);
                client.idr_needed = true;   // fresh stream → start with keyframe
                new_client_flag_ = true;    // host_loop fires the IDR encode
                // Register the audio destination that was stashed at
                // handshake completion but held back pending approval.
                if (audio_sender_ && client.audio_port_pending != 0
                    && !client.audio_registered) {
                    client.audio_dest.ip   = addr.ip;
                    client.audio_dest.port = client.audio_port_pending;
                    audio_sender_->add_destination(client.audio_dest,
                                                   &client.audio_send_cs);
                    client.audio_registered = true;
                    log::info("HostSession",
                        "Audio destination registered (post-approval): %u.%u.%u.%u:%u",
                        (addr.ip >> 0) & 0xFF, (addr.ip >> 8) & 0xFF,
                        (addr.ip >> 16) & 0xFF, (addr.ip >> 24) & 0xFF,
                        client.audio_port_pending);
                }
                log::info("HostSession", "Client approved — streaming starts");
            } else if (s == host::ApprovalState::Rejected) {
                log::info("HostSession", "Client rejected — disconnecting");
                timed_out.push_back(addr);
                continue;
            }
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

        // Deferred BW probe: ~250ms after handshake completion, send the
        // 1000-packet bandwidth measurement burst.  We cannot send it
        // synchronously inside handle_hello() because the burst dominates
        // the WiFi radio for ~50-100ms and reliably wipes out the
        // HELLO_ACK that was sent moments earlier — clients then time
        // out on direct LAN and fall over to relay even when direct
        // would work for the real stream.
        if (client.handshake_complete && !client.probe_scheduled) {
            auto since_connect = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - client.connected_time).count();
            if (since_connect >= 250) {
                send_bw_probe(client);
                client.probe_scheduled = true;
            }
        }
    }

    // Remove timed-out clients.  Also evict their audio destinations
    // from AudioSender — that list holds a raw pointer into the
    // CipherState that lives inside the per-client struct in clients_.
    // Without the eviction the pointer dangles, sealed audio packets
    // ship encrypted with a freed cipher and the receiver silently
    // drops them on AEAD authentication failure.  This was the cause
    // of "audio doesn't recover after Mac wake" — client timed out
    // during sleep, host kept sending garbled-encrypted audio to the
    // still-listening client socket, only manual reconnect (which
    // re-adds the destination with a fresh cipher) restored sound.
    for (const auto& addr : timed_out) {
        log::warn("HostSession", "Client %u.%u.%u.%u:%u timed out",
            (addr.ip >> 0) & 0xFF, (addr.ip >> 8) & 0xFF,
            (addr.ip >> 16) & 0xFF, (addr.ip >> 24) & 0xFF, addr.port);
        if (audio_sender_) {
            auto it = clients_.find(addr);
            if (it != clients_.end()) {
                audio_sender_->remove_destination(it->second.audio_dest);
            }
        }
        if (approval_gate_) {
            approval_gate_->forget(host::HostApprovalGate::make_key(addr.ip, addr.port));
        }
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

    // Refresh rendezvous registration well before TTL — TTL is 60s, send
    // every 30s so a single dropped keepalive doesn't drop us off the map.
    // GUI Refresh button can also force-fire via request_rendezvous_refresh.
    if (rendezvous_addr_.ip != 0) {
        const bool forced = rendezvous_refresh_pending_.exchange(false);
        auto since_rdv = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_rdv_send_).count();
        if (forced || since_rdv >= RDV_KEEPALIVE_S) {
            send_rendezvous_register();
            last_rdv_send_ = now;
            if (forced) log::info("HostSession", "Forced rendezvous refresh");
        }
    }

    // Keep the relay binding warm.
    if (relay_active_) {
        const auto since_kp = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_relay_keepalive_).count();
        if (since_kp >= RELAY_KEEPALIVE_S) {
            relay_send_keepalive();
        }
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

    // Send-pacing (VIV-82 option B): drain the frame a packet per ~100µs across
    // host-loop ticks instead of one burst the WiFi AP drops.  Applies to every
    // non-tiny frame — P-frames burst-lose too (smaller), which was cutting the
    // bitrate to 3-5 Mbps.  Tiny frames and heartbeats send immediately.
    if (kf_pace_enabled_ && fec_enabled &&
        sender_->prepared_wires().size() > PACE_MIN_PACKETS) {
        const auto& nw = sender_->prepared_wires();
        // If the previous frame is STILL draining, do NOT flush its tail as a
        // burst — that is exactly what lost the big 3440x1440 keyframe (it paces
        // in ~17ms > one 16ms frame interval, so the next frame used to flush
        // its tail) and drove the ~7s freeze cycle.  Instead APPEND this frame's
        // wires so the keyframe finishes pacing and this frame paces right after
        // it.  The pacer drains far faster than frames arrive (~10k vs ~2k
        // pkt/s), so the queue only builds during a keyframe overrun and then
        // drains — it never grows unbounded (VIV-82).
        if (kf_pacer_.active && kf_pacer_.pos < kf_pacer_.wires.size()) {
            if (kf_pacer_.pos > 0) {  // drop already-sent wires, keep bounded
                kf_pacer_.wires.erase(kf_pacer_.wires.begin(),
                                      kf_pacer_.wires.begin() + kf_pacer_.pos);
                kf_pacer_.pos = 0;
            }
            kf_pacer_.wires.insert(kf_pacer_.wires.end(), nw.begin(), nw.end());
            drain_kf_pacer();
            return static_cast<int>(nw.size());
        }
        kf_pacer_.wires = nw;   // fresh job
        kf_pacer_.dests.clear();
        for (auto& [addr, client] : clients_) {
            if (client.handshake_complete && client.approved)
                kf_pacer_.dests.push_back(addr);
        }
        kf_pacer_.pos = 0;
        kf_pacer_.last_chunk = {};   // first chunk fires immediately
        kf_pacer_.active = true;
        drain_kf_pacer();            // emit the first chunk now
        return static_cast<int>(kf_pacer_.wires.size());
    }

    int total = 0;
    for (auto& [addr, client] : clients_) {
        if (!client.handshake_complete) continue;
        if (!client.approved) continue;   // VIV-53 approval gate
        int n = sender_->send_prepared(addr, &client.send_cs);
        if (n > 0) total += n;
    }
    return total;
}

void HostSession::drain_kf_pacer() {
    if (!kf_pacer_.active || !sender_) return;
    const auto now = Clock::now();
    if (kf_pacer_.last_chunk.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::microseconds>(
            now - kf_pacer_.last_chunk).count() < KF_PACE_GAP_US) {
        return;  // chunk interval not elapsed yet
    }
    const size_t begin = kf_pacer_.pos;
    const size_t end   = std::min(begin + KF_PACE_CHUNK, kf_pacer_.wires.size());
    for (const auto& addr : kf_pacer_.dests) {
        auto it = clients_.find(addr);
        if (it == clients_.end() || !it->second.approved) continue;
        sender_->send_wire_range(kf_pacer_.wires, begin, end, addr,
                                 &it->second.send_cs);
    }
    kf_pacer_.pos = end;
    kf_pacer_.last_chunk = now;
    if (kf_pacer_.pos >= kf_pacer_.wires.size()) {
        kf_pacer_.active = false;
        kf_pacer_.wires.clear();
        kf_pacer_.dests.clear();
    }
}

int HostSession::flush_video_fec(uint16_t frame_seq, uint32_t timestamp) {
    if (state_ != SessionState::Connected || !sender_ || clients_.empty())
        return 0;
    if (!sender_->flush_pending_fec(frame_seq, timestamp)) return 0;
    int total = 0;
    for (auto& [addr, client] : clients_) {
        if (!client.handshake_complete) continue;
        if (!client.approved) continue;   // VIV-53 approval gate
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

float HostSession::last_effective_loss() const {
    uint8_t worst = 0;
    for (const auto& [addr, client] : clients_) {
        worst = std::max(worst, std::max(client.perf_drop_pct,
                                         client.perf_reject_pct));
    }
    return static_cast<float>(worst) / 100.0f;
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
    // Demux rendezvous traffic before anything else — the wire magic ('D'
    // 0x44) doesn't collide with any PacketType enum value, so this is a
    // safe top-of-loop check.
    if (len >= 4 && data[0] == 'D' && data[1] == 'B' && data[2] == 'R' && data[3] == 'V') {
        handle_rendezvous_packet(data, len, sender);
        return;
    }
    // Relay-forwarded payload: only accept when it comes from our relay
    // endpoint, then unwrap and recurse with the inner bytes attributed
    // to the (single) bound client.  We use clients_.begin()'s addr as
    // the synthetic sender — v1 binds one peer at a time.
    if (relay_active_ && sender == relay_addr_
        && len >= 4 && data[0] == 'D' && data[1] == 'B'
        && data[2] == 'R' && data[3] == 'L') {
        namespace rly = net::relay;
        rly::MsgType t; size_t poff = 0, plen = 0;
        if (!rly::parse_header(data, len, t, poff, plen)) return;
        if (t != rly::MsgType::Data) return;
        uint8_t aid[8]; const uint8_t* inner = nullptr; size_t inner_len = 0;
        if (!rly::decode_data(data + poff, plen, aid, &inner, &inner_len)) return;
        if (std::memcmp(aid, relay_alloc_id_, 8) != 0) return;
        // The relay is a single conduit — synthesise a stable sender addr
        // so per-client tracking keys consistently.  Use 0.0.0.1:0 as a
        // sentinel that no real peer would have; the same struct identity
        // is what clients_.find() keys on.
        net::SocketAddr synth{ net::parse_ip("0.0.0.1"), 0 };
        handle_packet(inner, inner_len, synth);
        return;
    }

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
            // VIV-53: drop input from clients still awaiting approval.
            // Without this gate the host's mouse/keyboard would jump
            // around immediately on connect, before the user even saw
            // the approval popup.
            // VIV-60: also drop when the connection was granted view-only
            // (input capability off) — the viewer sees the screen but can't
            // control it.
            if (client && client->approved && client->grant.input) {
                handle_input(payload, payload_len);
            }
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
    return transport_send(sealed, sealed_len, client.addr) >= 0;
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

    // VIV-61: with the IK handshake the host now learns the connecting
    // viewer's long-term static pubkey.  Capture it here — while the
    // handshake object is still alive (it's reset right after finalize) —
    // so the approval gate can surface the viewer's identity / fingerprint.
    uint8_t client_pubkey[32] = {};
    const bool have_client_pubkey = handshake->peer_static_key(client_pubkey);

    uint16_t client_audio_port = 0;
    if (inner_len >= static_cast<int>(sizeof(HELLO_MAGIC)) + 2) {
        client_audio_port = static_cast<uint16_t>(inner[sizeof(HELLO_MAGIC)])
            | (static_cast<uint16_t>(inner[sizeof(HELLO_MAGIC) + 1]) << 8);
    }

    // Optional viewer device name (VIV-61): MAGIC(9) | port(2) | len(1) | name.
    // Self-asserted but integrity-protected by the Noise transcript — a
    // display hint for the approval prompt, as trustworthy as the static key.
    std::string client_name;
    {
        const int name_off = static_cast<int>(sizeof(HELLO_MAGIC)) + 2;
        if (inner_len > name_off) {
            const int nlen = inner[name_off];
            if (nlen > 0 && name_off + 1 + nlen <= inner_len) {
                client_name.assign(reinterpret_cast<const char*>(inner + name_off + 1),
                                   static_cast<size_t>(nlen));
            }
        }
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
    transport_send(wire.data(), wire.size(), sender);

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
    client.probe_scheduled = false;   // re-arm deferred probe on each HELLO

    // Seed the idle timer at handshake completion.  Without this a
    // client that connects and never sends any input (looks at a static
    // screen) leaves last_input_time_ at epoch zero, seconds_since_
    // last_input() returns 0, and the GUI idle-timeout never fires.
    last_input_time_ = now;

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

    // VIV-53: connection approval gate.  When the GUI has installed
    // one, every new client lands in Pending and won't receive video
    // or audio frames until the user clicks Accept in the popup that
    // the gate's callback raises.  audio_port is stashed in
    // audio_port_pending and registered with AudioSender only once
    // approval flips Approved (handled in poll()).
    //
    // CLI mode (no gate) or VIVORA_AUTO_ACCEPT=1 dev override → mark
    // Approved immediately and register audio right here, matching
    // pre-VIV-53 behaviour.
    client.audio_port_pending = client_audio_port;
    const bool dev_auto = std::getenv("VIVORA_AUTO_ACCEPT") != nullptr;
    if (!approval_gate_ || dev_auto) {
        client.approved = true;
        if (audio_sender_ && client_audio_port != 0) {
            client.audio_dest.ip   = sender.ip;
            client.audio_dest.port = client_audio_port;
            audio_sender_->add_destination(client.audio_dest, &client.audio_send_cs);
            client.audio_registered = true;
            log::info("HostSession", "Audio destination registered: %u.%u.%u.%u:%u",
                      (client.audio_dest.ip >> 0) & 0xFF, (client.audio_dest.ip >> 8) & 0xFF,
                      (client.audio_dest.ip >> 16) & 0xFF, (client.audio_dest.ip >> 24) & 0xFF,
                      client_audio_port);
        }
        if (approval_gate_) approval_gate_->preapprove(
            host::HostApprovalGate::make_key(sender.ip, sender.port));
    } else {
        // Pending — fire the popup.  The viewer's peer code + pubkey
        // fingerprint come from the static key the IK handshake just
        // authenticated (VIV-61); empty only if the key was unavailable.
        std::string peer_code, pubkey_hex;
        if (have_client_pubkey) {
            pubkey_hex = crypto::hex_encode(client_pubkey, 32);
            peer_code  = peer_code::encode(client_pubkey);
        }
        char ip[32];
        std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u:%u",
            (sender.ip >> 0) & 0xFF, (sender.ip >> 8) & 0xFF,
            (sender.ip >> 16) & 0xFF, (sender.ip >> 24) & 0xFF, sender.port);
        approval_gate_->notify_pending(
            host::HostApprovalGate::make_key(sender.ip, sender.port),
            peer_code, pubkey_hex, ip, client_name);
        log::info("HostSession", "Client %s awaiting approval", ip);
    }

    // BW probe is delayed by ~250ms (see poll()).  Sending it here would
    // blast 1000×1200B at the WiFi radio immediately after HELLO_ACK,
    // which on busy 2.4GHz channels has been observed to fully drown
    // out the HELLO_ACK itself — Linux client never receives msg2,
    // assumes timeout and falls over to relay even when direct LAN
    // would have worked perfectly fine for the actual stream.

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

void HostSession::send_encoder_bitrate(uint32_t kbps) {
    if (!socket_ || clients_.empty()) return;
    protocol::Packet pkt;
    pkt.header.type        = protocol::PacketType::HostStats;
    pkt.header.seq_no      = 0;
    pkt.header.timestamp   = 0;
    pkt.header.flags       = 0;
    pkt.payload.resize(4);
    pkt.payload[0] = static_cast<uint8_t>(kbps & 0xFF);
    pkt.payload[1] = static_cast<uint8_t>((kbps >> 8) & 0xFF);
    pkt.payload[2] = static_cast<uint8_t>((kbps >> 16) & 0xFF);
    pkt.payload[3] = static_cast<uint8_t>((kbps >> 24) & 0xFF);
    pkt.header.payload_len = 4;
    auto wire = pkt.serialize();
    for (auto& [addr, client] : clients_) {
        if (!client.handshake_complete) continue;
        send_sealed(client, wire);
    }
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
        // Touch last-input for the idle-timeout feature.  Updated for
        // every event, not just successful injection, because client
        // intent is what matters — we'd otherwise mistake an injection
        // failure for an idle session.
        last_input_time_ = Clock::now();
    }
}

int64_t HostSession::seconds_since_last_input() const {
    if (last_input_time_.time_since_epoch().count() == 0) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(
        Clock::now() - last_input_time_).count();
}

void HostSession::disconnect_all_clients() {
    if (clients_.empty()) return;
    log::info("HostSession", "Force-disconnecting %zu client(s) (idle timeout)",
              clients_.size());
    clients_.clear();
    state_ = SessionState::Disconnected;
    // Socket stays open — new HELLOs from fresh clients will land in
    // handle_hello and reconnect normally.
}

std::string HostSession::host_public_key_hex() const {
    return crypto::hex_encode(host_identity_.public_key, 32);
}

void HostSession::set_relay(const net::SocketAddr& addr, const uint8_t session_id[32]) {
    relay_addr_ = addr;
    std::memcpy(relay_session_id_, session_id, 32);
    relay_session_set_ = true;
}

void HostSession::set_relay_license(const uint8_t token[95]) {
    std::memcpy(relay_license_, token, 95);
    relay_license_set_ = true;
}

int HostSession::transport_send(const uint8_t* data, size_t len, const net::SocketAddr& peer) {
    if (!socket_) return -1;
    // Wrap in DBRL DATA only when the peer is the relay endpoint itself
    // (i.e. this client reached us through the relay so the relay's
    // ip:port is the only path back to it).  Direct-LAN clients have a
    // real ip:port stored in client.addr; we send them straight via
    // the wire even when relay_active_ is true (host BIND'd at startup
    // for OTHER potential clients).  Without this check transport_send
    // funnelled every direct client through the relay too, silently
    // breaking the entire direct-LAN path.
    if (relay_active_ && peer == relay_addr_) {
        namespace rly = net::relay;
        uint8_t buf[rly::MAX_DATA_PACKET];
        const size_t n = rly::encode_data(buf, sizeof(buf), relay_alloc_id_, data, len);
        if (n == 0) return -1;
        return paced_sender_ ? paced_sender_->send_to(buf, n, relay_addr_)
                             : socket_->send_to(buf, n, relay_addr_);
    }
    return paced_sender_ ? paced_sender_->send_to(data, len, peer)
                         : socket_->send_to(data, len, peer);
}

void HostSession::relay_send_keepalive() {
    if (!relay_active_ || !socket_) return;
    namespace rly = net::relay;
    rly::KeepalivePayload k{};
    std::memcpy(k.alloc_id, relay_alloc_id_, 8);
    uint8_t buf[rly::MAX_CONTROL_PACKET];
    const size_t n = rly::encode_keepalive(buf, sizeof(buf), k);
    if (n == 0) return;
    socket_->send_to(buf, n, relay_addr_);
    last_relay_keepalive_ = Clock::now();
}

bool HostSession::relay_bind_blocking() {
    if (!socket_ || !relay_session_set_ || relay_addr_.ip == 0) return false;
    namespace rly = net::relay;
    rly::BindPayload b{};
    std::memcpy(b.session_id, relay_session_id_, 32);
    if (relay_license_set_) {
        b.has_license = true;
        std::memcpy(b.license, relay_license_, 95);
    }
    uint8_t txbuf[rly::MAX_CONTROL_PACKET];
    const size_t txlen = rly::encode_bind(txbuf, sizeof(txbuf), b);
    if (txlen == 0) return false;

    log::info("HostSession", "Relay BIND at %u.%u.%u.%u:%u%s",
              (relay_addr_.ip >>  0) & 0xff, (relay_addr_.ip >>  8) & 0xff,
              (relay_addr_.ip >> 16) & 0xff, (relay_addr_.ip >> 24) & 0xff,
              relay_addr_.port,
              relay_license_set_ ? " [with license]" : "");

    const auto start = std::chrono::steady_clock::now();
    auto next_send = start;
    uint8_t rxbuf[rly::MAX_DATA_PACKET];
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now - start > std::chrono::seconds(3)) break;
        if (now >= next_send) {
            socket_->send_to(txbuf, txlen, relay_addr_);
            next_send = now + std::chrono::milliseconds(200);
        }
        net::SocketAddr sender;
        const int n = socket_->recv_from(rxbuf, sizeof(rxbuf), sender);
        if (n > 0 && n >= 4 && rxbuf[0] == 'D' && rxbuf[1] == 'B'
            && rxbuf[2] == 'R' && rxbuf[3] == 'L') {
            rly::MsgType t; size_t poff = 0, plen = 0;
            if (!rly::parse_header(rxbuf, static_cast<size_t>(n), t, poff, plen)) continue;
            if (t != rly::MsgType::BindAck) continue;
            rly::BindAckPayload ack{};
            if (!rly::decode_bind_ack(rxbuf + poff, plen, ack)) continue;
            std::memcpy(relay_alloc_id_, ack.alloc_id, 8);
            relay_active_         = true;
            last_relay_keepalive_ = std::chrono::steady_clock::now();
            if (sender_) sender_->set_relay_active(relay_addr_, relay_alloc_id_);
            log::info("HostSession",
                "Relay bound: alloc=%02x%02x%02x%02x%02x%02x%02x%02x paired=%d ttl=%us",
                ack.alloc_id[0], ack.alloc_id[1], ack.alloc_id[2], ack.alloc_id[3],
                ack.alloc_id[4], ack.alloc_id[5], ack.alloc_id[6], ack.alloc_id[7],
                ack.paired, ack.ttl_seconds);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    log::error("HostSession", "Relay BIND timed out");
    return false;
}

void HostSession::send_rendezvous_register() {
    if (!socket_ || rendezvous_addr_.ip == 0) return;
    namespace rdv = net::rdv;
    rdv::RegisterPayload reg{};
    std::memcpy(reg.pubkey, host_identity_.public_key, 32);

    // Advertise LAN candidates so peers behind the same NAT can connect
    // directly without the (usually broken) hairpin path through the
    // public router.  The candidate port is whatever the host bound the
    // main video socket to; the rendezvous gets it from socket_->local_port.
    const uint16_t local_port = socket_->local_port();
    const auto lans = net::enumerate_local_ipv4(rdv::MAX_LAN_CANDIDATES);
    reg.lan_count = static_cast<uint8_t>(std::min(lans.size(), rdv::MAX_LAN_CANDIDATES));
    for (uint8_t i = 0; i < reg.lan_count; ++i) {
        reg.lan[i].ip   = lans[i];
        reg.lan[i].port = local_port;
    }

    uint8_t buf[rdv::MAX_PACKET];
    const size_t n = rdv::encode_register(buf, sizeof(buf), reg);
    if (n == 0) return;
    socket_->send_to(buf, n, rendezvous_addr_);
}

void HostSession::handle_rendezvous_packet(const uint8_t* data, size_t len,
                                           const net::SocketAddr& sender) {
    namespace rdv = net::rdv;
    rdv::MsgType type;
    size_t poff = 0, plen = 0;
    if (!rdv::parse_header(data, len, type, poff, plen)) return;

    switch (type) {
    case rdv::MsgType::RegisterAck: {
        rdv::RegisterAckPayload p;
        if (!rdv::decode_register_ack(data + poff, plen, p)) break;
        log::info("HostSession",
                  "Rendezvous OK: reflexive %u.%u.%u.%u:%u (TTL %us)",
                  (p.reflexive_ip >>  0) & 0xff, (p.reflexive_ip >>  8) & 0xff,
                  (p.reflexive_ip >> 16) & 0xff, (p.reflexive_ip >> 24) & 0xff,
                  p.reflexive_port, p.ttl_seconds);
        // If the user enabled relay mode (--relay) without manually
        // pinning a session_id, fill the session_id from this RegisterAck
        // and BIND now — this is the path that makes --relay-session
        // HEX64 unnecessary in normal Pro flows.  Self-host setups
        // without --relay are unaffected: we never auto-route media
        // through an unrelated relay just because rdv mentioned one.
        if (p.relay_ip != 0
            && relay_addr_.ip != 0 && !relay_session_set_
            && !relay_active_) {
            std::memcpy(relay_session_id_, p.session_id, 32);
            relay_session_set_ = true;
            log::info("HostSession",
                "Using rendezvous-minted relay session_id (no manual --relay-session needed)");
            relay_bind_blocking();
        }
        break;
    }
    case rdv::MsgType::PunchHint: {
        rdv::PunchHintPayload p;
        if (!rdv::decode_punch_hint(data + poff, plen, p)) break;
        net::SocketAddr client_ep{ p.client_ip, p.client_port };
        log::info("HostSession",
                  "PunchHint: client at %u.%u.%u.%u:%u — opening pinhole",
                  (p.client_ip >>  0) & 0xff, (p.client_ip >>  8) & 0xff,
                  (p.client_ip >> 16) & 0xff, (p.client_ip >> 24) & 0xff,
                  p.client_port);
        punch_to(client_ep);
        break;
    }
    default:
        // Server shouldn't be sending us Register/Lookup; ignore quietly.
        (void)sender;
        break;
    }
}

void HostSession::punch_to(const net::SocketAddr& client) {
    if (!socket_) return;
    // Send 3 small one-byte UDP packets to the client.  The client side will
    // ignore them (length < PacketHeader::WIRE_SIZE → handle_packet bails
    // immediately) but the NAT in front of us now has an outbound binding
    // toward client_addr, so its return HELLOs will land on this socket.
    // Three packets gives us margin against a single drop.
    const uint8_t pad = 0x00;
    for (int i = 0; i < 3; ++i) socket_->send_to(&pad, 1, client);
}

} // namespace vivora::host
