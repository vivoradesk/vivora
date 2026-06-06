#include "client/net/client_session.h"
#include "common/crypto/host_identity.h"
#include "common/crypto/packet_crypto.h"
#include "common/crypto/peer_pin.h"
#include "common/net/relay_protocol.h"
#include "common/net/rendezvous_protocol.h"
#include "common/net/stun_client.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/packet.h"
#include "common/utils/log.h"
#include <cstring>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#  include <winsock2.h>   // gethostname (Winsock already initialised by sockets)
#else
#  include <unistd.h>     // gethostname
#endif

namespace vivora::client {

static const uint8_t HELLO_MAGIC[] = { 'D','E','S','K','B','E','A','M', 0x01 };
static const uint8_t HELLO_ACK[]   = { 'D','E','S','K','B','E','A','M', 0x01, 0x00 };

// Best-effort local device name for the approval prompt (VIV-61).  Capped to
// 63 bytes so it fits the 1-byte length prefix in the HELLO payload.  Empty
// on failure — the host falls back to a generic label.
static std::string local_device_name() {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') {
        buf[sizeof(buf) - 1] = '\0';
        std::string n(buf);
        if (n.size() > 63) n.resize(63);
        return n;
    }
    return std::string();
}

void ClientSession::set_host_key(const uint8_t host_pk[32]) {
    std::memcpy(host_static_pk_, host_pk, 32);
    host_key_set_ = true;
}

bool ClientSession::ensure_client_identity() {
    if (client_identity_loaded_) return true;
    // Reuse this device's long-term identity (the same keypair we'd present
    // as a host).  IK presents it to the host so the viewer is recognisable.
    if (!crypto::load_or_create_host_identity(client_identity_, "")) {
        log::error("ClientSession", "Failed to load client identity for handshake");
        return false;
    }
    client_identity_loaded_ = true;
    return true;
}

void ClientSession::set_peer_pubkey(const uint8_t pubkey[32]) {
    std::memcpy(peer_pubkey_, pubkey, 32);
    peer_pubkey_set_ = true;
}

void ClientSession::set_relay(const net::SocketAddr& addr, const uint8_t session_id[32]) {
    relay_addr_ = addr;
    std::memcpy(relay_session_id_, session_id, 32);
    relay_session_set_ = true;
}

void ClientSession::set_relay_license(const uint8_t token[95]) {
    std::memcpy(relay_license_, token, 95);
    relay_license_set_ = true;
}

