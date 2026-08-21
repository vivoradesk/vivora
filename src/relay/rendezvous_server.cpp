// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// vivora-rendezvous: standalone UDP signalling server.
//
// Maintains a per-process in-memory map from host long-term public key to
// the host's reflexive (NAT-translated) UDP endpoint as observed at the
// rendezvous socket.  Clients lookup by pubkey to learn where to punch;
// the server simultaneously notifies the host that a punch is incoming
// from the client's reflexive endpoint.
//
// Storage is in-memory only — restart loses all registrations.  Hosts
// re-register on reconnect, so this is intentional (avoids state mgmt).
//
// Single thread, single UDP socket.  Blocking recv with a 1-second
// timeout drives the periodic stale-entry sweep — no separate timer
// thread needed.  At <1k messages/sec this is plenty (each message is
// O(1) hash lookup + 1 sendto).

#include "common/net/rendezvous_protocol.h"
#include "common/net/socket.h"
#include "common/utils/log.h"
#include "common/utils/peer_code.h"
#include "relay/metrics_emitter.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <signal.h>
#endif

namespace {

constexpr const char* TAG = "RDV";
constexpr uint16_t    DEFAULT_PORT     = 7000;
constexpr int         REGISTRATION_TTL = 60;   // seconds before a stale entry is dropped

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

struct Registration {
    vivora::net::SocketAddr endpoint;   // reflexive (the source addr the rdv saw)
    TimePoint                 expires_at; // wall-clock cutoff
    uint8_t                   lan_count = 0;
    vivora::net::rdv::LanCandidate lan[vivora::net::rdv::MAX_LAN_CANDIDATES]{};
    // Relay session id minted on first Register, kept stable across
    // keepalives so subsequent client lookups land on the same id.  Both
    // the host (via RegisterAck) and clients (via LookupResponse) get it.
    uint8_t                   session_id[32]{};
};

// Hash a pubkey by treating it as four 64-bit words — cheap, reasonable
// distribution for a 32-byte uniform random key.
struct PubkeyHash {
    size_t operator()(const std::array<uint8_t, 32>& k) const noexcept {
        size_t h = 0;
        for (int i = 0; i < 32; i += 8) {
            uint64_t word = 0;
            std::memcpy(&word, k.data() + i, 8);
            h ^= std::hash<uint64_t>{}(word) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        }
        return h;
    }
};

std::string ip_to_string(uint32_t ip_be) {
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &ip_be, buf, sizeof(buf));
    return std::string(buf);
}

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s [--port PORT] [--relay-endpoint HOST:PORT]\n"
        "  --port PORT              UDP port to listen on (default %u)\n"
        "  --relay-endpoint HP      Advertise relay HOST:PORT to peers in\n"
        "                           RegisterAck and LookupResponse so they can\n"
        "                           use Pro relay without manual --relay flags.\n"
        "                           Optional — omit for self-host setups where\n"
        "                           peers wire up the relay themselves.\n",
        argv0, DEFAULT_PORT);
}

#ifndef _WIN32
volatile sig_atomic_t g_running = 1;
void sigint_handler(int) { g_running = 0; }
#endif

} // namespace

int main(int argc, char** argv) {
    using namespace vivora;
    namespace rdv = vivora::net::rdv;

    uint16_t port = DEFAULT_PORT;
    vivora::net::SocketAddr relay_endpoint{};
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if ((std::strcmp(argv[i], "--relay-endpoint") == 0) && i + 1 < argc) {
            relay_endpoint = vivora::net::resolve_host_port(argv[++i]);
            if (relay_endpoint.ip == 0) {
                std::fprintf(stderr, "could not resolve --relay-endpoint %s\n", argv[i]);
                return 1;
            }
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log::error(TAG, "WSAStartup failed");
        return 1;
    }
#else
    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN);
