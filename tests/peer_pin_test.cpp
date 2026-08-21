// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// peer_pin_test.cpp — unit tests for TOFU peer pinning (VIV-23).
// Tests: legacy check_or_pin_peer auto-TOFU, read-only query_peer_pin,
//        pin_peer insert + replace, forget_peer_pin by code / pubkey,
//        file-format back-compat (comments + hand-edits preserved),
//        canonical key fingerprint format.

#include "common/crypto/host_identity.h"
#include "common/crypto/peer_pin.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace vivora::crypto;

static const char* PIN_FILE = "peer_pin_test_pins.txt";

static void reset_file() { std::remove(PIN_FILE); }

static void write_file(const std::string& content) {
    std::ofstream out(PIN_FILE, std::ios::trunc);
    out << content;
}

static std::string read_file() {
    std::ifstream in(PIN_FILE);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void fill_key(uint8_t key[32], uint8_t seed) {
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(seed + i);
}

static void test_legacy_check_or_pin() {
    printf("  legacy check_or_pin_peer auto-TOFU...\n");
    reset_file();
    uint8_t k1[32], k2[32];
    fill_key(k1, 1);
    fill_key(k2, 100);

    assert(check_or_pin_peer("civic-panda-4644", k1, PIN_FILE) == PinResult::NewlyPinned);
    assert(check_or_pin_peer("civic-panda-4644", k1, PIN_FILE) == PinResult::Match);
    assert(check_or_pin_peer("civic-panda-4644", k2, PIN_FILE) == PinResult::Mismatch);
    // Mismatch must NOT modify the file.
    assert(check_or_pin_peer("civic-panda-4644", k1, PIN_FILE) == PinResult::Match);
}

static void test_query() {
    printf("  query_peer_pin (read-only)...\n");
    reset_file();
    uint8_t k1[32], k2[32];
    fill_key(k1, 2);
    fill_key(k2, 200);

    // Missing file → Unknown, and the query must not create the file.
    assert(query_peer_pin("brave-otter-1234", k1, nullptr, PIN_FILE) == PinQuery::Unknown);
    assert(!std::ifstream(PIN_FILE));

    assert(pin_peer("brave-otter-1234", k1, PIN_FILE));
    assert(query_peer_pin("brave-otter-1234", k1, nullptr, PIN_FILE) == PinQuery::Match);

    std::string stored;
    assert(query_peer_pin("brave-otter-1234", k2, &stored, PIN_FILE) == PinQuery::Mismatch);
    assert(stored == hex_encode(k1, 32));

    // Unknown code in an existing file.
    assert(query_peer_pin("other-code-0000", k1, nullptr, PIN_FILE) == PinQuery::Unknown);
}

static void test_pin_replace() {
    printf("  pin_peer replaces on re-pin (Trust new key)...\n");
    reset_file();
    uint8_t k1[32], k2[32];
    fill_key(k1, 3);
    fill_key(k2, 30);

    assert(pin_peer("calm-heron-9999", k1, PIN_FILE));
    assert(pin_peer("calm-heron-9999", k2, PIN_FILE));   // user trusted the new key
    assert(query_peer_pin("calm-heron-9999", k2, nullptr, PIN_FILE) == PinQuery::Match);

    // Exactly one entry for the code remains.
    const std::string body = read_file();
    size_t count = 0, pos = 0;
    while ((pos = body.find("calm-heron-9999", pos)) != std::string::npos) {
        ++count;
        pos += 1;
    }
    assert(count == 1);
}

static void test_forget() {
    printf("  forget_peer_pin by code and by pubkey...\n");
    reset_file();
    uint8_t k1[32], k2[32], k3[32];
    fill_key(k1, 4);
    fill_key(k2, 40);
    fill_key(k3, 44);

    assert(pin_peer("alpha-fox-1111", k1, PIN_FILE));
    assert(pin_peer("beta-owl-2222",  k2, PIN_FILE));
    assert(pin_peer("gamma-elk-3333", k3, PIN_FILE));

    // By code.
    assert(forget_peer_pin("alpha-fox-1111", "", PIN_FILE));
    assert(query_peer_pin("alpha-fox-1111", k1, nullptr, PIN_FILE) == PinQuery::Unknown);

    // By pubkey (covers a peer whose code changed on reinstall).
    assert(forget_peer_pin("", hex_encode(k2, 32), PIN_FILE));
    assert(query_peer_pin("beta-owl-2222", k2, nullptr, PIN_FILE) == PinQuery::Unknown);

    // Unrelated entry untouched.
    assert(query_peer_pin("gamma-elk-3333", k3, nullptr, PIN_FILE) == PinQuery::Match);

    // Forgetting a non-existent peer or with a missing file is success.
    assert(forget_peer_pin("nobody-here-0000", "", PIN_FILE));
    reset_file();
    assert(forget_peer_pin("nobody-here-0000", "", PIN_FILE));
    assert(!std::ifstream(PIN_FILE));   // must not create the file
}

static void test_format_back_compat() {
    printf("  file format back-compat (comments + hand-edits survive)...\n");
    reset_file();
    uint8_t k1[32], k2[32];
    fill_key(k1, 5);
    fill_key(k2, 50);

    // Hand-written file: comment, blank line, single-space separator.
    write_file("# trusted peers\n\nold-peer-7777 " + hex_encode(k1, 32) + "\n");
    assert(query_peer_pin("old-peer-7777", k1, nullptr, PIN_FILE) == PinQuery::Match);

    // A rewrite (pin of another code) must keep the comment and the
    // hand-written entry.
    assert(pin_peer("new-peer-8888", k2, PIN_FILE));
    const std::string body = read_file();
    assert(body.find("# trusted peers") != std::string::npos);
    assert(body.find("old-peer-7777") != std::string::npos);
    assert(query_peer_pin("old-peer-7777", k1, nullptr, PIN_FILE) == PinQuery::Match);
    assert(query_peer_pin("new-peer-8888", k2, nullptr, PIN_FILE) == PinQuery::Match);

    // Legacy reader still accepts what pin_peer wrote.
    assert(check_or_pin_peer("new-peer-8888", k2, PIN_FILE) == PinResult::Match);
}

static void test_fingerprint() {
    printf("  canonical key fingerprint...\n");
    uint8_t k[32] = {};
    k[0] = 0x6d; k[1] = 0x2e; k[2] = 0x0c; k[3] = 0x4a;
    k[4] = 0x7f; k[5] = 0x3b; k[6] = 0x9e; k[7] = 0x11;
    assert(key_fingerprint(k) == "6D2E 0C4A 7F3B 9E11");
    // Hex-string variant matches the byte variant.
    assert(key_fingerprint_hex(hex_encode(k, 32)) == key_fingerprint(k));
    // Malformed / short input is grouped as-is, never crashes.
    assert(key_fingerprint_hex("") == "");
    assert(key_fingerprint_hex("abcd12") == "ABCD 12");
}

int main() {
    printf("peer_pin_test:\n");
    test_legacy_check_or_pin();
    test_query();
    test_pin_replace();
    test_forget();
    test_format_back_compat();
    test_fingerprint();
    reset_file();
    printf("peer_pin_test: ALL PASSED\n");
    return 0;
}