bool ClientSession::start(const char* host_ip, uint16_t port) {
    // The pubkey may be either pinned up front (--host-key HEX) or learned
    // mid-start() from a rendezvous lookup-by-code. The hard check moves
    // to after the lookup attempt — until then either a pinned pubkey or
    // a peer_code_ to resolve is sufficient.
    if (!host_key_set_ && peer_code_.empty()) {
        log::error("ClientSession",
            "Host pubkey not set and no --peer code — refusing to start");
        return false;
    }

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

    // STUN before the rendezvous lookup so we have our own reflexive in
    // hand for same-NAT detection (compare against host's reflexive that
    // the lookup returns).  Same socket as the future video session, so
    // the binding STUN observed is the binding any peer will reach.
    if (stun_server_.ip != 0 && stun_server_.port != 0) {
        reflexive_addr_ = net::StunClient::discover(stun_server_, *socket_);
        if (reflexive_addr_.ip != 0) {
            log::info("ClientSession",
                "Reflexive address: %u.%u.%u.%u:%u (share this with the peer)",
                (reflexive_addr_.ip >> 0) & 0xFF, (reflexive_addr_.ip >> 8) & 0xFF,
                (reflexive_addr_.ip >> 16) & 0xFF, (reflexive_addr_.ip >> 24) & 0xFF,
                reflexive_addr_.port);
        } else {
            log::warn("ClientSession", "STUN discovery failed — reflexive address unknown");
        }
    }

    // Rendezvous lookup overrides the manually-passed --view IP:PORT when
    // both --rendezvous and --peer are set.  If the lookup fails and we
    // have no fallback host_ip, we bail.
    if (rendezvous_addr_.ip != 0 && (peer_pubkey_set_ || !peer_code_.empty())) {
        net::SocketAddr discovered{};
        uint8_t resolved_pk[32]{};
        if (lookup_via_rendezvous(discovered, resolved_pk) && discovered.ip != 0) {
            host_addr_ = discovered;
            // Lookup-by-code path: the rendezvous tells us the pubkey.  If
            // the user had also explicitly pinned --host-key, verify the
            // returned pubkey matches — otherwise the server is MITM-ing
            // and we refuse to proceed.
            if (host_key_set_) {
                if (std::memcmp(resolved_pk, host_static_pk_, 32) != 0) {
                    log::error("ClientSession",
                        "Rendezvous returned a different pubkey than --host-key — refusing");
                    return false;
                }
            } else {
                std::memcpy(host_static_pk_, resolved_pk, 32);
                host_key_set_ = true;
            }
            // Trust-on-first-use pin against persisted file.  Decoupled
            // from the explicit --host-key check above so even hex-pinned
            // connects benefit from the historical record.
            if (!peer_code_.empty()) {
                using crypto::PinResult;
                const PinResult pr = crypto::check_or_pin_peer(peer_code_, resolved_pk);
                if (pr == PinResult::NewlyPinned) {
                    log::info("ClientSession",
                        "Pinned new peer '%s' (first connect)", peer_code_.c_str());
                } else if (pr == PinResult::Mismatch) {
                    const std::string path = crypto::default_peer_pins_path();
                    log::error("ClientSession",
                        "Peer '%s' pubkey CHANGED — refusing to connect.\n"
                        "  If this is intentional (host re-installed), delete the\n"
                        "  matching line in: %s",
                        peer_code_.c_str(), path.c_str());
                    return false;
                } else if (pr == PinResult::IoError) {
                    log::warn("ClientSession",
                        "Could not read/write peer pin file (continuing without pin)");
                }
            }
            log::info("ClientSession",
                "Rendezvous lookup → host at %u.%u.%u.%u:%u (+ %u LAN candidate(s))",
                (host_addr_.ip >> 0) & 0xff, (host_addr_.ip >> 8) & 0xff,
                (host_addr_.ip >> 16) & 0xff, (host_addr_.ip >> 24) & 0xff,
                host_addr_.port,
                (unsigned)lookup_lan_count_);
            for (uint8_t i = 0; i < lookup_lan_count_; ++i) {
                log::info("ClientSession",
                    "  LAN candidate %u: %u.%u.%u.%u:%u", (unsigned)i,
                    (lookup_lan_[i].ip >>  0) & 0xff, (lookup_lan_[i].ip >>  8) & 0xff,
                    (lookup_lan_[i].ip >> 16) & 0xff, (lookup_lan_[i].ip >> 24) & 0xff,
                    lookup_lan_[i].port);
            }
        } else {
            log::warn("ClientSession",
                "Rendezvous lookup failed — falling back to --view target");
        }
    }

    if (host_addr_.ip == 0) {
        log::error("ClientSession", "Invalid host IP: %s", host_ip);
        return false;
    }
    if (!host_key_set_) {
        log::error("ClientSession",
            "Could not resolve host pubkey (rendezvous lookup failed)");
        return false;
    }

    receiver_ = std::make_unique<VideoReceiver>(*socket_);

    // Audio socket: ephemeral port. Used to receive PacketType::Audio.
    audio_socket_ = net::IUdpSocket::create();
    if (audio_socket_ && audio_socket_->bind(0)) {
        audio_socket_->set_nonblocking(true);
        audio_socket_->set_recvbuf(256 * 1024);
        audio_local_port_ = audio_socket_->local_port();
        log::info("ClientSession", "Audio socket bound to port %u", audio_local_port_);
    } else {
        log::warn("ClientSession", "Failed to bind audio socket — audio disabled");
        audio_socket_.reset();
        audio_local_port_ = 0;
    }

    // Relay is configured (--relay) but we don't BIND yet.  The auto-
    // fallback in poll() switches to relay only when the direct path
    // doesn't ACK within RELAY_FALLBACK_MS.  This way:
    //   - Direct connections never spend a relay slot (saves bandwidth
    //     and a license-token check on the managed relay).
    //   - Bad NAT pairs (CGNAT, restrictive corporate FW) fail over
    //     after ~5 s with one extra round trip for the BIND.
    // If relay isn't configured at all, the client just times out as
    // before — same shape as today's behaviour with no --relay flag.

    // Same-NAT short-circuit.  If our reflexive matches the host's, we're
    // both behind the same router and the public-IP punch path requires
    // hairpin NAT — usually broken on consumer routers.  Try the LAN
    // candidate that's on the same /24 as one of OUR local addresses
    // (i.e. the candidate that's actually routable from us); fall back
    // to the first if none match.  Without the subnet match Windows
    // hosts often advertise Hyper-V / WSL adapters first (172.23.x,
    // 192.168.14.x, etc.) and we'd punch into a black hole.
    if (lookup_lan_count_ > 0
        && reflexive_addr_.ip != 0
        && reflexive_addr_.ip == host_addr_.ip) {
        net::SocketAddr lan = lookup_lan_[0];
        const auto my_local = net::enumerate_local_ipv4(8);
        for (size_t i = 0; i < lookup_lan_count_; ++i) {
            const uint32_t cand_ip = lookup_lan_[i].ip;
            for (uint32_t my_ip : my_local) {
                // Compare /24 (low 24 bits in host order = top 3 octets;
                // ip stored little-endian byte=octet so first 3 octets
                // are bits 0..23).
                if ((cand_ip & 0x00FFFFFFu) == (my_ip & 0x00FFFFFFu)) {
                    lan = lookup_lan_[i];
                    goto picked;
                }
            }
        }
    picked:
        log::info("ClientSession",
            "Same-NAT detected (both at %u.%u.%u.%u) — trying LAN candidate %u.%u.%u.%u:%u",
            (reflexive_addr_.ip >>  0) & 0xff, (reflexive_addr_.ip >>  8) & 0xff,
            (reflexive_addr_.ip >> 16) & 0xff, (reflexive_addr_.ip >> 24) & 0xff,
            (lan.ip >>  0) & 0xff, (lan.ip >>  8) & 0xff,
            (lan.ip >> 16) & 0xff, (lan.ip >> 24) & 0xff, lan.port);
        host_addr_ = lan;
    }

    // Prime a fresh Noise_NK handshake.  send_hello() will write msg1 into
    // the wire; handle_control() processes msg2.  init_initiator() re-runs
    // InitializeSymmetric internally, so calling it is equivalent to a reset.
    if (!ensure_client_identity()) return false;
    if (!handshake_.init_initiator(host_static_pk_, client_identity_)) {
        log::error("ClientSession", "Noise init_initiator failed");
        return false;
    }
    handshake_complete_ = false;

    state_ = SessionState::Connecting;
    connect_start_ = Clock::now();
    last_hello_time_ = {};
    last_recv_time_ = Clock::now();

    log::info("ClientSession", "Connecting to %u.%u.%u.%u:%u",
              (host_addr_.ip >> 0) & 0xff, (host_addr_.ip >> 8) & 0xff,
              (host_addr_.ip >> 16) & 0xff, (host_addr_.ip >> 24) & 0xff,
              host_addr_.port);
    send_hello();
    return true;
}

void ClientSession::stop() {
    stop_audio();
    if (socket_) {
        socket_->close();
        socket_.reset();
    }
    if (audio_socket_) {
        audio_socket_->close();
        audio_socket_.reset();
    }
    receiver_.reset();
    state_ = SessionState::Disconnected;
}

bool ClientSession::start_audio() {
    if (!audio_socket_) return false;
    if (audio_receiver_) return true;

    auto output = audio::create_default_audio_output();
    if (!output) {
        log::warn("ClientSession", "No default audio output available");
        return false;
    }
    audio_receiver_ = std::make_unique<AudioReceiver>();
    // 60ms = 6 frames of prebuffer. Wide enough to absorb typical WiFi
    // bursts of 3–5 dropped packets without dropping to PLC, while still
    // cheap in end-to-end audio latency terms.
    if (!audio_receiver_->start(std::move(output), /*jitter_target_ms=*/120)) {
        audio_receiver_.reset();
        return false;
    }
    log::info("ClientSession", "Audio playback started");
    return true;
}

void ClientSession::stop_audio() {
    if (audio_receiver_) {
        audio_receiver_->stop();
        audio_receiver_.reset();
    }
}

