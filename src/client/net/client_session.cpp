#include "client/net/client_session.h"
#include "common/crypto/host_identity.h"
#include "common/crypto/packet_crypto.h"
#include "common/crypto/peer_pin.h"
#include "common/crypto/random.h"
#include "common/net/relay_protocol.h"
#include "common/net/rendezvous_protocol.h"
#include "common/net/stun_client.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/packet.h"
#include "common/utils/log.h"
#include <algorithm>
#include <cstdlib>
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
    // Remember the target so a reconnect can re-run establish() without the
    // caller re-supplying it (VIV-54).
    host_ip_ = host_ip ? host_ip : "";
    port_    = port;
    if (!establish(/*is_reconnect=*/false)) return false;
    state_ = SessionState::Connecting;
    return true;
}

const char* ClientSession::disconnect_status_text() const {
    // User-visible terminal messages (no ticket refs).  Unknown/forward-compat
    // reasons fall through to a neutral wording — still a terminal, no-reconnect
    // state (VIV-52).
    if (!host_disconnected_) return "";
    switch (disconnect_reason_) {
        case protocol::DisconnectReason::Rejected:
            return "Connection declined";
        case protocol::DisconnectReason::Kicked:
            return "Removed from your account";
        case protocol::DisconnectReason::HostShutdown:
            return "The host ended the session";
        default:
            return "Disconnected by host";
    }
}

double ClientSession::reconnect_seconds() const {
    if (reconnect_start_.time_since_epoch().count() == 0) return 0.0;
    return std::chrono::duration<double>(Clock::now() - reconnect_start_).count();
}

void ClientSession::begin_reconnect() {
    // Connected → Reconnecting (VIV-54).  The socket, audio socket and async
    // receive thread all stay alive: cheap HELLO probes ride the existing
    // socket and a returning host keeps hitting the same audio port.  The
    // stream ciphers are stale until the next handshake, so drop
    // handshake_complete_ and let a fresh msg2 re-key us.
    state_                = SessionState::Reconnecting;
    handshake_complete_   = false;
    reconnect_start_      = Clock::now();
    reconnect_attempt_    = 1;   // banner counts from 1 (probing phase)
    reconnect_backoff_ms_ = RECONNECT_BACKOFF_MIN_MS;
    reconnect_saw_packet_ = false;
    // First full re-establish (which re-runs the rendezvous lookup) after the
    // initial backoff; until then poll() sends cheap HELLO probes to host_addr_.
    next_reestablish_at_  = Clock::now() + std::chrono::milliseconds(reconnect_backoff_ms_);
    last_hello_time_      = {};
    log::warn("ClientSession", "Host went silent — reconnecting (window stays open)");
}

