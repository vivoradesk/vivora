// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// rendezvous_test.cpp — unit test for the VIV-92 anti-spoofing nonce added
// to the rendezvous Lookup / LookupByCode / LookupResponse wire messages.
// Covers: nonce round-trip, backward-compatible legacy (nonce-less) decode,
// all four LookupResponse wire lengths (72/80/110/118), and the exact byte
// lengths so the offset math for the relay+nonce tails can't silently rot.

#include "common/net/rendezvous_protocol.h"
#include <cstdio>
#include <cstring>
#include "check.h"

using namespace vivora::net;

static void test_lookup_nonce_roundtrip() {
    printf("  lookup nonce round-trip...\n");
    rdv::LookupPayload q{};
    for (int i = 0; i < 32; ++i) q.pubkey[i] = static_cast<uint8_t>(i);
    for (size_t i = 0; i < rdv::LOOKUP_NONCE_LEN; ++i)
        q.nonce[i] = static_cast<uint8_t>(0xA0 + i);
    q.has_nonce = true;

    uint8_t buf[rdv::MAX_PACKET];
    const size_t n = rdv::encode_lookup(buf, sizeof(buf), q);
    // 8-byte header + 32 pubkey + 8 nonce.
    CHECK(n == rdv::HEADER_SIZE + 32 + rdv::LOOKUP_NONCE_LEN);

    rdv::MsgType type; size_t poff = 0, plen = 0;
    CHECK(rdv::parse_header(buf, n, type, poff, plen));
    CHECK(type == rdv::MsgType::Lookup);

    rdv::LookupPayload out{};
    CHECK(rdv::decode_lookup(buf + poff, plen, out));
    CHECK(out.has_nonce);
    CHECK(std::memcmp(out.pubkey, q.pubkey, 32) == 0);
    CHECK(std::memcmp(out.nonce, q.nonce, rdv::LOOKUP_NONCE_LEN) == 0);
}

static void test_lookup_legacy_decode() {
    printf("  lookup legacy (nonce-less) decode...\n");
    // A legacy 32-byte Lookup must still decode, with has_nonce=false and a
    // zeroed nonce, so an old client can still be served by a new server.
    rdv::LookupPayload q{};
    for (int i = 0; i < 32; ++i) q.pubkey[i] = static_cast<uint8_t>(0xF0 - i);
    q.has_nonce = false;

    uint8_t buf[rdv::MAX_PACKET];
    const size_t n = rdv::encode_lookup(buf, sizeof(buf), q);
    CHECK(n == rdv::HEADER_SIZE + 32);

    rdv::MsgType type; size_t poff = 0, plen = 0;
    CHECK(rdv::parse_header(buf, n, type, poff, plen));
    rdv::LookupPayload out{};
    CHECK(rdv::decode_lookup(buf + poff, plen, out));
    CHECK(!out.has_nonce);
    uint8_t zero[rdv::LOOKUP_NONCE_LEN] = {};
    CHECK(std::memcmp(out.nonce, zero, rdv::LOOKUP_NONCE_LEN) == 0);
}

static void test_lookup_code_nonce_roundtrip() {
    printf("  lookup-by-code nonce round-trip + legacy...\n");
    rdv::LookupByCodePayload q{};
    std::strncpy(q.code, "brave-otter-4271", sizeof(q.code) - 1);
    for (size_t i = 0; i < rdv::LOOKUP_NONCE_LEN; ++i)
        q.nonce[i] = static_cast<uint8_t>(0x11 * i);
    q.has_nonce = true;

    uint8_t buf[rdv::MAX_PACKET];
    size_t n = rdv::encode_lookup_code(buf, sizeof(buf), q);
    CHECK(n == rdv::HEADER_SIZE + sizeof(q.code) + rdv::LOOKUP_NONCE_LEN);

    rdv::MsgType type; size_t poff = 0, plen = 0;
    CHECK(rdv::parse_header(buf, n, type, poff, plen));
    rdv::LookupByCodePayload out{};
    CHECK(rdv::decode_lookup_code(buf + poff, plen, out));
    CHECK(out.has_nonce);
    CHECK(std::strcmp(out.code, q.code) == 0);
    CHECK(std::memcmp(out.nonce, q.nonce, rdv::LOOKUP_NONCE_LEN) == 0);

    // Legacy 24-byte form.
    rdv::LookupByCodePayload ql{};
    std::strncpy(ql.code, "brave-otter-4271", sizeof(ql.code) - 1);
    ql.has_nonce = false;
    n = rdv::encode_lookup_code(buf, sizeof(buf), ql);
    CHECK(n == rdv::HEADER_SIZE + sizeof(ql.code));
    CHECK(rdv::parse_header(buf, n, type, poff, plen));
    rdv::LookupByCodePayload outl{};
    CHECK(rdv::decode_lookup_code(buf + poff, plen, outl));
    CHECK(!outl.has_nonce);
}