void ClientSession::poll() {
    if (!socket_) return;

    // Refresh the relay binding well before TTL (server uses 60s); this
    // also keeps any NAT pinhole alive on long-quiet sessions.
    if (relay_active_) {
        const auto now = Clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_relay_keepalive_).count() >= 20) {
            relay_send_keepalive();
        }
    }

    uint8_t buf[RECV_BUF_SIZE];
    net::SocketAddr sender;

    for (;;) {
        int n = socket_->recv_from(buf, sizeof(buf), sender);
        if (n <= 0) break;
        last_recv_time_ = Clock::now();
        bytes_received_ += static_cast<uint64_t>(n);
        handle_packet(buf, static_cast<size_t>(n));
    }

    // Periodic audio firewall keepalive. Sent every ~1s FROM the audio socket
    // TO the host's audio port (port+1) so Windows Defender's stateful UDP
    // filter keeps the return path open and isn't dependent on one punch
    // packet surviving. Also kicks in before state becomes Connected so the
    // host's first audio burst after handshake isn't dropped.
    if (audio_socket_) {
        auto since_punch = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - last_audio_punch_).count();
        if (last_audio_punch_.time_since_epoch().count() == 0 || since_punch >= 1000) {
            net::SocketAddr audio_host = host_addr_;
            audio_host.port = host_addr_.port + 1;
            uint8_t punch[1] = {0};
            audio_socket_->send_to(punch, 1, audio_host);
            if (last_audio_punch_.time_since_epoch().count() == 0) {
                log::info("ClientSession", "Audio firewall punch started to port %u (1s keepalive)",
                          audio_host.port);
            }
            last_audio_punch_ = Clock::now();
        }
    }

    // Drain audio socket: each packet is a PacketType::Audio wrapper holding
    // an Opus frame, AEAD-sealed with audio_recv_cs_.  Feed the decrypted
    // Opus payload directly into the jitter buffer.
    if (audio_socket_ && audio_receiver_) {
        net::SocketAddr a_sender;
        uint8_t opened[RECV_BUF_SIZE];
        for (;;) {
            int n = audio_socket_->recv_from(buf, sizeof(buf), a_sender);
            if (n <= 0) break;
            // Raw-socket counter: increments BEFORE parsing so we can tell
            // "nothing reaches the socket" (firewall/NAT drop) from "socket
            // receives but parser rejects" (bad header, wrong type, etc.).
            audio_raw_packets_++;
            audio_raw_bytes_ += static_cast<uint64_t>(n);
            // Sealed audio wire must carry at least header + AEAD overhead.
            // Anything shorter is either a stray packet or a truncated record.
            if (!handshake_complete_) continue;
            if (static_cast<size_t>(n) < protocol::PacketHeader::WIRE_SIZE +
                                         crypto::CipherState::OVERHEAD) continue;
            size_t opened_len = crypto::open_packet(buf, static_cast<size_t>(n),
                                                    audio_recv_cs_, opened);
            if (opened_len == 0) continue;  // auth fail / replay / tamper
            auto h = protocol::PacketHeader::deserialize(opened);
            if (h.type != protocol::PacketType::Audio) continue;
            size_t plen = opened_len - protocol::PacketHeader::WIRE_SIZE;
            if (plen == 0 || plen != h.payload_len) continue;
            audio_receiver_->feed(h.seq_no,
                                  opened + protocol::PacketHeader::WIRE_SIZE,
                                  plen);
        }
        // Periodic diagnostic log — separates "socket silent" from parser issues.
        auto since_log = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - last_audio_stat_log_).count();
        if (last_audio_stat_log_.time_since_epoch().count() == 0 || since_log >= 5000) {
            uint64_t recv = audio_receiver_ ? audio_receiver_->packets_received() : 0;
            uint64_t plc  = audio_receiver_ ? audio_receiver_->plc_frames()      : 0;
            // Per-second rates for the HUD — averaged across this 5 s
            // log window.  Audio runs at ~100 pps when healthy.
            const double window_s = since_log > 0 ? since_log / 1000.0 : 5.0;
            const uint64_t d_recv = recv - last_audio_recv_;
            const uint64_t d_plc  = plc  - last_audio_plc_;
            last_audio_pps_ = static_cast<uint32_t>(d_recv / window_s);
            last_plc_pct_   = d_recv > 0
                ? static_cast<uint32_t>((d_plc * 100) / d_recv)
                : 0;
            last_audio_recv_ = recv;
            last_audio_plc_  = plc;
            log::info("ClientSession",
                      "Audio socket stats: raw_pkts=%llu raw_bytes=%llu parsed=%llu",
                      (unsigned long long)audio_raw_packets_,
                      (unsigned long long)audio_raw_bytes_,
                      (unsigned long long)recv);
            last_audio_stat_log_ = Clock::now();
        }
    }

    auto now = Clock::now();

    if (state_ == SessionState::Connecting) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - connect_start_).count();
        // Relay auto-fallback: if direct hasn't ACKed in RELAY_FALLBACK_MS
        // and a relay was configured (--relay), BIND to the relay now and
        // keep retrying HELLO through it.  Reset the connect_start_ so
        // we get a fresh CONNECT_TIMEOUT_MS budget for the relay attempt.
        if (elapsed > RELAY_FALLBACK_MS
            && relay_session_set_ && !relay_active_
            && relay_addr_.ip != 0) {
            log::warn("ClientSession",
                "No HELLO_ACK in %lldms — falling back to relay",
                static_cast<long long>(elapsed));
            if (relay_bind_blocking()) {
                // Reset Noise state and HELLO timers so the next send_hello
                // starts a clean handshake on the relay path.
                ensure_client_identity();
                handshake_.init_initiator(host_static_pk_, client_identity_);
                handshake_complete_ = false;
                connect_start_   = Clock::now();
                last_hello_time_ = {};
                send_hello();
            } else {
                log::error("ClientSession", "Relay BIND failed during fallback");
            }
            return;
        }
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

        if (receiver_) {
            // FEC recovery FIRST: recover lost packets before NACK fires.
            // The recv loop above already drained all buffered packets, so
            // any FEC group still missing exactly 1 packet = true loss.
            fec_recovered_scratch_.clear();
            receiver_->fec_tick(fec_recovered_scratch_);
            for (const auto& rec : fec_recovered_scratch_) {
                if (rec.size() >= protocol::PacketHeader::WIRE_SIZE) {
                    auto pkt = protocol::Packet::deserialize(rec.data(), rec.size());
                    receiver_->feed(pkt);
                }
            }

            // NACK processing: retransmit only what FEC couldn't recover.
            // We tried gap=2 / rl=RTT for tail-loss recovery — it caused
            // a hang/crash within a couple of seconds (suspected NACK
            // storm).  Reverted to the conservative 4 ms / 1.5×RTT
            // values that have been stable across the project.
            int64_t gap_ms = 4;
            int64_t rl_ms = rtt_ms_ > 0 ? static_cast<int64_t>(rtt_ms_ * 1.5) : 8;
            if (rl_ms < 8) rl_ms = 8;
            auto batches = receiver_->collect_nacks(gap_ms, rl_ms);
            for (const auto& b : batches) {
                send_nack(b.seq_no, b.frag_indices.data(), b.frag_indices.size());
            }
        }

        // Periodic FEC loss report — host uses this to adapt FEC group size K.
        {
            auto since_report = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_fec_report_time_).count();
            if (since_report >= FEC_REPORT_INTERVAL_MS) {
                send_fec_report();
                last_fec_report_time_ = now;
            }
        }

        // Periodic perf report — drives adaptive framerate on the host
        // side.  Sent every 1 s with the client's current sustainable-
        // fps estimate based on this interval's reject + drop counters.
        // Initialise the timer at the first poll so we don't fire a
        // bogus "huge interval" report before the very first second.
        if (last_perf_report_time_.time_since_epoch().count() == 0) {
            last_perf_report_time_ = now;
            perf_drops_baseline_   = receiver_ ? receiver_->frames_dropped() : 0;
        } else {
            auto since_perf = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_perf_report_time_).count();
            if (since_perf >= PERF_REPORT_INTERVAL_MS) {
                send_perf_report();
                last_perf_report_time_ = now;
            }
        }

        // BW probe flush: host sends N packets in a burst. If the tail is
        // lost we'd never hit the last-index trigger in handle_bw_probe and
        // the probe would time out. After 500ms of silence with partial
        // receipts, ACK with what we got.
        if (probe_received_ > 0 && !probe_ack_sent_) {
            auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - probe_last_time_).count();
            if (since_last > 500) {
                send_bw_probe_ack();
                probe_ack_sent_ = true;
            }
        }
    }
}

