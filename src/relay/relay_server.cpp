// vivora-relay: standalone UDP forwarder for peers that can't punch
// through their NATs directly (symmetric NAT, CGNAT, blocking firewalls).
// Both peers BIND with their pubkey + the peer's pubkey; once both halves
// arrive, the relay links them and forwards every DATA payload from one
// side to the other.  All session-level encryption is opaque to the relay
// — it only ever sees Noise-encrypted bytes.
//
// V1 has no auth: any peer can BIND.  The Pro-managed instance will be
// built on top by adding a `--require-license` flag that verifies a JWT
// in the BIND envelope before allocating; self-host instances will keep
// running with no auth, exactly like this binary today.
//
// Single thread, single UDP socket.  Per-binding TTL = 60s, keepalives
// every 15-25s on the peer side keep entries fresh.

#include "common/net/relay_protocol.h"
#include "common/crypto/license_token.h"
#include "common/net/socket.h"
#include "common/utils/log.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
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

constexpr const char* TAG          = "RLY";
constexpr uint16_t    DEFAULT_PORT = 7100;
constexpr int         BINDING_TTL  = 60;   // seconds; peers keepalive every TTL/3

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// 8-byte allocation id type with a hash specialisation that fits in the
// stdlib's unordered_map without ceremony.
struct AllocId {
    uint8_t bytes[8] = {};
    bool operator==(const AllocId& o) const {
        return std::memcmp(bytes, o.bytes, 8) == 0;
    }
};
struct AllocIdHash {
    size_t operator()(const AllocId& a) const noexcept {
        uint64_t v = 0;
        std::memcpy(&v, a.bytes, 8);
        return std::hash<uint64_t>{}(v);
    }
};

// Same shape as vivora::net::SocketAddr but used here as map keys, so
// it needs hash + equality.  We define equality on the wrapped value to
// dodge any future change to the struct layout.
struct EndpointKey {
    uint32_t ip;
    uint16_t port;
    bool operator==(const EndpointKey& o) const {
        return ip == o.ip && port == o.port;
    }
};
struct EndpointKeyHash {
    size_t operator()(const EndpointKey& e) const noexcept {
        return std::hash<uint64_t>{}((static_cast<uint64_t>(e.ip) << 16) | e.port);
    }
};

struct SessionId {
    std::array<uint8_t, 32> v;
    bool operator==(const SessionId& o) const { return v == o.v; }
};
struct SessionIdHash {
    size_t operator()(const SessionId& k) const noexcept {
        size_t h = 0;
        for (size_t i = 0; i < 32; i += 8) {
            uint64_t word = 0;
            std::memcpy(&word, k.v.data() + i, 8);
            h ^= std::hash<uint64_t>{}(word) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        }
        return h;
    }
};

// One side of a paired session.  `paired_alloc` is the alloc id of the
// other peer once both halves bound; until then it's unset.
struct Binding {
    vivora::net::SocketAddr endpoint;
    SessionId                 session_id;
    AllocId                   paired_alloc;
    bool                      paired = false;
    TimePoint                 expires_at;
};

std::string ip_to_string(uint32_t ip_be) {
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &ip_be, buf, sizeof(buf));
    return std::string(buf);
}

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s [--port PORT] [--require-license PUBKEY_FILE]\n"
        "  --port PORT              UDP port to listen on (default %u)\n"
        "  --require-license PATH   Reject BINDs without a valid Ed25519-signed\n"
        "                           license token (verified against PUBKEY_FILE,\n"
        "                           a 32-byte raw public key).  Off by default —\n"
        "                           AGPL self-host instances stay open.\n",
        argv0, DEFAULT_PORT);
}

// Read exactly N bytes from a file into out[].  Returns true on success.
bool read_file_exact(const char* path, uint8_t* out, size_t n) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n));
    return static_cast<size_t>(in.gcount()) == n;
}

#ifndef _WIN32
volatile sig_atomic_t g_running = 1;
void sigint_handler(int) { g_running = 0; }
#endif

void hex_short(const uint8_t pubkey[32], char out[17]) {
    static const char* H = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        out[i * 2 + 0] = H[(pubkey[i] >> 4) & 0xf];
        out[i * 2 + 1] = H[ pubkey[i]       & 0xf];
    }
    out[16] = '\0';
}

} // namespace