static void roundtrip_resp(bool relay, bool nonce, size_t expect_plen) {
    rdv::LookupResponsePayload r{};
    for (int i = 0; i < 32; ++i) r.pubkey[i] = static_cast<uint8_t>(i * 7);
    r.host_ip   = 0x0100007f;   // 127.0.0.1 network order
    r.host_port = 9876;
    r.found     = 1;
    r.lan_count = 2;
    r.lan[0] = { 0x0200a8c0, 9876, 0 };
    r.lan[1] = { 0x0300a8c0, 9877, 0 };
    if (relay) {
        r.relay_ip   = 0x0400a8c0;
        r.relay_port = 7000;
        for (int i = 0; i < 32; ++i) r.session_id[i] = static_cast<uint8_t>(0x40 + i);
    }
    if (nonce) {
        r.has_nonce = true;
        for (size_t i = 0; i < rdv::LOOKUP_NONCE_LEN; ++i)
            r.nonce[i] = static_cast<uint8_t>(0xC0 + i);
    }

    uint8_t buf[rdv::MAX_PACKET];
    const size_t n = rdv::encode_lookup_resp(buf, sizeof(buf), r);
    CHECK(n == rdv::HEADER_SIZE + expect_plen);

    rdv::MsgType type; size_t poff = 0, plen = 0;
    CHECK(rdv::parse_header(buf, n, type, poff, plen));
    CHECK(type == rdv::MsgType::LookupResponse);
    CHECK(plen == expect_plen);

    rdv::LookupResponsePayload out{};
    CHECK(rdv::decode_lookup_resp(buf + poff, plen, out));
    CHECK(std::memcmp(out.pubkey, r.pubkey, 32) == 0);
    CHECK(out.host_ip == r.host_ip);
    CHECK(out.host_port == r.host_port);
    CHECK(out.found == 1);
    CHECK(out.lan_count == 2);
    CHECK(out.lan[0].ip == r.lan[0].ip && out.lan[0].port == r.lan[0].port);
    CHECK(out.lan[1].ip == r.lan[1].ip && out.lan[1].port == r.lan[1].port);
    CHECK((out.relay_ip != 0) == relay);
    if (relay) {
        CHECK(out.relay_ip == r.relay_ip && out.relay_port == r.relay_port);
        CHECK(std::memcmp(out.session_id, r.session_id, 32) == 0);
    }
    CHECK(out.has_nonce == nonce);
    if (nonce)
        CHECK(std::memcmp(out.nonce, r.nonce, rdv::LOOKUP_NONCE_LEN) == 0);
}

static void test_response_all_forms() {
    printf("  lookup response 72/80/110/118 forms...\n");
    // Base = 40 + 8*MAX_LAN_CANDIDATES (=32) = 72.
    const size_t base = 40 + 8 * rdv::MAX_LAN_CANDIDATES;
    roundtrip_resp(/*relay=*/false, /*nonce=*/false, base);                        // 72
    roundtrip_resp(/*relay=*/false, /*nonce=*/true,  base + rdv::LOOKUP_NONCE_LEN); // 80
    roundtrip_resp(/*relay=*/true,  /*nonce=*/false, base + 38);                    // 110
    roundtrip_resp(/*relay=*/true,  /*nonce=*/true,  base + 38 + rdv::LOOKUP_NONCE_LEN); // 118
}

static void test_response_bad_length_rejected() {
    printf("  malformed response length rejected...\n");
    uint8_t buf[rdv::MAX_PACKET] = {};
    rdv::LookupResponsePayload out{};
    // A length between valid forms (e.g. 100) must be rejected.
    CHECK(!rdv::decode_lookup_resp(buf, 100, out));
    CHECK(!rdv::decode_lookup_resp(buf, 0, out));
}

int main() {
    printf("rendezvous_test:\n");
    test_lookup_nonce_roundtrip();
    test_lookup_legacy_decode();
    test_lookup_code_nonce_roundtrip();
    test_response_all_forms();
    test_response_bad_length_rejected();
    return check_report("All rendezvous tests passed.");
}