bool ClientSession::pop_frame(net::AssembledFrame& frame) {
    if (!receiver_) return false;
    return receiver_->pop_frame(frame);
}

void ClientSession::handle_packet(const uint8_t* data, size_t len) {
    // Drop rendezvous packets that arrive after start() has finished — they
    // happen if the server retransmits a LookupResponse for the same lookup,
    // or if a stray PunchHint somehow lands here.  Either way, ignored at
    // steady state.  The pre-handshake lookup loop pulls them inside
    // lookup_via_rendezvous() directly.  Single-byte UDP "punch" probes
    // from the host's pinhole-opener also land here and fall through the
    // size guard below.
    if (len >= 4 && data[0] == 'D' && data[1] == 'B' && data[2] == 'R' && data[3] == 'V') {
        return;
    }
    // Relay-forwarded payload arrives wrapped in DBRL DATA — strip the
    // header and re-feed the inner payload through this same handler.
    // BIND_ACK is consumed inline in relay_bind_blocking() during start(),
    // not here; if we see one at steady state it's a stray, drop it.
    if (len >= 4 && data[0] == 'D' && data[1] == 'B' && data[2] == 'R' && data[3] == 'L') {
        namespace rly = net::relay;
        rly::MsgType t; size_t poff = 0, plen = 0;
        if (!rly::parse_header(data, len, t, poff, plen)) return;
        if (t != rly::MsgType::Data) return;
        uint8_t aid[8]; const uint8_t* inner = nullptr; size_t inner_len = 0;
        if (!rly::decode_data(data + poff, plen, aid, &inner, &inner_len)) return;
        // Verify alloc id matches our binding.  Mismatch = stray traffic
        // for someone else, drop.
        if (std::memcmp(aid, relay_alloc_id_, 8) != 0) return;
        handle_packet(inner, inner_len);
        return;
    }
    if (len < protocol::PacketHeader::WIRE_SIZE) return;

    auto header = protocol::PacketHeader::deserialize(data);

    // Control is Noise handshake traffic — always plaintext on the wire.
    if (header.type == protocol::PacketType::Control) {
        const uint8_t* payload = data + protocol::PacketHeader::WIRE_SIZE;
        const size_t   payload_len = len - protocol::PacketHeader::WIRE_SIZE;
        handle_control(payload, payload_len);
        return;
    }

    // Every other type is expected to be sealed.  Pre-handshake traffic gets
    // dropped: the host only starts sending Video/Ping/etc after we complete
    // finalize(), so anything arriving earlier is stale or spoofed.
    if (!handshake_complete_) return;

    uint8_t opened[RECV_BUF_SIZE];
    size_t  opened_len = crypto::open_packet(data, len, recv_cs_, opened);
    if (opened_len == 0) return;  // AEAD rejected — drop silently.

    auto h = protocol::PacketHeader::deserialize(opened);
    const uint8_t* payload     = opened + protocol::PacketHeader::WIRE_SIZE;
    const size_t   payload_len = opened_len - protocol::PacketHeader::WIRE_SIZE;

    switch (h.type) {
        case protocol::PacketType::Ping:
            handle_ping(payload, payload_len);
            break;
        case protocol::PacketType::BwProbe:
            handle_bw_probe(payload, payload_len);
            break;
        case protocol::PacketType::CursorShape:
            handle_cursor_shape(payload, payload_len);
            break;
        case protocol::PacketType::CursorPosition:
            handle_cursor_position(payload, payload_len);
            break;
        case protocol::PacketType::StreamInfo:
            handle_stream_info(payload, payload_len);
            break;
        case protocol::PacketType::Video: {
            // Feed plaintext wire bytes through FEC decoder → assembler.
            // FEC decoder consumes FLAG_FEC parity packets internally and
            // emits recovered data wires for the assembler.
            if (receiver_) {
                fec_recovered_scratch_.clear();
                receiver_->fec_feed(opened, opened_len, fec_recovered_scratch_);
                for (const auto& rec : fec_recovered_scratch_) {
                    if (rec.size() >= protocol::PacketHeader::WIRE_SIZE) {
                        auto pkt = protocol::Packet::deserialize(rec.data(), rec.size());
                        receiver_->feed(pkt);
                    }
                }
                if (!(h.flags & protocol::FLAG_FEC)) {
                    auto packet = protocol::Packet::deserialize(opened, opened_len);
                    receiver_->feed(packet);
                }
            }
            break;
        }
        default:
            break;
    }
}