int main(int argc, char** argv) {
    using namespace vivora;
    namespace rly = vivora::net::relay;

    uint16_t port = DEFAULT_PORT;
    bool require_license = false;
    uint8_t license_pubkey[32] = {};
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if ((std::strcmp(argv[i], "--require-license") == 0) && i + 1 < argc) {
            const char* path = argv[++i];
            if (!read_file_exact(path, license_pubkey, 32)) {
                std::fprintf(stderr, "failed to read 32-byte license pubkey from %s\n", path);
                return 1;
            }
            require_license = true;
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
    sock->set_recvbuf(4 << 20);
    sock->set_sendbuf(4 << 20);
    sock->set_nonblocking(true);
    log::info(TAG, "vivora-relay listening on UDP :%u (TTL=%ds)%s",
              port, BINDING_TTL,
              require_license ? "  [license required]" : "");

    // Allocations and the indexes we lookup from each path:
    //   alloc_id → Binding   (DATA / KEEPALIVE arrive with alloc_id)
    //   session_id → list<alloc_id>   (BIND looks for the matching half)
    //   endpoint → alloc_id   (cleanup: drop the binding when the peer
    //                          rebinds from the same endpoint)
    std::unordered_map<AllocId,   Binding,            AllocIdHash>     bindings;
    std::unordered_map<SessionId, std::vector<AllocId>, SessionIdHash> by_session;
    std::unordered_map<EndpointKey, AllocId, EndpointKeyHash>          by_endpoint;

    std::random_device rd;
    std::mt19937_64    rng(rd());

    auto erase_binding = [&](const AllocId& id) {
        auto it = bindings.find(id);
        if (it == bindings.end()) return;
        // If the binding is paired, mark the other side as un-paired so it
        // doesn't try to forward to a now-gone endpoint.  The peer's
        // keepalives will keep it alive until its own TTL expires.
        if (it->second.paired) {
            auto pit = bindings.find(it->second.paired_alloc);
            if (pit != bindings.end()) pit->second.paired = false;
        }
        // Drop our entry from the session_id slot.
        auto sit = by_session.find(it->second.session_id);
        if (sit != by_session.end()) {
            auto& list = sit->second;
            list.erase(std::remove(list.begin(), list.end(), id), list.end());
            if (list.empty()) by_session.erase(sit);
        }
        EndpointKey ek{ it->second.endpoint.ip, it->second.endpoint.port };
        by_endpoint.erase(ek);
        bindings.erase(it);
    };

    std::vector<uint8_t> rxbuf(rly::MAX_DATA_PACKET);
    uint8_t txbuf[rly::MAX_DATA_PACKET];

    auto last_sweep = Clock::now();
    auto next_stats = Clock::now() + std::chrono::seconds(60);
    uint64_t total_bind = 0, total_data = 0, total_drop = 0, total_unpaired_drop = 0;

    while (true) {
#ifndef _WIN32
        if (!g_running) break;
#endif
        net::SocketAddr sender;
        const int n = sock->recv_from(rxbuf.data(), rxbuf.size(), sender);

        const auto now = Clock::now();

        // 1Hz stale sweep.
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_sweep).count() >= 1) {
            for (auto it = bindings.begin(); it != bindings.end(); ) {
                if (it->second.expires_at <= now) {
                    AllocId id = it->first;
                    ++it;
                    erase_binding(id);
                    ++total_drop;
                } else {
                    ++it;
                }
            }
            last_sweep = now;
        }
        if (now >= next_stats) {
            log::info(TAG, "stats: bindings=%zu bind=%llu data=%llu drop=%llu unpaired=%llu",
                      bindings.size(),
                      (unsigned long long)total_bind,
                      (unsigned long long)total_data,
                      (unsigned long long)total_drop,
                      (unsigned long long)total_unpaired_drop);
            next_stats = now + std::chrono::seconds(60);
        }

        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        rly::MsgType type;
        size_t poff = 0, plen = 0;
        if (!rly::parse_header(rxbuf.data(), static_cast<size_t>(n), type, poff, plen)) {
            // Bad packet — silent drop (internet noise).
            continue;
        }

        switch (type) {
        case rly::MsgType::Bind: {
            rly::BindPayload p{};
            if (!rly::decode_bind(rxbuf.data() + poff, plen, p)) break;
            ++total_bind;

            // License check (managed-relay mode only).  Reject quietly with
            // a paired=0 / ttl=0 ack so a probing client gets a definitive
            // "no" without us spending CPU on real allocation.
            if (require_license) {
                if (!p.has_license) {
                    log::info(TAG, "reject bind from %s:%u: no license",
                              ip_to_string(sender.ip).c_str(), sender.port);
                    rly::BindAckPayload ack{};
                    const size_t ackn = rly::encode_bind_ack(txbuf, sizeof(txbuf), ack);
                    if (ackn) sock->send_to(txbuf, ackn, sender);
                    break;
                }
                vivora::crypto::LicenseClaims claims;
                const int64_t now_unix =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                if (!vivora::crypto::verify_license(p.license, license_pubkey,
                                                      now_unix, claims)) {
                    log::info(TAG, "reject bind from %s:%u: invalid / expired license",
                              ip_to_string(sender.ip).c_str(), sender.port);
                    rly::BindAckPayload ack{};
                    const size_t ackn = rly::encode_bind_ack(txbuf, sizeof(txbuf), ack);
                    if (ackn) sock->send_to(txbuf, ackn, sender);
                    break;
                }
                // Pro-tier required for managed relay.  Trial tier passes
                // verification but doesn't get to bind here.
                if (claims.tier != vivora::crypto::LicenseTier::Pro) {
                    log::info(TAG, "reject bind from %s:%u: trial tier",
                              ip_to_string(sender.ip).c_str(), sender.port);
                    rly::BindAckPayload ack{};
                    const size_t ackn = rly::encode_bind_ack(txbuf, sizeof(txbuf), ack);
                    if (ackn) sock->send_to(txbuf, ackn, sender);
                    break;
                }
            }

            // If the same endpoint is already bound, drop the old binding
            // (rebind on reconnect / source-port change is the only sane
            // semantic; keeping both creates ghost entries).
            EndpointKey ek{ sender.ip, sender.port };
            auto eit = by_endpoint.find(ek);
            if (eit != by_endpoint.end()) erase_binding(eit->second);

            // Mint a fresh alloc_id.  64 random bits is plenty for the
            // expected scale (collision after sqrt(2^64) ≈ 4 billion
            // active bindings).
            AllocId id;
            const uint64_t r = rng();
            std::memcpy(id.bytes, &r, 8);

            Binding b;
            b.endpoint    = sender;
            std::memcpy(b.session_id.v.data(), p.session_id, 32);
            b.expires_at  = now + std::chrono::seconds(BINDING_TTL);
            b.paired      = false;

            // Find any existing binding with the same session_id — if one
            // is waiting, link them.  We only support pairing two peers
            // per session_id; further binds with the same id are rejected
            // (would otherwise create a triangle that can't forward
            // unambiguously).
            auto& slot = by_session[b.session_id];
            if (slot.size() >= 2) {
                log::warn(TAG, "session_id collision: third bind from %s:%u rejected",
                          ip_to_string(sender.ip).c_str(), sender.port);
                rly::BindAckPayload ack{};
                ack.paired = 0;
                ack.ttl_seconds = 0;
                const size_t ackn = rly::encode_bind_ack(txbuf, sizeof(txbuf), ack);
                if (ackn) sock->send_to(txbuf, ackn, sender);
                break;
            }
            if (slot.size() == 1) {
                auto other = bindings.find(slot[0]);
                if (other != bindings.end()) {
                    b.paired       = true;
                    b.paired_alloc = slot[0];
                    other->second.paired       = true;
                    other->second.paired_alloc = id;
                }
            }

            slot.push_back(id);
            by_endpoint[ek] = id;
            bindings[id]    = b;

            char sid_short[17];
            hex_short(p.session_id, sid_short);
            log::info(TAG, "bind sid=%s.. from %s:%u alloc=%02x%02x%02x%02x%02x%02x%02x%02x paired=%d",
                      sid_short,
                      ip_to_string(sender.ip).c_str(), sender.port,
                      id.bytes[0], id.bytes[1], id.bytes[2], id.bytes[3],
                      id.bytes[4], id.bytes[5], id.bytes[6], id.bytes[7],
                      b.paired ? 1 : 0);

            // Reply.
            rly::BindAckPayload ack{};
            std::memcpy(ack.alloc_id, id.bytes, 8);
            ack.paired      = b.paired ? 1 : 0;
            ack.ttl_seconds = BINDING_TTL;
            const size_t ackn = rly::encode_bind_ack(txbuf, sizeof(txbuf), ack);
            if (ackn) sock->send_to(txbuf, ackn, sender);
            break;
        }

        case rly::MsgType::Keepalive: {
            rly::KeepalivePayload p{};
            if (!rly::decode_keepalive(rxbuf.data() + poff, plen, p)) break;
            AllocId id; std::memcpy(id.bytes, p.alloc_id, 8);
            auto it = bindings.find(id);
            if (it == bindings.end()) break;
            // Verify endpoint matches — we don't want a wrong source to
            // refresh someone else's binding (cheap forgery defence; the
            // alloc_id is 64 random bits but better safe than sorry).
            if (it->second.endpoint != sender) break;
            it->second.expires_at = now + std::chrono::seconds(BINDING_TTL);
            break;
        }

        case rly::MsgType::Data: {
            uint8_t alloc[8];
            const uint8_t* payload = nullptr;
            size_t payload_len = 0;
            if (!rly::decode_data(rxbuf.data() + poff, plen, alloc, &payload, &payload_len))
                break;
            AllocId id; std::memcpy(id.bytes, alloc, 8);
            auto it = bindings.find(id);
            if (it == bindings.end()) { ++total_drop; break; }
            if (it->second.endpoint != sender) { ++total_drop; break; }
            if (!it->second.paired) { ++total_unpaired_drop; break; }
            auto pit = bindings.find(it->second.paired_alloc);
            if (pit == bindings.end()) { ++total_unpaired_drop; break; }
            ++total_data;
            // Forward the opaque payload as-is — the receiving peer's
            // socket will see it as if it came directly from the relay's
            // IP.  Session-level Noise crypto handles the rest.
            sock->send_to(payload, payload_len, pit->second.endpoint);
            // Bump TTL on traffic — it's effectively a keepalive.
            it->second.expires_at = now + std::chrono::seconds(BINDING_TTL);
            break;
        }

        default:
            // BindAck shouldn't arrive at the server; silently ignore.
            break;
        }
    }

    log::info(TAG, "shutting down (bindings=%zu)", bindings.size());
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
