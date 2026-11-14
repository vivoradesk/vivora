// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// Unattended-access password (VIV-151): the IKpsk1 handshake variant, the
// Argon2id key derivation, the key file and the wrong-password throttle.

#include "check.h"
#include "common/crypto/access_password.h"
#include "common/crypto/access_throttle.h"
#include "common/crypto/noise_nk.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace vivora::crypto;

namespace {

struct Pair {
    KeyPair host, viewer;
    Pair() {
        generate_x25519_keypair(host);
        generate_x25519_keypair(viewer);
    }
};

// Runs msg1 through a responder; returns the payload length or -1.
int deliver_msg1(HandshakeStateNK& initiator, HandshakeStateNK& responder) {
    const uint8_t hello[] = {'H', 'I'};
    uint8_t msg1[256], out[256];
    const size_t n = initiator.write_message(hello, sizeof(hello), msg1, sizeof(msg1));
    if (n == 0) return -1;
    return responder.read_message(msg1, n, out, sizeof(out));
}

void test_psk_handshake_completes_and_keys_match() {
    Pair p;
    uint8_t psk[32];
    std::memset(psk, 0x5a, sizeof(psk));

    HandshakeStateNK i, r;
    CHECK(i.init_initiator(p.host.public_key, p.viewer, psk));
    CHECK(r.init_responder(p.host, psk));
    CHECK(i.uses_psk() && r.uses_psk());
    CHECK(deliver_msg1(i, r) == 2);

    uint8_t msg2[128], out[64];
    const size_t n2 = r.write_message(nullptr, 0, msg2, sizeof(msg2));
    CHECK(n2 > 0);
    CHECK(i.read_message(msg2, n2, out, sizeof(out)) == 0);

    CipherState is, ir, rs, rr;
    CHECK(i.finalize(is, ir));
    CHECK(r.finalize(rs, rr));
    const uint8_t pt[] = "after password";
    uint8_t wire[64], plain[64];
    const size_t wl = is.encrypt(pt, sizeof(pt), wire);
    CHECK(rr.decrypt(wire, wl, plain) == sizeof(pt));
}

void test_wrong_psk_rejected_but_viewer_known() {
    Pair p;
    uint8_t right[32], wrong[32];
    std::memset(right, 1, 32);
    std::memset(wrong, 2, 32);

    HandshakeStateNK i, r;
    i.init_initiator(p.host.public_key, p.viewer, wrong);
    r.init_responder(p.host, right);
    CHECK(deliver_msg1(i, r) == -1);
    // The host still learns who tried, for its log.
    uint8_t learned[32];
    CHECK(r.peer_static_key(learned));
    CHECK(std::memcmp(learned, p.viewer.public_key, 32) == 0);
}

void test_plain_and_psk_do_not_cross() {
    Pair p;
    uint8_t psk[32];
    std::memset(psk, 7, 32);
    {
        // A viewer without a password must not open as a psk handshake...
        HandshakeStateNK i, r;
        i.init_initiator(p.host.public_key, p.viewer);
        r.init_responder(p.host, psk);
        CHECK(deliver_msg1(i, r) == -1);
    }
    {
        // ...nor a password viewer as a plain one: a host without a
        // password set must not let it in past the approval prompt.
        HandshakeStateNK i, r;
        i.init_initiator(p.host.public_key, p.viewer, psk);
        r.init_responder(p.host);
        CHECK(deliver_msg1(i, r) == -1);
    }
    {
        // Plain IK still works unchanged.
        HandshakeStateNK i, r;
        i.init_initiator(p.host.public_key, p.viewer);
        r.init_responder(p.host);
        CHECK(!r.uses_psk());
        CHECK(deliver_msg1(i, r) == 2);
    }
}

void test_derivation_is_salted_by_host() {
    Pair a, b;
    uint8_t k1[32], k2[32], k3[32], k4[32];
    CHECK(derive_access_psk("correct horse", a.host.public_key, k1));
    CHECK(derive_access_psk("correct horse", a.host.public_key, k2));
    CHECK(derive_access_psk("correct horse", b.host.public_key, k3));
    CHECK(derive_access_psk("correct horsf", a.host.public_key, k4));
    CHECK(std::memcmp(k1, k2, 32) == 0);
    CHECK(std::memcmp(k1, k3, 32) != 0);
    CHECK(std::memcmp(k1, k4, 32) != 0);
}

void test_key_file_roundtrip() {
    Pair a, b;
    uint8_t psk[32], got[32];
    std::memset(psk, 0x33, 32);
    const std::string path = "access_psk_test.bin";
    std::remove(path.c_str());

    CHECK(!load_access_psk(path, a.host.public_key, got));
    CHECK(save_access_psk(path, a.host.public_key, psk));
    CHECK(load_access_psk(path, a.host.public_key, got));
    CHECK(std::memcmp(got, psk, 32) == 0);
    // Tied to the host key it was made for.
    CHECK(!load_access_psk(path, b.host.public_key, got));
    CHECK(clear_access_psk(path));
    CHECK(!load_access_psk(path, a.host.public_key, got));
    CHECK(clear_access_psk(path));   // already gone is fine
}

void test_path_sits_next_to_host_key() {
    CHECK(access_psk_path_for("C:\\x\\Vivora\\host_key") == "C:\\x\\Vivora\\access_psk");
    CHECK(access_psk_path_for("/etc/vivora/host_key") == "/etc/vivora/access_psk");
    CHECK(access_psk_path_for("host_key") == "access_psk");
}

void test_throttle_per_source() {
    AccessThrottle::Config cfg;
    cfg.per_source_limit = 3;
    cfg.global_limit     = 100;
    cfg.window_ms        = 1000;
    cfg.source_lockout_ms = 5000;
    AccessThrottle t(cfg);
    const uint32_t a = 1, b = 2;

    CHECK(t.blocked_for_ms(a, 0) == 0);
    CHECK(!t.record_failure(a, 0));
    CHECK(!t.record_failure(a, 10));
    CHECK(t.record_failure(a, 20));          // third one locks
    CHECK(t.blocked_for_ms(a, 20) == 5000);
    CHECK(t.blocked_for_ms(b, 20) == 0);     // other addresses unaffected
    CHECK(t.blocked_for_ms(a, 5020) == 0);

    // Failures spread wider than the window never add up.
    CHECK(!t.record_failure(b, 0));
    CHECK(!t.record_failure(b, 600));
    CHECK(!t.record_failure(b, 1700));
    CHECK(!t.record_failure(b, 2000));
    CHECK(t.blocked_for_ms(b, 2000) == 0);

    // Success clears the address.
    t.record_success(b);
    CHECK(!t.record_failure(b, 2100));
    CHECK(!t.record_failure(b, 2200));
}

void test_throttle_global() {
    AccessThrottle::Config cfg;
    cfg.per_source_limit  = 100;
    cfg.global_limit      = 4;
    cfg.window_ms         = 1000;
    cfg.global_lockout_ms = 3000;
    AccessThrottle t(cfg);
    // An attacker rotating addresses still trips the global limit.
    CHECK(!t.record_failure(10, 0));
    CHECK(!t.record_failure(11, 1));
    CHECK(!t.record_failure(12, 2));
    CHECK(t.record_failure(13, 3));
    CHECK(t.blocked_for_ms(99, 3) == 3000);
    // A success does not lift the global lockout.
    t.record_success(13);
    CHECK(t.blocked_for_ms(13, 100) == 2903);
    CHECK(t.blocked_for_ms(99, 3003) == 0);
}

} // namespace

int main() {
    test_psk_handshake_completes_and_keys_match();
    test_wrong_psk_rejected_but_viewer_known();
    test_plain_and_psk_do_not_cross();
    test_derivation_is_salted_by_host();
    test_key_file_roundtrip();
    test_path_sits_next_to_host_key();
    test_throttle_per_source();
    test_throttle_global();
    return check_report("access_password_test");
}