bool ClientSession::send_sealed(const std::vector<uint8_t>& wire) {
    uint8_t sealed[RECV_BUF_SIZE];
    size_t  sealed_len = crypto::seal_packet(wire.data(), wire.size(),
                                             send_cs_, sealed);
    if (sealed_len == 0) {
        log::warn("ClientSession", "seal_packet failed (nonce exhausted?)");
        return false;
    }
    return transport_send(sealed, sealed_len);
}

void ClientSession::handle_control(const uint8_t* payload, size_t len) {
    // Already connected?  This is either a duplicate msg2 or a stale one from
    // a prior handshake — ignore.  Rekey is not implemented yet.
    if (handshake_complete_) return;

    // Expected shape: Noise msg2 = ephemeral(32) + encrypted(HELLO_ACK+codec) + tag(16).
    uint8_t inner[32] = {};
    int inner_len = handshake_.read_message(payload, len, inner, sizeof(inner));
    if (inner_len < 0) {
        log::warn("ClientSession", "Noise msg2 rejected — wrong host key or tampered");
        return;
    }
    if (inner_len < static_cast<int>(sizeof(HELLO_ACK))) return;
    if (std::memcmp(inner, HELLO_ACK, sizeof(HELLO_ACK)) != 0) return;

    if (inner_len >= static_cast<int>(sizeof(HELLO_ACK)) + 1) {
        uint8_t codec_byte = inner[sizeof(HELLO_ACK)];
        host_codec_ = (codec_byte == static_cast<uint8_t>(VideoCodec::H264))
                          ? VideoCodec::H264
                          : VideoCodec::HEVC;
    }

    // Transfer handshake keys into transport cipher states — main channel
    // (video/control) and audio in the same finalize() call.  After this the
    // handshake object is effectively spent.
    if (!handshake_.finalize(send_cs_,       recv_cs_,
                             audio_send_cs_, audio_recv_cs_)) {
        log::error("ClientSession", "Noise finalize failed — transport not keyed");
        return;
    }
    handshake_complete_ = true;

    if (state_ == SessionState::Connecting) {
        state_ = SessionState::Connected;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - connect_start_).count();
        log::info("ClientSession",
                  "Connected (handshake took %lldms, host codec=%s)",
                  elapsed, host_codec_ == VideoCodec::HEVC ? "HEVC" : "H.264");
    }
}

void ClientSession::handle_cursor_shape(const uint8_t* payload, size_t len) {
    // Fragmented payload:
    //   [shape_id:4B LE][frag_idx:1B][frag_count:1B][chunk bytes...]
    // Host fragments because cursor bitmaps exceed MTU and fragmented
    // UDP is frequently dropped by WiFi gear.
    constexpr size_t FRAG_HEADER = 6;
    if (len < FRAG_HEADER) return;
    uint32_t shape_id = 0;
    std::memcpy(&shape_id, payload, 4);
    uint8_t frag_idx   = payload[4];
    uint8_t frag_count = payload[5];
    if (frag_count == 0 || frag_idx >= frag_count) return;

    // Skip fragments for shapes we already delivered — host retries for
    // loss protection and we don't want to repeatedly re-reassemble.
    if (shape_id == last_delivered_shape_id_ && !pending_shape_valid_) return;

    auto& r = shape_reassembly_[shape_id];
    if (r.fragments.size() != frag_count) {
        r.fragments.assign(frag_count, std::vector<uint8_t>{});
        r.received_count = 0;
    }
    if (!r.fragments[frag_idx].empty()) return;  // duplicate fragment
    r.fragments[frag_idx].assign(payload + FRAG_HEADER, payload + len);
    r.received_count++;

    if (r.received_count < frag_count) return;

    // All chunks arrived — concatenate and deserialize.
    std::vector<uint8_t> body;
    size_t total = 0;
    for (auto& f : r.fragments) total += f.size();
    body.reserve(total);
    for (auto& f : r.fragments) body.insert(body.end(), f.begin(), f.end());

    protocol::CursorShapeMessage msg;
    if (protocol::CursorShapeMessage::deserialize(body.data(), body.size(), msg)) {
        pending_shape_ = std::move(msg);
        pending_shape_valid_ = true;
    }
    // Drop the buffer whether or not deserialization succeeded — we
    // can't do anything more with these fragments.
    shape_reassembly_.erase(shape_id);
}

void ClientSession::handle_cursor_position(const uint8_t* payload, size_t len) {
    protocol::CursorPositionMessage msg;
    if (!protocol::CursorPositionMessage::deserialize(payload, len, msg)) return;
    cursor_pos_ = msg;
    cursor_pos_received_ = true;
}

void ClientSession::handle_stream_info(const uint8_t* payload, size_t len) {
    protocol::StreamInfoMessage msg;
    if (!protocol::StreamInfoMessage::deserialize(payload, len, msg)) {
        log::warn("ClientSession", "StreamInfo deserialize failed (len=%zu)", len);
        return;
    }
    if (msg.width == 0 || msg.height == 0) return;
    if (msg.width  != stream_info_.width ||
        msg.height != stream_info_.height) {
        stream_info_ = msg;
        new_stream_info_ = true;
        log::info("ClientSession", "Got StreamInfo %ux%u", msg.width, msg.height);
    }
}

bool ClientSession::take_new_stream_info(protocol::StreamInfoMessage& out) {
    if (!new_stream_info_) return false;
    out = stream_info_;
    new_stream_info_ = false;
    return true;
}

bool ClientSession::take_new_cursor_shape(protocol::CursorShapeMessage& out) {
    if (!pending_shape_valid_) return false;
    out = std::move(pending_shape_);
    pending_shape_ = protocol::CursorShapeMessage{};
    pending_shape_valid_ = false;
    last_delivered_shape_id_ = out.shape_id;
    return true;
}

