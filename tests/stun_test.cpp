// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// stun_test.cpp — unit test for StunClient::parse_binding_response.
// Covers: valid XOR-MAPPED-ADDRESS, legacy MAPPED-ADDRESS, XOR preference
// when both are present, TID mismatch, magic cookie mismatch, truncated
// buffers, wrong message type, and IPv6 family is ignored (IPv4-only build).
// We only unit-test the parser — discover() requires a live UDP socket and
// is covered by the smoke test.

#include "common/net/stun_client.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include "check.h"

using namespace vivora::net;

namespace {

constexpr uint8_t kMagicCookie[4] = {0x21, 0x12, 0xA4, 0x42};

// Build a Binding Response header prefix into buf (20 bytes).
void write_header(uint8_t* buf, uint16_t type, uint16_t msg_len,
                  const uint8_t* tid, const uint8_t* cookie = kMagicCookie) {
    buf[0] = static_cast<uint8_t>(type >> 8);
    buf[1] = static_cast<uint8_t>(type & 0xFF);
    buf[2] = static_cast<uint8_t>(msg_len >> 8);
    buf[3] = static_cast<uint8_t>(msg_len & 0xFF);
    std::memcpy(buf + 4, cookie, 4);
    std::memcpy(buf + 8, tid, 12);
}

// Append an IPv4 MAPPED-ADDRESS / XOR-MAPPED-ADDRESS attribute.
//   attr_type: 0x0001 = MAPPED-ADDRESS, 0x0020 = XOR-MAPPED-ADDRESS
//   port:      the *plain* port (we XOR here if attr_type is XOR)
//   ip_be:     the *plain* IPv4 in network byte order (we XOR here if XOR)
void append_ipv4_attr(std::vector<uint8_t>& out, uint16_t attr_type,
                      uint16_t port, uint32_t ip_be) {
    // Attr header.
    out.push_back(static_cast<uint8_t>(attr_type >> 8));
    out.push_back(static_cast<uint8_t>(attr_type & 0xFF));
    out.push_back(0x00);
    out.push_back(0x08);  // length = 8 (reserved + family + port + IPv4)
    // Body: [0]=reserved, [1]=family (0x01 IPv4), [2..3]=port, [4..7]=IP.
    out.push_back(0x00);
    out.push_back(0x01);
    uint8_t port_bytes[2] = {
        static_cast<uint8_t>(port >> 8),
        static_cast<uint8_t>(port & 0xFF)
    };
    uint8_t ip_bytes[4];
    std::memcpy(ip_bytes, &ip_be, 4);
    if (attr_type == 0x0020) {
        port_bytes[0] ^= kMagicCookie[0];
        port_bytes[1] ^= kMagicCookie[1];
        for (int i = 0; i < 4; ++i) ip_bytes[i] ^= kMagicCookie[i];
    }
    out.push_back(port_bytes[0]);
    out.push_back(port_bytes[1]);
    for (int i = 0; i < 4; ++i) out.push_back(ip_bytes[i]);
}

void test_xor_mapped_address() {
    printf("  XOR-MAPPED-ADDRESS parses correctly...\n");
    uint8_t tid[12];
    for (int i = 0; i < 12; ++i) tid[i] = static_cast<uint8_t>(i + 1);

    std::vector<uint8_t> body;
    // 203.0.113.42:40000 — arbitrary example from RFC 5737.
    uint32_t ip_be;
    uint8_t ip_bytes[4] = {203, 0, 113, 42};
    std::memcpy(&ip_be, ip_bytes, 4);
    append_ipv4_attr(body, 0x0020, 40000, ip_be);

    std::vector<uint8_t> pkt(20);
    write_header(pkt.data(), 0x0101, static_cast<uint16_t>(body.size()), tid);
    pkt.insert(pkt.end(), body.begin(), body.end());

    SocketAddr out = StunClient::parse_binding_response(pkt.data(), pkt.size(), tid);
    CHECK(out.port == 40000);
    CHECK(out.ip == ip_be);
}

void test_legacy_mapped_address() {
    printf("  legacy MAPPED-ADDRESS parses correctly...\n");
    uint8_t tid[12] = {0};

    std::vector<uint8_t> body;
    uint32_t ip_be;
    uint8_t ip_bytes[4] = {192, 0, 2, 1};
    std::memcpy(&ip_be, ip_bytes, 4);
    append_ipv4_attr(body, 0x0001, 54321, ip_be);

    std::vector<uint8_t> pkt(20);
    write_header(pkt.data(), 0x0101, static_cast<uint16_t>(body.size()), tid);
    pkt.insert(pkt.end(), body.begin(), body.end());

    SocketAddr out = StunClient::parse_binding_response(pkt.data(), pkt.size(), tid);
    CHECK(out.port == 54321);
    CHECK(out.ip == ip_be);
}

void test_prefers_xor_over_plain() {
    printf("  prefers XOR-MAPPED over legacy MAPPED...\n");
    uint8_t tid[12] = {0xAA};

    uint32_t xor_ip_be, plain_ip_be;
    uint8_t xor_bytes[4]   = {10, 1, 1, 1};
    uint8_t plain_bytes[4] = {192, 168, 0, 1};
    std::memcpy(&xor_ip_be, xor_bytes, 4);
    std::memcpy(&plain_ip_be, plain_bytes, 4);

    std::vector<uint8_t> body;
    append_ipv4_attr(body, 0x0001, 1111, plain_ip_be);
    append_ipv4_attr(body, 0x0020, 2222, xor_ip_be);

    std::vector<uint8_t> pkt(20);
    write_header(pkt.data(), 0x0101, static_cast<uint16_t>(body.size()), tid);
    pkt.insert(pkt.end(), body.begin(), body.end());

    SocketAddr out = StunClient::parse_binding_response(pkt.data(), pkt.size(), tid);
    CHECK(out.port == 2222);
    CHECK(out.ip == xor_ip_be);
}

void test_tid_mismatch_returns_zero() {
    printf("  TID mismatch returns {0,0}...\n");
    uint8_t tid[12];
    for (int i = 0; i < 12; ++i) tid[i] = static_cast<uint8_t>(i);
    uint8_t wrong_tid[12];
    for (int i = 0; i < 12; ++i) wrong_tid[i] = static_cast<uint8_t>(i + 1);

    std::vector<uint8_t> body;
    uint32_t ip_be = 0x0100007F;  // 127.0.0.1
    append_ipv4_attr(body, 0x0020, 5000, ip_be);

    std::vector<uint8_t> pkt(20);
    write_header(pkt.data(), 0x0101, static_cast<uint16_t>(body.size()), tid);
    pkt.insert(pkt.end(), body.begin(), body.end());

    SocketAddr out = StunClient::parse_binding_response(pkt.data(), pkt.size(), wrong_tid);
    CHECK(out.ip == 0 && out.port == 0);
}

void test_cookie_mismatch_returns_zero() {
    printf("  magic cookie mismatch returns {0,0}...\n");
    uint8_t tid[12] = {0};
    uint8_t bad_cookie[4] = {0xDE, 0xAD, 0xBE, 0xEF};

    std::vector<uint8_t> body;
    uint32_t ip_be = 0x0100007F;
    append_ipv4_attr(body, 0x0001, 5000, ip_be);

    std::vector<uint8_t> pkt(20);
    write_header(pkt.data(), 0x0101,
                 static_cast<uint16_t>(body.size()), tid, bad_cookie);
    pkt.insert(pkt.end(), body.begin(), body.end());

    SocketAddr out = StunClient::parse_binding_response(pkt.data(), pkt.size(), tid);
    CHECK(out.ip == 0 && out.port == 0);
}

void test_truncated_buffer_returns_zero() {
    printf("  truncated buffer returns {0,0}...\n");
    uint8_t tid[12] = {0};
    uint8_t buf[10] = {};
    SocketAddr out = StunClient::parse_binding_response(buf, sizeof(buf), tid);
    CHECK(out.ip == 0 && out.port == 0);

    // Null pointer safe.
    out = StunClient::parse_binding_response(nullptr, 0, tid);
    CHECK(out.ip == 0 && out.port == 0);
}

void test_wrong_message_type_returns_zero() {
    printf("  wrong message type returns {0,0}...\n");
    uint8_t tid[12] = {0};
    std::vector<uint8_t> body;
    uint32_t ip_be = 0x0100007F;
    append_ipv4_attr(body, 0x0020, 5000, ip_be);

    std::vector<uint8_t> pkt(20);
    write_header(pkt.data(), 0x0001,  // Binding Request, not Response
                 static_cast<uint16_t>(body.size()), tid);
    pkt.insert(pkt.end(), body.begin(), body.end());

    SocketAddr out = StunClient::parse_binding_response(pkt.data(), pkt.size(), tid);
    CHECK(out.ip == 0 && out.port == 0);
}

void test_ipv6_family_ignored() {
    printf("  IPv6 family attribute is ignored (IPv4-only build)...\n");
    uint8_t tid[12] = {0};
    std::vector<uint8_t> body;
    // Manually build an IPv6 XOR-MAPPED-ADDRESS attr: len=20.
    body.push_back(0x00); body.push_back(0x20);
    body.push_back(0x00); body.push_back(0x14);
    body.push_back(0x00);            // reserved
    body.push_back(0x02);            // family = IPv6
    body.push_back(0x12); body.push_back(0x34);  // xor'd port
    for (int i = 0; i < 16; ++i) body.push_back(0x00);

    std::vector<uint8_t> pkt(20);
    write_header(pkt.data(), 0x0101, static_cast<uint16_t>(body.size()), tid);
    pkt.insert(pkt.end(), body.begin(), body.end());

    SocketAddr out = StunClient::parse_binding_response(pkt.data(), pkt.size(), tid);
    CHECK(out.ip == 0 && out.port == 0);
}

} // namespace

int main() {
    printf("stun_test: starting\n");
    test_xor_mapped_address();
    test_legacy_mapped_address();
    test_prefers_xor_over_plain();
    test_tid_mismatch_returns_zero();
    test_cookie_mismatch_returns_zero();
    test_truncated_buffer_returns_zero();
    test_wrong_message_type_returns_zero();
    test_ipv6_family_ignored();
    return check_report("stun_test: OK");
}