bool ClientSession::establish(bool is_reconnect) {
    // Stale-state guard: a re-dial on a fresh attempt must not report the
    // previous attempt's trust question (VIV-23).
    trust_pending_active_ = false;
    trust_pending_ = TrustPending{};
    // The pubkey may be either pinned up front (--host-key HEX) or learned
    // mid-start() from a rendezvous lookup-by-code. The hard check moves
    // to after the lookup attempt — until then either a pinned pubkey or
    // a peer_code_ to resolve is sufficient.
    if (!host_key_set_ && peer_code_.empty()) {
        log::error("ClientSession",
            "Host pubkey not set and no --peer code — refusing to start");
        return false;
    }

    // A reconnect reuses the existing UDP socket (keeping the same source port
    // keeps any host-side NAT pinhole warm) but must first stop the async
    // receive thread so the synchronous rendezvous lookup / relay BIND below
    // can read the socket exclusively (VIV-54).
    if (recv_running_.exchange(false)) {
        if (recv_thread_.joinable()) recv_thread_.join();
    }
    recv_ring_.reset();

    if (!is_reconnect) {
        socket_ = net::IUdpSocket::create();
        if (!socket_) return false;

        // Bind to any port
        if (!socket_->bind(0)) {
            log::error("ClientSession", "Failed to bind");
            return false;
        }

        socket_->set_nonblocking(true);
        socket_->set_recvbuf(8 * 1024 * 1024);  // 8MB — absorb decode-spike backlog (VIV-82)
    }
    if (!socket_) return false;

    // Re-evaluate the relay path from scratch each attempt; a reconnect that
    // had been on the relay re-BINDs once the socket is ready (below).
    const bool was_relay_active = relay_active_;
    relay_active_ = false;

    host_addr_.ip = net::parse_ip(host_ip_.c_str());
    host_addr_.port = port_;

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
            if (!peer_code_.empty() && interactive_trust_) {
                // VIV-23 GUI path: never auto-pin, never hard-refuse — hand
                // the decision to the user.  On Unknown/Mismatch we record
                // what we learned and abort; the GUI pins on consent (via
                // crypto::pin_peer) and re-dials, and the retry Matches.
                std::string stored_hex;
                switch (crypto::query_peer_pin(peer_code_, resolved_pk, &stored_hex)) {
                    case crypto::PinQuery::Match:
                        break;
                    case crypto::PinQuery::Unknown:
                        trust_pending_ = TrustPending{
                            /*mismatch=*/false, peer_code_,
                            crypto::hex_encode(resolved_pk, 32), std::string() };
                        trust_pending_active_ = true;
                        log::info("ClientSession",
                            "Peer '%s' not pinned yet — awaiting user trust decision",
                            peer_code_.c_str());
                        return false;
                    case crypto::PinQuery::Mismatch:
                        trust_pending_ = TrustPending{
                            /*mismatch=*/true, peer_code_,
                            crypto::hex_encode(resolved_pk, 32), stored_hex };
                        trust_pending_active_ = true;
                        log::warn("ClientSession",
                            "Peer '%s' pubkey CHANGED (possible MITM) — "
                            "awaiting user trust decision", peer_code_.c_str());
                        return false;
                }
            } else if (!peer_code_.empty()) {
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
        log::error("ClientSession", "Invalid host IP: %s", host_ip_.c_str());
        return false;
    }
    if (!host_key_set_) {
        log::error("ClientSession",
            "Could not resolve host pubkey (rendezvous lookup failed)");
        return false;
    }

    receiver_ = std::make_unique<VideoReceiver>(*socket_);

    // Audio socket: ephemeral port. Used to receive PacketType::Audio.  On a
    // reconnect we deliberately keep the already-bound audio socket (and its
    // port) so the host can keep delivering audio to the same endpoint across
    // the dropout (VIV-54) — only bind it on the initial connect.
    if (!is_reconnect || !audio_socket_) {
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
    // A reconnect that had been running over the relay re-BINDs to refresh the
    // (likely expired) allocation before the first HELLO (VIV-54).  Direct
    // reconnects skip this and try P2P first, exactly like the initial connect.
    if (is_reconnect && was_relay_active
        && relay_session_set_ && relay_addr_.ip != 0) {
        if (relay_bind_blocking()) {
            log::info("ClientSession", "Reconnect: relay re-bound");
        } else {
            log::warn("ClientSession", "Reconnect: relay re-BIND failed — trying direct");
        }
    }

    if (!ensure_client_identity()) return false;
    if (!handshake_.init_initiator(host_static_pk_, client_identity_)) {
        log::error("ClientSession", "Noise init_initiator failed");
        return false;
    }
    handshake_complete_ = false;

    // Caller sets the visible state (Connecting for a fresh start(),
    // Reconnecting is kept across an attempt) — establish() only primes timers.
    connect_start_ = Clock::now();
    last_hello_time_ = {};
    last_recv_time_ = Clock::now();

    log::info("ClientSession", "Connecting to %u.%u.%u.%u:%u",
              (host_addr_.ip >> 0) & 0xff, (host_addr_.ip >> 8) & 0xff,
              (host_addr_.ip >> 16) & 0xff, (host_addr_.ip >> 24) & 0xff,
              host_addr_.port);
    send_hello();

    // VIV-81: optionally offload socket receive to a dedicated thread so the
    // kernel UDP buffer is drained continuously and never overflows under
    // burst (the recvbuf-loss → IDR-churn root cause).  ON BY DEFAULT now
    // (VIV-82) — without it a slow poll()/decode lets the kernel recvbuf
    // overflow (RcvbufErrors in the 100k's, mass packet loss → freezes).
    // VIVORA_PIPELINE=legacy forces the old in-poll recv for debugging.
    if (const char* p = std::getenv("VIVORA_PIPELINE"))
        async_recv_ = (std::strcmp(p, "legacy") != 0 && std::strcmp(p, "inpoll") != 0);
    else
        async_recv_ = true;
    if (async_recv_) {
        recv_ring_ = std::make_unique<util::SpscRing<RawPacket, 8192>>();
        recv_running_.store(true, std::memory_order_release);
        recv_thread_ = std::thread(&ClientSession::recv_thread_proc, this);
        log::info("ClientSession",
                  "async socket-receive thread started (VIVORA_PIPELINE=threaded)");
    }
    return true;
}

void ClientSession::recv_thread_proc() {
    // Drain the video socket as fast as the wire delivers, parking raw
    // packets in recv_ring_ for poll() to decrypt/dispatch.  Non-blocking
    // recv + a short nap when idle: under load it never sleeps, so the kernel
    // buffer stays near-empty; when quiet it yields the core.
    RawPacket pkt;
    while (recv_running_.load(std::memory_order_acquire)) {
        int n = socket_->recv_from(pkt.data, sizeof(pkt.data), pkt.from);
        if (n > 0) {
            pkt.len = n;
            // Ring full only if poll() stalled badly (shouldn't happen with a
            // 1024-deep ring + 60Hz poll); drop — host FEC/IDR covers it.
            recv_ring_->try_push(pkt);
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}

void ClientSession::stop() {
    stop_audio();
    // Stop the receive thread before closing the socket it reads (VIV-81).
    if (recv_running_.exchange(false)) {
        if (recv_thread_.joinable()) recv_thread_.join();
    }
    recv_ring_.reset();
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
    // 40ms = 2 frames of prebuffer. CLAUDE.md budgets 20–40ms for the audio
    // jitter buffer; we sit at the top of that band so a typical WiFi burst of
    // 1–2 dropped packets rides through, and lean on Opus in-band FEC + the PLC
    // path for anything larger rather than paying fixed prebuffer latency
    // (VIV-94: was 120ms, ~3× the spec and out of sync with video).
    if (!audio_receiver_->start(std::move(output), /*jitter_target_ms=*/40)) {
        audio_receiver_.reset();
        return false;
    }
    // Apply any volume/mute the user set before audio came up (VIV-74).
    audio_receiver_->set_volume(audio_volume_);
    audio_receiver_->set_muted(audio_muted_);
    log::info("ClientSession", "Audio playback started");
    return true;
}

void ClientSession::set_audio_volume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    audio_volume_ = v;
    if (audio_receiver_) audio_receiver_->set_volume(v);
}

void ClientSession::set_audio_muted(bool m) {
    audio_muted_ = m;
    if (audio_receiver_) audio_receiver_->set_muted(m);
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

    if (async_recv_ && recv_ring_) {
        // Threaded path: process whatever the receive thread has parked.
        RawPacket pkt;
        while (recv_ring_->try_pop(pkt)) {
            if (pkt.len <= 0) continue;
            last_recv_time_ = Clock::now();
            if (state_ == SessionState::Reconnecting) reconnect_saw_packet_ = true;
            bytes_received_ += static_cast<uint64_t>(pkt.len);
            handle_packet(pkt.data, static_cast<size_t>(pkt.len));
        }
    } else {
        // Legacy path: recv directly here.
        for (;;) {
            int n = socket_->recv_from(buf, sizeof(buf), sender);
            if (n <= 0) break;
            last_recv_time_ = Clock::now();
            if (state_ == SessionState::Reconnecting) reconnect_saw_packet_ = true;
            bytes_received_ += static_cast<uint64_t>(n);
            handle_packet(buf, static_cast<size_t>(n));
        }
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

    // Reconnecting (VIV-54): retry the handshake against the same peer config
    // with exponential backoff until the host returns, the user cancels, or the
    // overall deadline expires.  Kept between Connected and Disconnected so the
    // window (and audio socket) stay alive across the dropout.
    if (state_ == SessionState::Reconnecting) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - reconnect_start_).count();
        if (max_reconnect_ms_ > 0 && elapsed > max_reconnect_ms_) {
            log::warn("ClientSession",
                "Reconnect gave up after %llds — closing",
                static_cast<long long>(elapsed / 1000));
            state_ = SessionState::Disconnected;
            return;
        }

        // Full re-establish on the backoff boundary: re-runs the (blocking)
        // rendezvous lookup / same-NAT / relay path so a host that came back at
        // a *different* address is found again.  Cheap HELLO probes to the
        // last-known address run in between, so the common case (host returns at
        // the same endpoint) reconnects within ~1 RTT without a re-lookup.
        if (now >= next_reestablish_at_) {
            log::info("ClientSession",
                "Reconnect attempt %d re-establish (%llds elapsed)",
                reconnect_attempt_, static_cast<long long>(elapsed / 1000));
            if (!establish(/*is_reconnect=*/true) && trust_pending_active_) {
                // Host pubkey changed during the dropout (possible MITM): do
                // NOT silently reconnect.  Drop to Disconnected; the view layer
                // surfaces the existing VIV-23 trust dialog via trust_pending().
                log::warn("ClientSession",
                    "Host key changed on reconnect — stopping for trust decision");
                state_ = SessionState::Disconnected;
                return;
            }
            // Schedule the next full re-establish, grow the backoff, and count
            // the next window as a new attempt.  A transient miss (host still
            // down) just waits out the next gap.
            next_reestablish_at_ = Clock::now()
                + std::chrono::milliseconds(reconnect_backoff_ms_);
            reconnect_backoff_ms_ = std::min<int64_t>(
                reconnect_backoff_ms_ * 2, RECONNECT_BACKOFF_MAX_MS);
            ++reconnect_attempt_;
            reconnect_saw_packet_ = false;
            last_hello_time_ = Clock::now();  // establish() already sent a HELLO
        } else {
            // Cheap probe: on the HELLO retry cadence, or immediately when the
            // host sends us anything (pre-emptive shortcut).  Re-inits the
            // handshake and re-sends msg1 to the last-known address — no
            // blocking lookup, so it's safe to run every tick.
            const bool preempt = reconnect_saw_packet_;
            const auto since_hello = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_hello_time_).count();
            if (preempt || last_hello_time_.time_since_epoch().count() == 0
                || since_hello > HELLO_RETRY_MS) {
                reconnect_saw_packet_ = false;
                send_hello();
            }
        }
        return;
    }

    if (state_ == SessionState::Connected) {
        auto since_recv = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_recv_time_).count();
        if (since_recv > DISCONNECT_TIMEOUT_MS) {
            // VIV-52: an explicit host Disconnect already put us in Disconnected
            // (handled below in the switch), so this branch normally won't run
            // for a reject/kick.  Guard anyway: never auto-reconnect after an
            // intentional teardown — only a silent link drop reconnects.
            if (max_reconnect_ms_ > 0 && !host_disconnected_) {
                // Keep the window open and start retrying (VIV-54).
                begin_reconnect();
            } else {
                log::warn("ClientSession", "Host timed out");
                state_ = SessionState::Disconnected;
            }
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
            // storm).  gap must span at least ~1.5 frame intervals: age is
            // measured from the frame's FIRST packet while pacing spreads
            // the frame across its whole interval, so a shorter gap NACKs
            // packets that are still in flight (spurious retx storms on
            // clean links, worse the higher the framerate).
            const uint16_t fps = effective_target_fps();
            int64_t gap_ms = fps > 0 ? (1500 / fps) : 25;
            if (gap_ms < 8) gap_ms = 8;
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

        // VIV-22: one delayed clipboard re-send for UDP-loss resilience.
        // The host dedups by clip_id, so the repeat is idempotent.
        if (clip_resend_pending_) {
            auto since_clip = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - clip_last_send_).count();
            if (since_clip >= CLIPBOARD_RESEND_MS) {
                for (const auto& wire : clip_tx_wires_) send_sealed(wire);
                clip_resend_pending_ = false;
                clip_tx_wires_.clear();
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
        case protocol::PacketType::MonitorList:
            handle_monitor_list(payload, payload_len);
            break;
        case protocol::PacketType::Clipboard: {
            // VIV-22: host -> client clipboard, fragmented like CursorShape.
            protocol::ClipboardMessage msg;
            if (clipboard_rx_.feed(payload, payload_len, msg)) {
                pending_clipboard_       = std::move(msg);
                pending_clipboard_valid_ = true;
                log::info("ClientSession", "Clipboard received from host (%zu bytes)",
                          pending_clipboard_.data.size());
            }
            break;
        }
        case protocol::PacketType::Disconnect: {
            // VIV-52: the host is intentionally ending this session (declined
            // approval or removed device).  Latch the reason, go straight to
            // Disconnected, and set host_disconnected_ so the auto-reconnect
            // path stays down — a silent link drop (no packet) still reconnects.
            protocol::DisconnectReason reason = protocol::DisconnectReason::HostShutdown;
            if (payload_len >= 1)
                reason = static_cast<protocol::DisconnectReason>(payload[0]);
            disconnect_reason_ = reason;
            host_disconnected_ = true;
            state_             = SessionState::Disconnected;
            log::info("ClientSession",
                      "Host sent Disconnect (reason=%u) — ending session, no reconnect",
                      static_cast<unsigned>(payload_len >= 1 ? payload[0] : 0));
            break;
        }
        case protocol::PacketType::HostStats:
            // Host's current encoder target bitrate (kbps, u32 LE) — for the
            // "encoding (actual)" HUD readout (VIV-82).
            if (payload_len >= 4) {
                encoding_kbps_ = static_cast<uint32_t>(payload[0])
                               | (static_cast<uint32_t>(payload[1]) << 8)
                               | (static_cast<uint32_t>(payload[2]) << 16)
                               | (static_cast<uint32_t>(payload[3]) << 24);
            }
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

    if (state_ == SessionState::Connecting || state_ == SessionState::Reconnecting) {
        const bool was_reconnect = (state_ == SessionState::Reconnecting);
        state_ = SessionState::Connected;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - connect_start_).count();
        log::info("ClientSession",
                  "%s (handshake took %lldms, host codec=%s)",
                  was_reconnect ? "Reconnected" : "Connected",
                  elapsed, host_codec_ == VideoCodec::HEVC ? "HEVC" : "H.264");
        // Reset reconnect bookkeeping so a future dropout starts a fresh backoff
        // ladder (VIV-54).  The view layer notices the Reconnecting → Connected
        // edge and requests an IDR to resume rendering.
        reconnect_attempt_    = 0;
        reconnect_backoff_ms_ = RECONNECT_BACKOFF_MIN_MS;
        reconnect_saw_packet_ = false;
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
    // Host's applied framerate target (VIV-67): 0 means an old host that
    // doesn't send the field — keep whatever we had.
    if (msg.target_fps != 0 && msg.target_fps != host_target_fps_) {
        host_target_fps_ = msg.target_fps;
        log::info("ClientSession", "Host stream target -> %u fps", host_target_fps_);
    }
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

void ClientSession::handle_monitor_list(const uint8_t* payload, size_t len) {
    protocol::MonitorListMessage msg;
    if (!protocol::MonitorListMessage::deserialize(payload, len, msg)) {
        log::warn("ClientSession", "MonitorList deserialize failed (len=%zu)", len);
        return;
    }
    monitor_list_     = std::move(msg.monitors);
    new_monitor_list_ = true;
    log::info("ClientSession", "Got MonitorList (%zu display(s))", monitor_list_.size());
}

bool ClientSession::take_new_monitor_list(std::vector<protocol::MonitorDesc>& out) {
    if (!new_monitor_list_) return false;
    out = monitor_list_;
    new_monitor_list_ = false;
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

    // Inner payload (all fields optional/length-tolerant so older/newer peers
    // interoperate — an absent tail field = "legacy default"):
    //   HELLO_MAGIC(9) | audio_port(2) | name_len(1) | name(name_len)
    //                  | caps_len(1)   | caps(caps_len)         <- VIV-112
    // The name (VIV-61) lets the host label the approval prompt; the codec
    // capability field (VIV-112) tells the host which codecs we can decode.
    uint8_t inner[96];
    std::memcpy(inner, HELLO_MAGIC, sizeof(HELLO_MAGIC));
    inner[sizeof(HELLO_MAGIC)]     = static_cast<uint8_t>(audio_local_port_ & 0xFF);
    inner[sizeof(HELLO_MAGIC) + 1] = static_cast<uint8_t>((audio_local_port_ >> 8) & 0xFF);
    size_t inner_len = sizeof(HELLO_MAGIC) + 2;
    const std::string dn = local_device_name();
    const uint8_t nlen = static_cast<uint8_t>(dn.size());   // already <= 63
    inner[inner_len++] = nlen;
    if (nlen) { std::memcpy(inner + inner_len, dn.data(), nlen); inner_len += nlen; }

    // VIV-112 codec-capability field: caps_len(1) | caps bytes.  A single
    // little-endian bitmask byte today (VideoCodecCaps); the length prefix lets
    // it grow (more codecs → more bits/bytes) without another wire revision.
    // A legacy host stops after the name and never reads these bytes; a legacy
    // client omits them entirely, which the host reads as "no caps → dictate".
    inner[inner_len++] = 1;                                   // caps_len
    inner[inner_len++] = decode_caps_;                        // caps[0]

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

VideoCodec ClientSession::request_codec_downgrade() {
    // The decoder couldn't initialise for host_codec_ at runtime.  Drop that
    // codec from our advertised caps and tell the host to renegotiate down; it
    // re-picks from the intersection (which now excludes the failed codec) and
    // switches its live encoder.  We also pin host_codec_ to H.264 locally so
    // the view retries decoder init with a codec every client can decode — even
    // if the CodecRenegotiate packet is lost, or the host is too old to honour
    // it, the client will at least try H.264.
    decode_caps_ &= ~codec_cap_bit(host_codec_);
    if (decode_caps_ == 0) decode_caps_ = CODEC_CAP_H264;  // never advertise nothing
    log::warn("ClientSession",
              "Decoder init failed for %s — requesting host downgrade (caps now 0x%02X)",
              host_codec_ == VideoCodec::HEVC ? "HEVC" : "H.264", decode_caps_);

    host_codec_ = VideoCodec::H264;

    if (state_ == SessionState::Connected && socket_) {
        protocol::Packet pkt;
        pkt.header.type = protocol::PacketType::CodecRenegotiate;
        pkt.header.seq_no = 0;
        pkt.header.timestamp = 0;
        pkt.header.flags = 0;
        pkt.payload.assign(1, decode_caps_);
        pkt.header.payload_len = 1;
        auto wire = pkt.serialize();
        send_sealed(wire);
    }
    return host_codec_;
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

void ClientSession::request_monitor_list() {
    if (state_ != SessionState::Connected || !socket_) return;

    protocol::Packet pkt;
    pkt.header.type = protocol::PacketType::MonitorListRequest;
    pkt.header.seq_no = 0;
    pkt.header.timestamp = 0;
    pkt.header.flags = 0;
    pkt.header.payload_len = 0;

    auto wire = pkt.serialize();
    send_sealed(wire);
}

void ClientSession::select_monitor(uint8_t index) {
    if (state_ != SessionState::Connected || !socket_) return;

    protocol::SelectMonitorMessage msg;
    msg.index = index;

    protocol::Packet pkt;
    pkt.header.type = protocol::PacketType::SelectMonitor;
    pkt.header.seq_no = 0;
    pkt.header.timestamp = 0;
    pkt.header.flags = 0;
    pkt.payload = msg.serialize();
    pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());

    auto wire = pkt.serialize();
    send_sealed(wire);
    log::info("ClientSession", "Requested host display switch -> %u", index);
}

void ClientSession::send_clipboard(const protocol::ClipboardMessage& msg) {
    if (state_ != SessionState::Connected || !socket_) return;

    const auto frags = protocol::fragment_clipboard(msg, ++clip_tx_id_);
    if (frags.empty()) {
        log::warn("ClientSession", "Clipboard message rejected by fragmenter (%zu bytes)",
                  msg.data.size());
        return;
    }
    clip_tx_wires_.clear();
    clip_tx_wires_.reserve(frags.size());
    for (const auto& f : frags) {
        protocol::Packet pkt;
        pkt.header.type        = protocol::PacketType::Clipboard;
        pkt.header.seq_no      = 0;
        pkt.header.timestamp   = 0;
        pkt.header.flags       = 0;
        pkt.payload            = f;
        pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());
        clip_tx_wires_.push_back(pkt.serialize());
    }
    for (const auto& wire : clip_tx_wires_) send_sealed(wire);
    clip_last_send_      = Clock::now();
    clip_resend_pending_ = true;
    log::info("ClientSession", "Clipboard sent to host (%zu bytes, %zu fragment(s))",
              msg.data.size(), frags.size());
}

bool ClientSession::take_new_clipboard(protocol::ClipboardMessage& out) {
    if (!pending_clipboard_valid_) return false;
    out = std::move(pending_clipboard_);
    pending_clipboard_       = protocol::ClipboardMessage{};
    pending_clipboard_valid_ = false;
    return true;
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
        // Learned ceiling: a punishment landing within two report windows
        // of an up-step means the climb itself likely overran what the
        // decoder/link sustains.  Two such punished climbs in a session
        // pin the ceiling at the level we last climbed FROM, so we stop
        // revisiting a rate that stutters (user-visible freezes) just to
        // re-learn the same lesson.
        if (windows_since_climb_ <= 2 && last_climb_from_ > 0) {
            if (++climb_strikes_ >= 2 &&
                (learned_fps_ceiling_ == 0 ||
                 last_climb_from_ < learned_fps_ceiling_)) {
                learned_fps_ceiling_ = last_climb_from_;
                log::info("ClientSession",
                          "Learned fps ceiling: %u (2 punished climbs — "
                          "not probing higher this session)",
                          learned_fps_ceiling_);
            }
        }
        perf_target_fps_     = next;
        perf_clean_streak_   = 0;
    } else if (reject_ratio < PERF_REJECT_UP && drop_ratio < PERF_REJECT_UP) {
        if (++perf_clean_streak_ >= PERF_UP_STREAK) {
            // Gentler probing above 60 fps: +12.5% instead of +25%, so a
            // probe that overruns the decoder overshoots by one small step
            // (72→81) instead of a leap (58→72) that visibly freezes.
            const uint16_t step = perf_target_fps_ >= 60
                ? std::max<uint16_t>(2, perf_target_fps_ / 8)
                : std::max<uint16_t>(2, perf_target_fps_ / 4);
            uint16_t next = static_cast<uint16_t>(perf_target_fps_ + step);
            // The report is a "can consume up to N" statement, not a demand:
            // the host clamps it to its own configured framerate cap (VIV-67,
            // HostSession::min_perf_target_fps).  Locally we additionally
            // honour the user's cap and the session's learned ceiling.
            uint16_t cap = PERF_TARGET_FPS_MAX;
            if (user_fps_cap_ != 0 && user_fps_cap_ < cap) cap = user_fps_cap_;
            if (learned_fps_ceiling_ != 0 && learned_fps_ceiling_ < cap)
                cap = learned_fps_ceiling_;
            if (next > cap) next = cap;
            if (next > perf_target_fps_) {
                last_climb_from_    = perf_target_fps_;
                windows_since_climb_ = 0;  // incremented to 1 below
                perf_target_fps_    = next;
            }
            perf_clean_streak_ = 0;
        }
    } else {
        perf_clean_streak_ = 0;  // marginal interval — neither up nor down
    }
    if (windows_since_climb_ < 255) ++windows_since_climb_;

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
    // [4..8): user-configured bitrate cap in kbps (LE u32, 0 = none).
    // Old hosts ignore these bytes (they were reserved zeros before).
    pkt.payload[4] = static_cast<uint8_t>(user_bitrate_cap_kbps_ & 0xFF);
    pkt.payload[5] = static_cast<uint8_t>((user_bitrate_cap_kbps_ >> 8) & 0xFF);
    pkt.payload[6] = static_cast<uint8_t>((user_bitrate_cap_kbps_ >> 16) & 0xFF);
    pkt.payload[7] = static_cast<uint8_t>((user_bitrate_cap_kbps_ >> 24) & 0xFF);
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

    // Per-lookup anti-spoofing nonce (VIV-92): the server echoes it verbatim
    // and we only accept a response that carries it back, so an attacker who
    // can't observe our outbound packet can't pre-forge a LookupResponse to
    // poison the TOFU pin.  CSPRNG — a predictable nonce would defeat the
    // whole point.
    uint8_t nonce[rdv::LOOKUP_NONCE_LEN];
    if (!crypto::random_bytes(nonce, sizeof(nonce))) {
        log::error("ClientSession", "CSPRNG failed for rendezvous nonce");
        return false;
    }

    uint8_t txbuf[rdv::MAX_PACKET];
    size_t  txlen = 0;
    if (peer_pubkey_set_) {
        rdv::LookupPayload q{};
        std::memcpy(q.pubkey, peer_pubkey_, 32);
        std::memcpy(q.nonce, nonce, sizeof(nonce));
        q.has_nonce = true;
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
        std::memcpy(q.nonce, nonce, sizeof(nonce));
        q.has_nonce = true;
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
        if (n > 0 && sender == rendezvous_addr_) {
            // Only accept DBRV packets that actually came from the rendezvous
            // server.  Without this, any host that can spray UDP at our
            // ephemeral source port could race a forged LookupResponse and get
            // its own pubkey silently TOFU-pinned as the trusted peer (MITM).
            // The sender check closes the off-path spray; the per-lookup nonce
            // below closes the on-path case (an attacker who can't observe our
            // outbound packet can't echo the right nonce) (VIV-92).
            rdv::MsgType type;
            size_t poff = 0, plen = 0;
            if (rdv::parse_header(rxbuf, static_cast<size_t>(n), type, poff, plen)
                && type == rdv::MsgType::LookupResponse) {
                rdv::LookupResponsePayload p{};
                if (rdv::decode_lookup_resp(rxbuf + poff, plen, p)) {
                    // Reject any response that doesn't echo our nonce — this
                    // includes a legacy (nonce-less) server, so the rendezvous
                    // must be redeployed with nonce support before clients that
                    // require it can connect.
                    if (!p.has_nonce
                        || std::memcmp(p.nonce, nonce, sizeof(nonce)) != 0) {
                        log::warn("ClientSession",
                            "Rendezvous response nonce mismatch — ignoring");
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        continue;
                    }
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