void ClientSession::handle_ping(const uint8_t* payload, size_t len) {
    // Ping payload carries host-measured RTT (microseconds) in bytes [4..8).
    if (len >= 8) {
        uint32_t rtt_us = static_cast<uint32_t>(payload[4])
                        | (static_cast<uint32_t>(payload[5]) << 8)
                        | (static_cast<uint32_t>(payload[6]) << 16)
                        | (static_cast<uint32_t>(payload[7]) << 24);
        rtt_ms_ = rtt_us / 1000.0;
    }
    // Reply with Pong, echoing the ping payload
    protocol::Packet pong;
    pong.header.type = protocol::PacketType::Pong;
    pong.header.seq_no = 0;
    pong.header.timestamp = 0;
    pong.header.flags = 0;
    pong.payload.assign(payload, payload + len);
    pong.header.payload_len = static_cast<uint16_t>(len);

    auto wire = pong.serialize();
    send_sealed(wire);
}

void ClientSession::send_hello() {
    // Every retry builds a fresh handshake: we don't know which of our prior
    // msg1s reached the host, and each carries its own ephemeral DH share.
    // The host mirrors this — a new msg1 always resets its side.
    if (!ensure_client_identity()) return;
    if (!handshake_.init_initiator(host_static_pk_, client_identity_)) {
        log::error("ClientSession", "Noise init_initiator failed on retry");
        return;
    }
    handshake_complete_ = false;

    // Inner payload: HELLO_MAGIC | audio port (2) | name len (1) | device name.
    // The name (VIV-61) lets the host label the approval prompt.
    uint8_t inner[96];
    std::memcpy(inner, HELLO_MAGIC, sizeof(HELLO_MAGIC));
    inner[sizeof(HELLO_MAGIC)]     = static_cast<uint8_t>(audio_local_port_ & 0xFF);
    inner[sizeof(HELLO_MAGIC) + 1] = static_cast<uint8_t>((audio_local_port_ >> 8) & 0xFF);
    size_t inner_len = sizeof(HELLO_MAGIC) + 2;
    const std::string dn = local_device_name();
    const uint8_t nlen = static_cast<uint8_t>(dn.size());   // already <= 63
    inner[inner_len++] = nlen;
    if (nlen) { std::memcpy(inner + inner_len, dn.data(), nlen); inner_len += nlen; }

    uint8_t msg1[256];
    size_t msg1_len = handshake_.write_message(inner, inner_len,
                                               msg1, sizeof(msg1));
    if (msg1_len == 0) return;

    protocol::Packet hello;
    hello.header.type = protocol::PacketType::Control;
    hello.header.seq_no = 0;
    hello.header.timestamp = 0;
    hello.header.flags = 0;
    hello.payload.assign(msg1, msg1 + msg1_len);
    hello.header.payload_len = static_cast<uint16_t>(hello.payload.size());

    auto wire = hello.serialize();
    transport_send(wire.data(), wire.size());
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
    send_sealed(wire);
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
    send_sealed(wire);
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
    send_sealed(wire);
}

void ClientSession::reset_video_stream() {
    if (receiver_) receiver_->reset_stream();
}

void ClientSession::handle_bw_probe(const uint8_t* payload, size_t len) {
    if (len < 6) return;
    uint16_t id    = payload[0] | (static_cast<uint16_t>(payload[1]) << 8);
    uint16_t index = payload[2] | (static_cast<uint16_t>(payload[3]) << 8);
    uint16_t count = payload[4] | (static_cast<uint16_t>(payload[5]) << 8);

    if (id != probe_id_) {
        probe_id_ = id;
        probe_received_ = 0;
        probe_count_ = count;
        probe_ack_sent_ = false;
        probe_first_time_ = Clock::now();
    }

    probe_received_++;
    probe_last_time_ = Clock::now();

    if (!probe_ack_sent_ && (index == count - 1 || probe_received_ == count)) {
        send_bw_probe_ack();
        probe_ack_sent_ = true;
    }
}

void ClientSession::send_bw_probe_ack() {
    if (!socket_ || probe_received_ < 2) return;

    auto span = std::chrono::duration<double>(probe_last_time_ - probe_first_time_);
    double span_sec = span.count();
    if (span_sec < 0.0001) span_sec = 0.0001;

    double bits = static_cast<double>(probe_received_) * 1200.0 * 8.0;
    uint32_t bw_bps = static_cast<uint32_t>(bits / span_sec);

    log::info("ClientSession", "BW probe: %u/%u received in %.1fms -> %u kbps",
              probe_received_, probe_count_, span_sec * 1000.0, bw_bps / 1000);

    protocol::Packet pkt;
    pkt.header.type = protocol::PacketType::BwProbeAck;
    pkt.header.seq_no = 0;
    pkt.header.timestamp = 0;
    pkt.header.flags = 0;
    // Payload: probe_id(2B) + received(2B LE) + bw_bps(4B LE)
    pkt.payload.resize(8);
    pkt.payload[0] = static_cast<uint8_t>(probe_id_ & 0xFF);
    pkt.payload[1] = static_cast<uint8_t>((probe_id_ >> 8) & 0xFF);
    pkt.payload[2] = static_cast<uint8_t>(probe_received_ & 0xFF);
    pkt.payload[3] = static_cast<uint8_t>((probe_received_ >> 8) & 0xFF);
    pkt.payload[4] = static_cast<uint8_t>(bw_bps & 0xFF);
    pkt.payload[5] = static_cast<uint8_t>((bw_bps >> 8) & 0xFF);
    pkt.payload[6] = static_cast<uint8_t>((bw_bps >> 16) & 0xFF);
    pkt.payload[7] = static_cast<uint8_t>((bw_bps >> 24) & 0xFF);
    pkt.header.payload_len = 8;

    auto wire = pkt.serialize();
    send_sealed(wire);
}

void ClientSession::note_decoder_accepted() { ++perf_accepted_; }
void ClientSession::note_decoder_rejected() {
    ++perf_rejected_;
    ++total_rejected_;
}

