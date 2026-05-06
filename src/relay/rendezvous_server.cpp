// deskbeam-rendezvous: standalone UDP signalling server.
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

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    deskbeam::net::SocketAddr endpoint;   // reflexive (the source addr the rdv saw)
    TimePoint                 expires_at; // wall-clock cutoff
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
        "usage: %s [--port PORT]\n"
        "  --port PORT     UDP port to listen on (default %u)\n",
        argv0, DEFAULT_PORT);
}

#ifndef _WIN32
volatile sig_atomic_t g_running = 1;
void sigint_handler(int) { g_running = 0; }
#endif

} // namespace

int main(int argc, char** argv) {
    using namespace deskbeam;
    namespace rdv = deskbeam::net::rdv;

    uint16_t port = DEFAULT_PORT;
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
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
    log::info(TAG, "deskbeam-rendezvous listening on UDP :%u (TTL=%ds)",
              port, REGISTRATION_TTL);

    using PubkeyArr = std::array<uint8_t, 32>;
    std::unordered_map<PubkeyArr, Registration, PubkeyHash> registry;
    std::vector<uint8_t> rxbuf(rdv::MAX_PACKET);
    uint8_t txbuf[rdv::MAX_PACKET];

    auto last_sweep = Clock::now();
    uint64_t total_register = 0, total_lookup = 0, total_punch_hint = 0, total_drop = 0;

    sock->set_nonblocking(true);
    auto next_stats = Clock::now() + std::chrono::seconds(60);

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
                    log::info(TAG, "expired registration %.16s... (was at %s:%u)",
                              hex, ip_to_string(it->second.endpoint.ip).c_str(),
                              it->second.endpoint.port);
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
            const bool fresh = (r.endpoint != sender);
            r.endpoint   = sender;
            r.expires_at = now + std::chrono::seconds(REGISTRATION_TTL);
            ++total_register;
            if (fresh) {
                char hex[65];
                rdv::pubkey_to_hex(reg.pubkey, hex);
                log::info(TAG, "%s %.16s... at %s:%u (live=%zu)",
                          type == rdv::MsgType::Register ? "registered" : "rebinding",
                          hex, ip_to_string(sender.ip).c_str(), sender.port,
                          registry.size());
            }
            // Always ack so the host knows the registration landed and
            // gets its actual reflexive endpoint back.
            rdv::RegisterAckPayload ack{};
            ack.reflexive_ip   = sender.ip;
            ack.reflexive_port = sender.port;
            ack.ttl_seconds    = REGISTRATION_TTL;
            const size_t out_len = rdv::encode_register_ack(txbuf, sizeof(txbuf), ack);
            if (out_len) sock->send_to(txbuf, out_len, sender);
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
            if (it != registry.end() && it->second.expires_at > now) {
                resp.host_ip   = it->second.endpoint.ip;
                resp.host_port = it->second.endpoint.port;
                resp.found     = 1;
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
