// Minimal smoke test for the vivora-relay binary.  Spins up two virtual
// peers (A, B) on loopback, BINDs each, sends a DATA packet from A, and
// verifies B receives the forwarded payload.  No external dependencies,
// no networking beyond the local loopback.
//
// Usage: vivora-relay --port 7100 must be running on localhost:7100.

#include "common/net/relay_protocol.h"
#include "common/net/socket.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace rly = vivora::net::relay;
using Clock = std::chrono::steady_clock;

static vivora::net::SocketAddr loopback(uint16_t port) {
    vivora::net::SocketAddr a;
    a.ip   = vivora::net::parse_ip("127.0.0.1");
    a.port = port;
    return a;
}

// Receive with a budget — returns bytes read or 0 on timeout.
static int recv_until(vivora::net::IUdpSocket& s, uint8_t* buf, size_t cap,
                      vivora::net::SocketAddr& sender, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        int n = s.recv_from(buf, cap, sender);
        if (n > 0) return n;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
}

int main() {
#ifdef _WIN32
    WSADATA wsa{}; if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::printf("WSA fail\n"); return 1; }
#endif

    auto sA = vivora::net::IUdpSocket::create();
    auto sB = vivora::net::IUdpSocket::create();
    if (!sA->bind(0) || !sB->bind(0)) { std::printf("bind fail\n"); return 1; }
    sA->set_nonblocking(true);
    sB->set_nonblocking(true);

    const auto relay_addr = loopback(7100);

    // Both peers BIND with the same session_id.  In real use the id would
    // come from rendezvous (random per pairing); here we hard-code it.
    rly::BindPayload bA{}, bB{};
    for (int i = 0; i < 32; ++i) {
        bA.session_id[i] = static_cast<uint8_t>(i + 1);
        bB.session_id[i] = static_cast<uint8_t>(i + 1);
    }

    uint8_t buf[rly::MAX_DATA_PACKET];

    // BIND A
    size_t n = rly::encode_bind(buf, sizeof(buf), bA);
    sA->send_to(buf, n, relay_addr);
    vivora::net::SocketAddr sender;
    int got = recv_until(*sA, buf, sizeof(buf), sender, 2000);
    if (got <= 0) { std::printf("FAIL: A no BIND_ACK\n"); return 1; }
    rly::MsgType t; size_t off = 0, plen = 0;
    if (!rly::parse_header(buf, got, t, off, plen) || t != rly::MsgType::BindAck) {
        std::printf("FAIL: A bad BIND_ACK header\n"); return 1;
    }
    rly::BindAckPayload ackA{};
    if (!rly::decode_bind_ack(buf + off, plen, ackA)) { std::printf("FAIL: ackA decode\n"); return 1; }
    std::printf("A bound, alloc=%02x%02x..., paired=%d\n",
                ackA.alloc_id[0], ackA.alloc_id[1], ackA.paired);

    // BIND B (mirror)
    n = rly::encode_bind(buf, sizeof(buf), bB);
    sB->send_to(buf, n, relay_addr);
    got = recv_until(*sB, buf, sizeof(buf), sender, 2000);
    if (got <= 0) { std::printf("FAIL: B no BIND_ACK\n"); return 1; }
    if (!rly::parse_header(buf, got, t, off, plen) || t != rly::MsgType::BindAck) {
        std::printf("FAIL: B bad BIND_ACK header\n"); return 1;
    }
    rly::BindAckPayload ackB{};
    if (!rly::decode_bind_ack(buf + off, plen, ackB)) { std::printf("FAIL: ackB decode\n"); return 1; }
    std::printf("B bound, alloc=%02x%02x..., paired=%d (expected 1)\n",
                ackB.alloc_id[0], ackB.alloc_id[1], ackB.paired);
    if (!ackB.paired) { std::printf("FAIL: B not paired after both bound\n"); return 1; }

    // A sends DATA, expect B to receive the inner payload as-is.
    const char* msg = "hello-from-A";
    n = rly::encode_data(buf, sizeof(buf), ackA.alloc_id,
                        reinterpret_cast<const uint8_t*>(msg),
                        std::strlen(msg));
    sA->send_to(buf, n, relay_addr);

    got = recv_until(*sB, buf, sizeof(buf), sender, 2000);
    if (got <= 0) { std::printf("FAIL: B no forwarded DATA\n"); return 1; }
    if (got != static_cast<int>(std::strlen(msg))
        || std::memcmp(buf, msg, std::strlen(msg)) != 0) {
        std::printf("FAIL: payload mismatch (got %d bytes)\n", got);
        return 1;
    }
    std::printf("PASS: B received forwarded payload (%d bytes)\n", got);

    // Reverse: B → A.
    const char* msg2 = "ping-from-B";
    n = rly::encode_data(buf, sizeof(buf), ackB.alloc_id,
                        reinterpret_cast<const uint8_t*>(msg2),
                        std::strlen(msg2));
    sB->send_to(buf, n, relay_addr);
    got = recv_until(*sA, buf, sizeof(buf), sender, 2000);
    if (got <= 0 || std::memcmp(buf, msg2, std::strlen(msg2)) != 0) {
        std::printf("FAIL: reverse direction\n"); return 1;
    }
    std::printf("PASS: A received reverse forward (%d bytes)\n", got);

    std::printf("ALL GREEN\n");
    return 0;
}