void ClientSession::send_perf_report() {
    if (state_ != SessionState::Connected || !socket_) return;

    // Compute reject + drop ratios for this 1 s window.
    const uint32_t total        = perf_accepted_ + perf_rejected_;
    const uint64_t drops_now    = receiver_ ? receiver_->frames_dropped() : 0;
    const uint64_t drops_window = drops_now - perf_drops_baseline_;
    perf_drops_baseline_ = drops_now;
    total_dropped_ = drops_now;  // assembler counter is already cumulative
    // Denominator includes drops so "0 decoded + many drops" reads as
    // 100% loss — the earlier `total > 0 ? ... : 0` guard masked startup
    // overload as a clean interval and let the up-step logic bump fps
    // back to 60 too aggressively.
    const uint32_t denom = total + static_cast<uint32_t>(drops_window);
    const float reject_ratio = denom > 0
        ? static_cast<float>(perf_rejected_) / static_cast<float>(denom)
        : 0.0f;
    const float drop_ratio = denom > 0
        ? static_cast<float>(drops_window) / static_cast<float>(denom)
        : 0.0f;

    // If the interval had almost no activity (e.g. waiting for an IDR),
    // there's no signal to act on — hold target_fps_ steady and keep
    // the clean-streak counter at zero so we don't drift down on
    // silence or up on pure absence-of-evidence.
    constexpr uint32_t MIN_ACTIVITY = 10;
    const bool low_activity = denom < MIN_ACTIVITY;

    // Adaptation: ratchet down quickly when we're overloaded, ratchet up
    // slowly after sustained clean intervals so we don't oscillate.
    if (low_activity) {
        perf_clean_streak_ = 0;
    } else if (reject_ratio > PERF_REJECT_DOWN || drop_ratio > PERF_REJECT_DOWN) {
        // Step ~25% down, snap to a coarse ladder to avoid jitter — 60,
        // 45, 30, 22, 15.  Floor at PERF_TARGET_FPS_MIN.
        uint16_t next = static_cast<uint16_t>(perf_target_fps_ * 3 / 4);
        if (next < PERF_TARGET_FPS_MIN) next = PERF_TARGET_FPS_MIN;
        perf_target_fps_     = next;
        perf_clean_streak_   = 0;
    } else if (reject_ratio < PERF_REJECT_UP && drop_ratio < PERF_REJECT_UP) {
        if (++perf_clean_streak_ >= PERF_UP_STREAK) {
            uint16_t next = static_cast<uint16_t>(perf_target_fps_ * 5 / 4);
            if (next > PERF_TARGET_FPS_MAX) next = PERF_TARGET_FPS_MAX;
            // Hold at 60 unless caller has explicitly opted into 120 — for
            // now everything tops out at 60 as a safety; the 120 ceiling
            // becomes meaningful when the host gains a `--target-fps 120`
            // CLI flag and signals the cap to the client.
            if (next > 60) next = 60;
            perf_target_fps_   = next;
            perf_clean_streak_ = 0;
        }
    } else {
        perf_clean_streak_ = 0;  // marginal interval — neither up nor down
    }

    // Build wire payload (8 bytes).
    protocol::Packet pkt;
    pkt.header.type      = protocol::PacketType::PerfReport;
    pkt.header.seq_no    = 0;
    pkt.header.timestamp = 0;
    pkt.header.flags     = 0;
    pkt.payload.resize(8, 0);
    pkt.payload[0] = static_cast<uint8_t>(perf_target_fps_ & 0xFF);
    pkt.payload[1] = static_cast<uint8_t>((perf_target_fps_ >> 8) & 0xFF);
    pkt.payload[2] = static_cast<uint8_t>(reject_ratio * 100.0f + 0.5f);
    pkt.payload[3] = static_cast<uint8_t>(drop_ratio   * 100.0f + 0.5f);
    // [4..8) reserved for future fields (decode_us / render_us avg).
    pkt.header.payload_len = 8;

    // Cache for HUD before resetting the counters below.
    last_reject_pct_ = reject_ratio * 100.0f;
    last_drop_pct_   = drop_ratio   * 100.0f;
    // Bitrate over this 1 s window — bytes received on the main socket
    // (video + control + retx).  Audio socket bytes are reported
    // separately via the audio-stat block.
    const uint64_t bytes_window = bytes_received_ - bytes_baseline_;
    bytes_baseline_   = bytes_received_;
    last_bitrate_bps_ = static_cast<uint32_t>(bytes_window * 8u);

    auto wire = pkt.serialize();
    send_sealed(wire);

    log::info("ClientSession",
              "PerfReport: target=%u accepted=%u rejected=%u drops=%llu "
              "reject=%.1f%% drop=%.1f%% streak=%d",
              perf_target_fps_, perf_accepted_, perf_rejected_,
              (unsigned long long)drops_window,
              reject_ratio * 100.0f, drop_ratio * 100.0f,
              perf_clean_streak_);

    // Reset window counters for the next interval.
    perf_accepted_ = 0;
    perf_rejected_ = 0;
}

void ClientSession::send_fec_report() {
    if (state_ != SessionState::Connected || !socket_ || !receiver_) return;

    float loss = receiver_->loss_rate();
    // Delta of FEC group failures since the last report — direct evidence
    // that M was undersized.  Host uses this as a fast event-driven signal
    // alongside the slower loss-rate EWMA: any delta > 0 immediately bumps
    // M, sustained zeros taper it back down.
    uint64_t failed_now    = receiver_->fec_failed();
    uint32_t delta_failed  = static_cast<uint32_t>(failed_now - last_fec_failed_reported_);
    last_fec_failed_reported_ = failed_now;

    protocol::Packet pkt;
    pkt.header.type = protocol::PacketType::FecReport;
    pkt.header.seq_no = 0;
    pkt.header.timestamp = 0;
    pkt.header.flags = 0;
    pkt.payload.resize(8, 0);
    std::memcpy(pkt.payload.data(),     &loss,         4);  // float32 LE
    std::memcpy(pkt.payload.data() + 4, &delta_failed, 4);  // u32 LE
    pkt.header.payload_len = 8;

    auto wire = pkt.serialize();
    send_sealed(wire);
}

uint64_t ClientSession::frames_dropped() const {
    return receiver_ ? receiver_->frames_dropped() : 0;
}

bool ClientSession::transport_send(const uint8_t* data, size_t len) {
    if (!socket_) return false;
    if (relay_active_) {
        namespace rly = net::relay;
        uint8_t buf[rly::MAX_DATA_PACKET];
        const size_t n = rly::encode_data(buf, sizeof(buf), relay_alloc_id_, data, len);
        if (n == 0) return false;
        return socket_->send_to(buf, n, relay_addr_) >= 0;
    }
    return socket_->send_to(data, len, host_addr_) >= 0;
}

