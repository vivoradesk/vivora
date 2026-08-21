// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "common/net/stun_client.h"
#include "common/utils/log.h"

#include <chrono>
#include <cstring>
#include <random>
#include <thread>

namespace vivora::net {

static const char* TAG = "STUN";

// Wire constants (RFC 5389 §6 / §15).
namespace {
constexpr uint16_t kBindingRequest  = 0x0001;
constexpr uint16_t kBindingResponse = 0x0101;
constexpr uint16_t kAttrMappedAddress    = 0x0001;
constexpr uint16_t kAttrXorMappedAddress = 0x0020;
// Magic cookie is fixed by the RFC; its on-wire bytes are 0x21 0x12 0xA4 0x42.
constexpr uint8_t  kMagicCookieBytes[4] = {0x21, 0x12, 0xA4, 0x42};

void build_binding_request(uint8_t out[20], const uint8_t tid[12]) {
    // Type = Binding Request (0x0001), length = 0, magic cookie, TID.
    out[0] = 0x00; out[1] = 0x01;
    out[2] = 0x00; out[3] = 0x00;
    std::memcpy(out + 4, kMagicCookieBytes, 4);
    std::memcpy(out + 8, tid, 12);
}

uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
} // namespace

SocketAddr StunClient::parse_binding_response(const uint8_t* buf, size_t len,
                                              const uint8_t* expected_tid) {
    SocketAddr result;
    if (!buf || len < 20) return result;

    uint16_t type    = read_u16(buf + 0);
    uint16_t msg_len = read_u16(buf + 2);
    if (type != kBindingResponse) return result;
    if (std::memcmp(buf + 4, kMagicCookieBytes, 4) != 0) return result;
    if (std::memcmp(buf + 8, expected_tid, 12) != 0) return result;
    if (20 + static_cast<size_t>(msg_len) > len) return result;

    SocketAddr xor_addr;
    SocketAddr plain_addr;

    size_t off = 20;
    const size_t end = 20 + msg_len;
    while (off + 4 <= end) {
        uint16_t attr_type = read_u16(buf + off);
        uint16_t attr_len  = read_u16(buf + off + 2);
        size_t val_off = off + 4;
        if (val_off + attr_len > end) break;

        // MAPPED-ADDRESS and XOR-MAPPED-ADDRESS share a layout:
        //   [0] reserved, [1] family (0x01=IPv4, 0x02=IPv6),
        //   [2..3] port, [4..] address bytes.
        if ((attr_type == kAttrXorMappedAddress || attr_type == kAttrMappedAddress)
                && attr_len >= 8) {
            uint8_t family = buf[val_off + 1];
            if (family == 0x01) {
                uint8_t port_bytes[2] = { buf[val_off + 2], buf[val_off + 3] };
                uint8_t ip_bytes[4]   = { buf[val_off + 4], buf[val_off + 5],
                                          buf[val_off + 6], buf[val_off + 7] };
                if (attr_type == kAttrXorMappedAddress) {
                    // XOR with top 2 bytes of magic cookie for the port, and
                    // with the full 4-byte cookie for IPv4. Byte-level XOR is
                    // endian-safe — the RFC defines it exactly this way.
                    port_bytes[0] ^= kMagicCookieBytes[0];
                    port_bytes[1] ^= kMagicCookieBytes[1];
                    for (int i = 0; i < 4; ++i)
                        ip_bytes[i] ^= kMagicCookieBytes[i];
                }
                SocketAddr* target =
                    (attr_type == kAttrXorMappedAddress) ? &xor_addr : &plain_addr;
                target->port = static_cast<uint16_t>((port_bytes[0] << 8) | port_bytes[1]);
                // SocketAddr.ip is documented as network byte order — the wire
                // bytes already are, so a raw memcpy preserves that.
                std::memcpy(&target->ip, ip_bytes, 4);
            }
        }

        // Attributes are padded to 4-byte boundaries on the wire.
        off = val_off + ((attr_len + 3u) & ~3u);
    }

    if (xor_addr.ip != 0) return xor_addr;  // prefer XOR per RFC 5389
    return plain_addr;
}

SocketAddr StunClient::discover(const SocketAddr& stun_server,
                                IUdpSocket& socket,
                                int timeout_ms) {
    SocketAddr result;
    if (stun_server.ip == 0 || stun_server.port == 0) {
        log::warn(TAG, "discover: invalid STUN server address");
        return result;
    }

    uint8_t tid[12];
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        for (auto& b : tid) b = static_cast<uint8_t>(gen() & 0xFF);
    }

    uint8_t req[20];
    build_binding_request(req, tid);

    // Ensure the socket is nonblocking so we can poll with a deadline; our
    // sessions set this anyway, so no behavioral change for callers.
    socket.set_nonblocking(true);

    int sent = socket.send_to(req, sizeof(req), stun_server);
    if (sent <= 0) {
        log::warn(TAG, "send_to(stun) failed");
        return result;
    }

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeout_ms);
    // One retransmit at ~1/3 of the budget absorbs single-packet loss cheaply.
    const auto retransmit_at = start + std::chrono::milliseconds(timeout_ms / 3);
    bool retransmitted = false;

    uint8_t buf[1500];
    while (clock::now() < deadline) {
        SocketAddr from;
        int n = socket.recv_from(buf, sizeof(buf), from);
        if (n > 0) {
            // Filter on source: if other traffic (client_session handshake,
            // stray audio punch, etc.) arrived on this socket before we
            // finish, drop it — we'll get it later from the normal loop
            // once it takes over. (In practice STUN runs before anyone else
            // knows our address, but be defensive.)
            if (from == stun_server) {
                SocketAddr parsed = parse_binding_response(buf, static_cast<size_t>(n), tid);
                if (parsed.ip != 0) return parsed;
                // else: malformed or TID mismatch; keep waiting
            }
        }

        if (!retransmitted && clock::now() >= retransmit_at) {
            socket.send_to(req, sizeof(req), stun_server);
            retransmitted = true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    log::warn(TAG, "no response within %d ms", timeout_ms);
    return result;
}

} // namespace vivora::net