#endif

    auto sock = net::IUdpSocket::create();
    if (!sock || !sock->bind(port)) {
        log::error(TAG, "Failed to bind UDP port %u", port);
        return 1;
    }
    sock->set_recvbuf(1 << 20);   // 1 MiB — comfortable headroom for bursts
    if (relay_endpoint.ip != 0) {
        log::info(TAG, "vivora-rendezvous listening on UDP :%u (TTL=%ds, relay=%s:%u)",
                  port, REGISTRATION_TTL,
                  ip_to_string(relay_endpoint.ip).c_str(), relay_endpoint.port);
    } else {
        log::info(TAG, "vivora-rendezvous listening on UDP :%u (TTL=%ds)",
                  port, REGISTRATION_TTL);
    }
    std::random_device rdv_rd;
    std::mt19937_64    rdv_rng(rdv_rd());

    using PubkeyArr = std::array<uint8_t, 32>;
    std::unordered_map<PubkeyArr, Registration, PubkeyHash> registry;
    // Reverse index: peer_code → pubkey.  Built / refreshed on every
    // Register so a host coming back online with a new reflexive doesn't
    // need to wait for the old code-binding to expire.
    std::unordered_map<std::string, PubkeyArr> code_index;
    std::vector<uint8_t> rxbuf(rdv::MAX_PACKET);
    uint8_t txbuf[rdv::MAX_PACKET];

    auto last_sweep = Clock::now();
    uint64_t total_register = 0, total_lookup = 0, total_punch_hint = 0, total_drop = 0;

    sock->set_nonblocking(true);
    auto next_stats = Clock::now() + std::chrono::seconds(60);
    vivora::ops::MetricsEmitter metrics("rendezvous");   // VIV-72

    while (true) {
#ifndef _WIN32
        if (!g_running) break;
#endif
        net::SocketAddr sender;
        int n = sock->recv_from(rxbuf.data(), rxbuf.size(), sender);

        // Stale-entry sweep — every second is plenty given a 60s TTL.
        const auto now = Clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_sweep).count() >= 1) {
            for (auto it = registry.begin(); it != registry.end(); ) {
                if (it->second.expires_at <= now) {
                    char hex[65];
                    rdv::pubkey_to_hex(it->first.data(), hex);
                    const std::string code = peer_code::encode(it->first.data());
                    log::info(TAG, "expired registration %s (%.16s...) (was at %s:%u)",
                              code.c_str(), hex,
                              ip_to_string(it->second.endpoint.ip).c_str(),
                              it->second.endpoint.port);
                    // Drop matching code → pubkey entry too, but only if it
                    // still points at THIS pubkey (a newer Register may have
                    // taken the code over since the expired one was made).
                    auto ci = code_index.find(code);
                    if (ci != code_index.end() && ci->second == it->first) {
                        code_index.erase(ci);
                    }
                    it = registry.erase(it);
                    ++total_drop;
                } else {
                    ++it;
                }
            }
            last_sweep = now;
        }
        if (now >= next_stats) {
            log::info(TAG, "stats: live=%zu reg=%llu lookup=%llu punch=%llu drop=%llu",
                      registry.size(),
                      (unsigned long long)total_register,
                      (unsigned long long)total_lookup,
                      (unsigned long long)total_punch_hint,
                      (unsigned long long)total_drop);
            // VIV-72: rendezvous only brokers hole-punching, no media egress.
            if (metrics.enabled()) metrics.emit(static_cast<int>(registry.size()), 0);
            next_stats = now + std::chrono::seconds(60);
        }

        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        rdv::MsgType type;
        size_t poff = 0, plen = 0;
        if (!rdv::parse_header(rxbuf.data(), static_cast<size_t>(n), type, poff, plen)) {
            // Bad packet — ignore quietly to avoid log spam from internet noise.
            continue;
        }

        switch (type) {
        case rdv::MsgType::Register:
        case rdv::MsgType::Keepalive: {
            rdv::RegisterPayload reg;
            if (!rdv::decode_register(rxbuf.data() + poff, plen, reg)) break;
            PubkeyArr key;
            std::memcpy(key.data(), reg.pubkey, 32);
            Registration& r = registry[key];
            const bool first_seen = (r.session_id[0] == 0 && r.session_id[1] == 0
                                  && r.session_id[2] == 0 && r.session_id[3] == 0);
            const bool fresh = (r.endpoint != sender);
            r.endpoint   = sender;
            r.expires_at = now + std::chrono::seconds(REGISTRATION_TTL);
            r.lan_count  = reg.lan_count;
            std::memcpy(r.lan, reg.lan, sizeof(r.lan));
            // Mint a stable session_id on first Register; keep it across
            // keepalives so subsequent client lookups land on the same id
            // and the host's relay binding doesn't need to roll.
            if (first_seen) {
                for (int j = 0; j < 4; ++j) {
                    const uint64_t r64 = rdv_rng();
                    std::memcpy(r.session_id + j * 8, &r64, 8);
                }
            }
            ++total_register;
            // Build / refresh the reverse code → pubkey index.  Two distinct
            // pubkeys with the same code are a real collision and we keep
            // the more recently registered one (last-writer-wins) — the
            // log line makes the eviction visible to operators.
            const std::string code = peer_code::encode(reg.pubkey);
            auto code_it = code_index.find(code);
            if (code_it != code_index.end() && code_it->second != key) {
                char old_hex[65];
                rdv::pubkey_to_hex(code_it->second.data(), old_hex);
                log::warn(TAG, "code collision: '%s' rebound from %.16s... → new pubkey",
                          code.c_str(), old_hex);
            }
            code_index[code] = key;
            if (fresh) {
                char hex[65];
                rdv::pubkey_to_hex(reg.pubkey, hex);
                log::info(TAG, "%s %s (%.16s...) at %s:%u (live=%zu)",
                          type == rdv::MsgType::Register ? "registered" : "rebinding",
                          code.c_str(), hex,
                          ip_to_string(sender.ip).c_str(), sender.port,
                          registry.size());
            }
            // Always ack so the host knows the registration landed and
            // gets its actual reflexive endpoint back.
            rdv::RegisterAckPayload ack{};
            ack.reflexive_ip   = sender.ip;
            ack.reflexive_port = sender.port;
            ack.ttl_seconds    = REGISTRATION_TTL;
            if (relay_endpoint.ip != 0) {
                ack.relay_ip   = relay_endpoint.ip;
                ack.relay_port = relay_endpoint.port;
                std::memcpy(ack.session_id, r.session_id, 32);
            }
            const size_t out_len = rdv::encode_register_ack(txbuf, sizeof(txbuf), ack);
            if (out_len) sock->send_to(txbuf, out_len, sender);
            break;
        }

        case rdv::MsgType::LookupByCode: {
            rdv::LookupByCodePayload q;
            if (!rdv::decode_lookup_code(rxbuf.data() + poff, plen, q)) break;
            ++total_lookup;
            // Filter early: malformed / unknown words → respond not-found
            // with a zeroed pubkey so the client doesn't keep retrying.
            rdv::LookupResponsePayload resp{};
            // Echo the client's anti-spoofing nonce (VIV-92) so it can bind
            // this response to its own request.  Legacy clients send no nonce.
            resp.has_nonce = q.has_nonce;
            std::memcpy(resp.nonce, q.nonce, rdv::LOOKUP_NONCE_LEN);
            if (!peer_code::is_well_formed(q.code)) {
                resp.found = 0;
                log::info(TAG, "lookup-by-code '%s': malformed", q.code);
                const size_t rl = rdv::encode_lookup_resp(txbuf, sizeof(txbuf), resp);
                if (rl) sock->send_to(txbuf, rl, sender);
                break;
            }
            auto code_it = code_index.find(std::string(q.code));
            PubkeyArr key{};
            bool have_key = false;
            if (code_it != code_index.end()) {
                key = code_it->second;
                have_key = true;
            }
            if (!have_key) {
                resp.found = 0;
                log::info(TAG, "lookup-by-code '%s' NOT FOUND (client at %s:%u)",
                          q.code, ip_to_string(sender.ip).c_str(), sender.port);
                const size_t rl = rdv::encode_lookup_resp(txbuf, sizeof(txbuf), resp);
                if (rl) sock->send_to(txbuf, rl, sender);
                break;
            }
            // Fall through into the by-pubkey lookup path with `key`
            // already filled — same response shape, same PunchHint logic.
            std::memcpy(resp.pubkey, key.data(), 32);
            auto rit = registry.find(key);
            if (rit != registry.end() && rit->second.expires_at > now) {
                resp.host_ip   = rit->second.endpoint.ip;
                resp.host_port = rit->second.endpoint.port;
                resp.found     = 1;
                resp.lan_count = rit->second.lan_count;
                std::memcpy(resp.lan, rit->second.lan, sizeof(resp.lan));
                if (relay_endpoint.ip != 0) {
                    resp.relay_ip   = relay_endpoint.ip;
                    resp.relay_port = relay_endpoint.port;
                    std::memcpy(resp.session_id, rit->second.session_id, 32);
                }
                rdv::PunchHintPayload hint{};
                hint.client_ip   = sender.ip;
                hint.client_port = sender.port;
                const size_t hl = rdv::encode_punch_hint(txbuf, sizeof(txbuf), hint);
                if (hl) {
                    sock->send_to(txbuf, hl, rit->second.endpoint);
                    ++total_punch_hint;
                }
                log::info(TAG, "lookup-by-code '%s' → %s:%u (client at %s:%u)",
                          q.code,
                          ip_to_string(resp.host_ip).c_str(), resp.host_port,
                          ip_to_string(sender.ip).c_str(), sender.port);
            } else {
                resp.found = 0;
                log::info(TAG, "lookup-by-code '%s': stale registration",
                          q.code);
            }
            const size_t rl = rdv::encode_lookup_resp(txbuf, sizeof(txbuf), resp);
            if (rl) sock->send_to(txbuf, rl, sender);
            break;
        }

        case rdv::MsgType::Lookup: {
            rdv::LookupPayload q;
            if (!rdv::decode_lookup(rxbuf.data() + poff, plen, q)) break;
            ++total_lookup;
            PubkeyArr key;
            std::memcpy(key.data(), q.pubkey, 32);
            auto it = registry.find(key);
            rdv::LookupResponsePayload resp{};
            std::memcpy(resp.pubkey, q.pubkey, 32);
            // Echo the client's anti-spoofing nonce (VIV-92).
            resp.has_nonce = q.has_nonce;
            std::memcpy(resp.nonce, q.nonce, rdv::LOOKUP_NONCE_LEN);
            if (it != registry.end() && it->second.expires_at > now) {
                resp.host_ip   = it->second.endpoint.ip;
                resp.host_port = it->second.endpoint.port;
                resp.found     = 1;
                resp.lan_count = it->second.lan_count;
                std::memcpy(resp.lan, it->second.lan, sizeof(resp.lan));
                if (relay_endpoint.ip != 0) {
                    resp.relay_ip   = relay_endpoint.ip;
                    resp.relay_port = relay_endpoint.port;
                    std::memcpy(resp.session_id, it->second.session_id, 32);
                }
                // Tell the host about the inbound client so both sides can
                // start punching simultaneously.
                rdv::PunchHintPayload hint{};
                hint.client_ip   = sender.ip;
                hint.client_port = sender.port;
                const size_t hlen = rdv::encode_punch_hint(txbuf, sizeof(txbuf), hint);
                if (hlen) {
                    sock->send_to(txbuf, hlen, it->second.endpoint);
                    ++total_punch_hint;
                }
                char hex[65];
                rdv::pubkey_to_hex(q.pubkey, hex);
                log::info(TAG, "lookup %.16s... → %s:%u (client at %s:%u)",
                          hex,
                          ip_to_string(resp.host_ip).c_str(), resp.host_port,
                          ip_to_string(sender.ip).c_str(), sender.port);
            } else {
                resp.found = 0;
                char hex[65];
                rdv::pubkey_to_hex(q.pubkey, hex);
                log::info(TAG, "lookup %.16s... NOT FOUND (client at %s:%u)",
                          hex, ip_to_string(sender.ip).c_str(), sender.port);
            }
            const size_t rlen = rdv::encode_lookup_resp(txbuf, sizeof(txbuf), resp);
            if (rlen) sock->send_to(txbuf, rlen, sender);
            break;
        }

        default:
            // Unknown / server-only message types from the wire — ignore.
            break;
        }
    }

    log::info(TAG, "shutting down (live=%zu)", registry.size());
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