void ClientSession::relay_send_keepalive() {
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

bool ClientSession::relay_bind_blocking() {
    if (!socket_ || !relay_session_set_ || relay_addr_.ip == 0) return false;
    namespace rly = net::relay;
    rly::BindPayload b{};
    std::memcpy(b.session_id, relay_session_id_, 32);
    if (relay_license_set_) {
        b.has_license = true;
        std::memcpy(b.license, relay_license_, 95);
    }
    uint8_t txbuf[rly::MAX_CONTROL_PACKET];   // 256 fits 8+32+95=135
    const size_t txlen = rly::encode_bind(txbuf, sizeof(txbuf), b);
    if (txlen == 0) return false;

    log::info("ClientSession", "Relay BIND at %u.%u.%u.%u:%u%s",
              (relay_addr_.ip >>  0) & 0xff, (relay_addr_.ip >>  8) & 0xff,
              (relay_addr_.ip >> 16) & 0xff, (relay_addr_.ip >> 24) & 0xff,
              relay_addr_.port,
              relay_license_set_ ? " [with license]" : "");

    // Same retry shape as lookup_via_rendezvous: 200ms tries, 3s total.
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
            log::info("ClientSession",
                "Relay bound: alloc=%02x%02x%02x%02x%02x%02x%02x%02x paired=%d ttl=%us",
                ack.alloc_id[0], ack.alloc_id[1], ack.alloc_id[2], ack.alloc_id[3],
                ack.alloc_id[4], ack.alloc_id[5], ack.alloc_id[6], ack.alloc_id[7],
                ack.paired, ack.ttl_seconds);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    log::error("ClientSession", "Relay BIND timed out");
    return false;
}

bool ClientSession::lookup_via_rendezvous(net::SocketAddr& out, uint8_t out_pk[32]) {
    if (!socket_ || rendezvous_addr_.ip == 0) return false;
    if (!peer_pubkey_set_ && peer_code_.empty())  return false;
    namespace rdv = net::rdv;

    uint8_t txbuf[rdv::MAX_PACKET];
    size_t  txlen = 0;
    if (peer_pubkey_set_) {
        rdv::LookupPayload q{};
        std::memcpy(q.pubkey, peer_pubkey_, 32);
        txlen = rdv::encode_lookup(txbuf, sizeof(txbuf), q);
        log::info("ClientSession",
            "Rendezvous lookup (by pubkey) at %u.%u.%u.%u:%u",
            (rendezvous_addr_.ip >> 0) & 0xff, (rendezvous_addr_.ip >> 8) & 0xff,
            (rendezvous_addr_.ip >> 16) & 0xff, (rendezvous_addr_.ip >> 24) & 0xff,
            rendezvous_addr_.port);
    } else {
        rdv::LookupByCodePayload q{};
        std::memset(q.code, 0, sizeof(q.code));
        std::strncpy(q.code, peer_code_.c_str(), sizeof(q.code) - 1);
        txlen = rdv::encode_lookup_code(txbuf, sizeof(txbuf), q);
        log::info("ClientSession",
            "Rendezvous lookup (by code '%s') at %u.%u.%u.%u:%u",
            peer_code_.c_str(),
            (rendezvous_addr_.ip >> 0) & 0xff, (rendezvous_addr_.ip >> 8) & 0xff,
            (rendezvous_addr_.ip >> 16) & 0xff, (rendezvous_addr_.ip >> 24) & 0xff,
            rendezvous_addr_.port);
    }
    if (txlen == 0) return false;

    // Retry every 200 ms for up to 3 s.  Drains the main socket's recv
    // queue between attempts so we catch the answer the moment it lands.
    const auto start = std::chrono::steady_clock::now();
    auto next_send = start;
    uint8_t rxbuf[rdv::MAX_PACKET];
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now - start > std::chrono::seconds(3)) break;
        if (now >= next_send) {
            socket_->send_to(txbuf, txlen, rendezvous_addr_);
            next_send = now + std::chrono::milliseconds(200);
        }
        net::SocketAddr sender;
        int n = socket_->recv_from(rxbuf, sizeof(rxbuf), sender);
        if (n > 0) {
            // Only accept DBRV packets here; everything else is unexpected
            // pre-handshake noise and is silently dropped.
            rdv::MsgType type;
            size_t poff = 0, plen = 0;
            if (rdv::parse_header(rxbuf, static_cast<size_t>(n), type, poff, plen)
                && type == rdv::MsgType::LookupResponse) {
                rdv::LookupResponsePayload p{};
                if (rdv::decode_lookup_resp(rxbuf + poff, plen, p)) {
                    if (p.found) {
                        out.ip   = p.host_ip;
                        out.port = p.host_port;
                        std::memcpy(out_pk, p.pubkey, 32);
                        // Pull out the LAN candidate list so start() can
                        // do same-NAT detection later.
                        lookup_lan_count_ = p.lan_count;
                        if (lookup_lan_count_ > rdv::MAX_LAN_CANDIDATES) {
                            lookup_lan_count_ = rdv::MAX_LAN_CANDIDATES;
                        }
                        for (uint8_t i = 0; i < lookup_lan_count_; ++i) {
                            lookup_lan_[i].ip   = p.lan[i].ip;
                            lookup_lan_[i].port = p.lan[i].port;
                        }
                        // If the rendezvous advertised a relay AND the
                        // user enabled --relay without pinning a session_id,
                        // adopt the rendezvous-minted session_id now.  The
                        // BIND happens later in start() once we've returned.
                        if (p.relay_ip != 0
                            && relay_addr_.ip != 0 && !relay_session_set_) {
                            std::memcpy(relay_session_id_, p.session_id, 32);
                            relay_session_set_ = true;
                            log::info("ClientSession",
                                "Using rendezvous-minted relay session_id");
                        }
                        return true;
                    } else {
                        log::warn("ClientSession",
                            "Peer not registered at rendezvous (yet?)");
                        // Keep retrying — host might come online in-window.
                    }
                }
            }
            // Other DBRV types (RegisterAck etc) are not for us, ignore.
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace vivora::client
