// Unit tests for the Noise_NK handshake + transport CipherState.
//
// What this covers:
//   1. Handshake round-trip with both sides reaching `complete()`.
//   2. Transport encrypt/decrypt symmetry (initiator->responder AND reverse).
//   3. Authentication failure when the initiator is lied to about the
//      responder's static public key (wrong remote_static_pk).
//   4. Replay protection — decrypting the same wire packet twice fails the
//      second time.
//   5. Tamper detection — a single flipped byte in the ciphertext or tag is
//      rejected by the AEAD.
//   6. Hex encode/decode round-trip on 32-byte keys.
//   7. Handshake payload delivery (the bytes we piggy-back on msg1/msg2
//      arrive intact on the other side).

#include "common/crypto/noise_nk.h"
#include "common/crypto/host_identity.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vivora::crypto;

namespace {

int g_tests_run = 0;
int g_tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++g_tests_run;                                                         \
        if (!(expr)) {                                                         \
            std::fprintf(stderr,                                               \
                         "FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);       \
            ++g_tests_failed;                                                  \
        }                                                                     \
    } while (0)

void test_handshake_roundtrip_and_transport() {
    // Responder (host) has a long-term keypair.
    KeyPair host_static;
    CHECK(generate_x25519_keypair(host_static));
    // Initiator (viewer) also has a long-term keypair — IK sends it in msg1.
    KeyPair client_static;
    CHECK(generate_x25519_keypair(client_static));

    // Initiator knows host's public key out of band.
    HandshakeStateNK initiator;
    HandshakeStateNK responder;
    CHECK(initiator.init_initiator(host_static.public_key, client_static));
    CHECK(responder.init_responder(host_static));

    // Initiator writes msg1 with a small payload.  IK msg1 = e(32) +
    // encrypted static(32+16) + encrypted payload(+16).
    const uint8_t hello[] = {'H', 'E', 'L', 'O'};
    uint8_t msg1[256] = {};
    size_t msg1_len = initiator.write_message(hello, sizeof(hello),
                                              msg1, sizeof(msg1));
    CHECK(msg1_len == 32 + (32 + 16) + sizeof(hello) + 16);

    // Responder reads msg1, extracts the payload.
    uint8_t resp_payload[256] = {};
    int got = responder.read_message(msg1, msg1_len,
                                     resp_payload, sizeof(resp_payload));
    CHECK(got == static_cast<int>(sizeof(hello)));
    CHECK(std::memcmp(resp_payload, hello, sizeof(hello)) == 0);

    // IK property: the responder now holds the initiator's static pubkey.
    uint8_t learned[32] = {};
    CHECK(responder.peer_static_key(learned));
    CHECK(std::memcmp(learned, client_static.public_key, 32) == 0);

    // Responder writes msg2.
    const uint8_t ack[] = {'A', 'C', 'K'};
    uint8_t msg2[256] = {};
    size_t msg2_len = responder.write_message(ack, sizeof(ack),
                                              msg2, sizeof(msg2));
    CHECK(msg2_len == 32 + sizeof(ack) + 16);
    CHECK(responder.complete());

    // Initiator reads msg2, extracts the payload.
    uint8_t init_payload[256] = {};
    int got2 = initiator.read_message(msg2, msg2_len,
                                      init_payload, sizeof(init_payload));
    CHECK(got2 == static_cast<int>(sizeof(ack)));
    CHECK(std::memcmp(init_payload, ack, sizeof(ack)) == 0);
    CHECK(initiator.complete());

    // Finalise — each side derives its own send/recv cipher pair.
    CipherState i_send, i_recv, r_send, r_recv;
    CHECK(initiator.finalize(i_send, i_recv));
    CHECK(responder.finalize(r_send, r_recv));

    // Transport: initiator -> responder.
    {
        const uint8_t pt[] = "video fragment payload i->r";
        uint8_t wire[256] = {};
        size_t wl = i_send.encrypt(pt, sizeof(pt), wire);
        CHECK(wl == sizeof(pt) + CipherState::OVERHEAD);

        uint8_t recovered[256] = {};
        size_t rl = r_recv.decrypt(wire, wl, recovered);
        CHECK(rl == sizeof(pt));
        CHECK(std::memcmp(recovered, pt, sizeof(pt)) == 0);
    }

    // Transport: responder -> initiator.
    {
        const uint8_t pt[] = "input event r->i";
        uint8_t wire[256] = {};
        size_t wl = r_send.encrypt(pt, sizeof(pt), wire);
        CHECK(wl == sizeof(pt) + CipherState::OVERHEAD);

        uint8_t recovered[256] = {};
        size_t rl = i_recv.decrypt(wire, wl, recovered);
        CHECK(rl == sizeof(pt));
        CHECK(std::memcmp(recovered, pt, sizeof(pt)) == 0);
    }
}

void test_wrong_host_key_fails_handshake() {
    // Real responder.
    KeyPair real_host;
    CHECK(generate_x25519_keypair(real_host));

    // Attacker's or stale key — initiator thinks *this* is the host.
    KeyPair wrong_host;
    CHECK(generate_x25519_keypair(wrong_host));

    KeyPair client_static;
    CHECK(generate_x25519_keypair(client_static));

    HandshakeStateNK initiator;
    HandshakeStateNK responder;
    CHECK(initiator.init_initiator(wrong_host.public_key, client_static));  // wrong!
    CHECK(responder.init_responder(real_host));

    uint8_t msg1[128] = {};
    size_t msg1_len = initiator.write_message(nullptr, 0, msg1, sizeof(msg1));
    CHECK(msg1_len == 32 + (32 + 16) + 16);  // e + enc_static + enc_empty_payload

    // Responder should fail to authenticate — DH(s, e) produces a different
    // key on each side, so the Poly1305 tag won't validate.
    uint8_t payload[128] = {};
    int got = responder.read_message(msg1, msg1_len, payload, sizeof(payload));
    CHECK(got == -1);
    CHECK(!responder.complete());
}

void test_replay_rejected() {
    // Set up a full session, then try to replay a transport packet.
    KeyPair host_static;
    CHECK(generate_x25519_keypair(host_static));
    KeyPair client_static;
    CHECK(generate_x25519_keypair(client_static));
    HandshakeStateNK initiator, responder;
    initiator.init_initiator(host_static.public_key, client_static);
    responder.init_responder(host_static);

    uint8_t msg1[128], msg2[128], scratch[128];
    size_t l1 = initiator.write_message(nullptr, 0, msg1, sizeof(msg1));
    responder.read_message(msg1, l1, scratch, sizeof(scratch));
    size_t l2 = responder.write_message(nullptr, 0, msg2, sizeof(msg2));
    initiator.read_message(msg2, l2, scratch, sizeof(scratch));

    CipherState i_send, i_recv, r_send, r_recv;
    initiator.finalize(i_send, i_recv);
    responder.finalize(r_send, r_recv);

    const uint8_t pt[] = "payload";
    uint8_t wire[128] = {};
    size_t wl = i_send.encrypt(pt, sizeof(pt), wire);

    uint8_t recovered[128] = {};
    size_t first = r_recv.decrypt(wire, wl, recovered);
    CHECK(first == sizeof(pt));

    // Second decrypt of the *same* wire buffer — nonce hasn't advanced past
    // max_recv_nonce_, so it must be rejected.
    size_t second = r_recv.decrypt(wire, wl, recovered);
    CHECK(second == SIZE_MAX);
}

void test_tamper_rejected() {
    KeyPair host_static;
    CHECK(generate_x25519_keypair(host_static));
    KeyPair client_static;
    CHECK(generate_x25519_keypair(client_static));
    HandshakeStateNK initiator, responder;
    initiator.init_initiator(host_static.public_key, client_static);
    responder.init_responder(host_static);

    uint8_t msg1[128], msg2[128], scratch[128];
    size_t l1 = initiator.write_message(nullptr, 0, msg1, sizeof(msg1));
    responder.read_message(msg1, l1, scratch, sizeof(scratch));
    size_t l2 = responder.write_message(nullptr, 0, msg2, sizeof(msg2));
    initiator.read_message(msg2, l2, scratch, sizeof(scratch));

    CipherState i_send, i_recv, r_send, r_recv;
    initiator.finalize(i_send, i_recv);
    responder.finalize(r_send, r_recv);

    const uint8_t pt[] = "payload";
    uint8_t wire[128] = {};
    size_t wl = i_send.encrypt(pt, sizeof(pt), wire);

    // Flip a bit in the ciphertext portion (byte 9 = first ciphertext byte).
    wire[9] ^= 0x01;

    uint8_t recovered[128] = {};
    size_t got = r_recv.decrypt(wire, wl, recovered);
    CHECK(got == SIZE_MAX);
}

void test_hex_roundtrip() {
    uint8_t original[32];
    for (int i = 0; i < 32; ++i) original[i] = static_cast<uint8_t>(i * 7);

    std::string hex = hex_encode(original, 32);
    CHECK(hex.size() == 64);

    uint8_t decoded[32] = {};
    CHECK(hex_decode_32(hex, decoded));
    CHECK(std::memcmp(original, decoded, 32) == 0);

    // Rejection of malformed input.
    uint8_t dummy[32] = {};
    CHECK(!hex_decode_32("too short", dummy));
    CHECK(!hex_decode_32(std::string(63, 'a'), dummy));
    std::string bad(64, 'a');
    bad[10] = 'z';
    CHECK(!hex_decode_32(bad, dummy));
}

void test_host_identity_persist_and_reload() {
    // Write to an ephemeral path so we don't touch the real user profile.
    const char* tmpdir = std::getenv("TEMP");
    if (!tmpdir) tmpdir = std::getenv("TMP");
    if (!tmpdir) tmpdir = ".";
    std::string path = std::string(tmpdir) + "/vivora_noise_test_key";
    std::remove(path.c_str());

    KeyPair first{};
    CHECK(load_or_create_host_identity(first, path));

    KeyPair second{};
    CHECK(load_or_create_host_identity(second, path));

    CHECK(std::memcmp(first.public_key, second.public_key, 32) == 0);
    CHECK(std::memcmp(first.secret_key, second.secret_key, 32) == 0);

    std::remove(path.c_str());
}

void test_zero_length_payload() {
    // Edge case: handshake with no piggy-backed payload — the AEAD still
    // produces 16-byte tags.  IK msg1 = 32 + (32+16) + 16 = 96; msg2 = 48.
    KeyPair host_static;
    CHECK(generate_x25519_keypair(host_static));
    KeyPair client_static;
    CHECK(generate_x25519_keypair(client_static));
    HandshakeStateNK initiator, responder;
    initiator.init_initiator(host_static.public_key, client_static);
    responder.init_responder(host_static);

    uint8_t msg1[128] = {};
    size_t l1 = initiator.write_message(nullptr, 0, msg1, sizeof(msg1));
    CHECK(l1 == 96);

    uint8_t payload[16] = {};
    int got = responder.read_message(msg1, l1, payload, sizeof(payload));
    CHECK(got == 0);  // empty payload is valid

    uint8_t msg2[128] = {};
    size_t l2 = responder.write_message(nullptr, 0, msg2, sizeof(msg2));
    CHECK(l2 == 48);

    int got2 = initiator.read_message(msg2, l2, payload, sizeof(payload));
    CHECK(got2 == 0);
    CHECK(initiator.complete() && responder.complete());
}

// Sliding replay window (64): packets arriving out of order within the
// window must all decrypt; duplicates and nonces that fell off the back
// must be rejected.  This is the property that makes retx + UDP-reorder
// safe — a late original arriving after its retransmission no longer
// collides with the strict-greater guard.
void test_sliding_replay_window() {
    KeyPair host_static;
    CHECK(generate_x25519_keypair(host_static));

    KeyPair client_static;
    CHECK(generate_x25519_keypair(client_static));
    HandshakeStateNK initiator, responder;
    initiator.init_initiator(host_static.public_key, client_static);
    responder.init_responder(host_static);

    uint8_t msg1[128]{}, msg2[128]{}, tmp[128]{};
    size_t l1 = initiator.write_message(nullptr, 0, msg1, sizeof(msg1));
    responder.read_message(msg1, l1, tmp, sizeof(tmp));
    size_t l2 = responder.write_message(nullptr, 0, msg2, sizeof(msg2));
    initiator.read_message(msg2, l2, tmp, sizeof(tmp));

    CipherState i_send, i_recv, r_send, r_recv;
    initiator.finalize(i_send, i_recv);
    responder.finalize(r_send, r_recv);

    // Produce 70 packets in order, capture their wires so we can replay in
    // any order we like.
    const uint8_t pt[] = "p";
    std::vector<std::vector<uint8_t>> wires;
    wires.reserve(70);
    for (int i = 0; i < 70; ++i) {
        std::vector<uint8_t> w(sizeof(pt) + CipherState::OVERHEAD);
        size_t wl = i_send.encrypt(pt, sizeof(pt), w.data());
        CHECK(wl == w.size());
        wires.push_back(std::move(w));
    }

    // Deliver nonce 10 first — window jumps forward.
    uint8_t plain[32]{};
    CHECK(r_recv.decrypt(wires[10].data(), wires[10].size(), plain) == sizeof(pt));

    // Deliver nonces 0..9 out of order — all must succeed (window covers 64).
    for (int i = 9; i >= 0; --i) {
        CHECK(r_recv.decrypt(wires[i].data(), wires[i].size(), plain) == sizeof(pt));
    }

    // Replay any of them — must now be rejected (bit set in window).
    CHECK(r_recv.decrypt(wires[5].data(), wires[5].size(), plain) == SIZE_MAX);
    CHECK(r_recv.decrypt(wires[10].data(), wires[10].size(), plain) == SIZE_MAX);

    // Jump far forward — nonces within [max-63..max] still work.
    CHECK(r_recv.decrypt(wires[69].data(), wires[69].size(), plain) == sizeof(pt));
    // max = 69, so nonce 6 is exactly at age 63 (edge of window) — still OK.
    CHECK(r_recv.decrypt(wires[6].data(), wires[6].size(), plain) == SIZE_MAX);  // replay of 6
    // nonce 69-63 = 6 already consumed; try an unused one at the edge: 68.
    CHECK(r_recv.decrypt(wires[68].data(), wires[68].size(), plain) == sizeof(pt));

    // Nonces too far behind (age >= 64 from current max=69) must be rejected
    // even if never seen.  Send an unused mid-range nonce first to confirm
    // the window works, then one that has fallen off.
    CHECK(r_recv.decrypt(wires[11].data(), wires[11].size(), plain) == sizeof(pt));
    // After accepting 69+68+11, max_recv_nonce is still 69.  Encrypt a
    // single extra wire, decrypt it to push max to 70, and see nonce 6
    // definitively outside (age 64).
    std::vector<uint8_t> push70(sizeof(pt) + CipherState::OVERHEAD);
    size_t pl = i_send.encrypt(pt, sizeof(pt), push70.data());
    CHECK(r_recv.decrypt(push70.data(), pl, plain) == sizeof(pt));
    // Now try to send nonce 6 again — still replay; use a completely unseen
    // nonce just past the back by producing a synthetic old ciphertext.
    // The cleanest assertion: if age==64 we reject.  Bump max to 70+64=134
    // by sending 64 more packets, then nonce 10 falls off and is rejected.
    for (int i = 0; i < 64; ++i) {
        std::vector<uint8_t> w(sizeof(pt) + CipherState::OVERHEAD);
        i_send.encrypt(pt, sizeof(pt), w.data());
        CHECK(r_recv.decrypt(w.data(), w.size(), plain) == sizeof(pt));
    }
    // Now window covers roughly nonces [71..134]; wires[10] is way behind.
    CHECK(r_recv.decrypt(wires[10].data(), wires[10].size(), plain) == SIZE_MAX);
}

// 4-key finalize: one Noise session keys both the main transport and an
// auxiliary channel (audio socket).  The two channels must:
//   (a) round-trip independently in both directions,
//   (b) use different keys — a main-channel ciphertext fails on aux_recv
//       even though nonce 0 is reused.
void test_finalize_4key_main_and_aux() {
    KeyPair host_static;
    CHECK(generate_x25519_keypair(host_static));

    KeyPair client_static;
    CHECK(generate_x25519_keypair(client_static));
    HandshakeStateNK initiator, responder;
    CHECK(initiator.init_initiator(host_static.public_key, client_static));
    CHECK(responder.init_responder(host_static));

    uint8_t msg1[128] = {};
    size_t l1 = initiator.write_message(nullptr, 0, msg1, sizeof(msg1));
    uint8_t tmp[128] = {};
    CHECK(responder.read_message(msg1, l1, tmp, sizeof(tmp)) == 0);

    uint8_t msg2[128] = {};
    size_t l2 = responder.write_message(nullptr, 0, msg2, sizeof(msg2));
    CHECK(initiator.read_message(msg2, l2, tmp, sizeof(tmp)) == 0);

    CipherState i_main_s, i_main_r, i_aux_s, i_aux_r;
    CipherState r_main_s, r_main_r, r_aux_s, r_aux_r;
    CHECK(initiator.finalize(i_main_s, i_main_r, i_aux_s, i_aux_r));
    CHECK(responder.finalize(r_main_s, r_main_r, r_aux_s, r_aux_r));

    // Main channel round-trip both directions.
    {
        const uint8_t pt[] = "video main i->r";
        uint8_t wire[128]{}, plain[128]{};
        size_t wl = i_main_s.encrypt(pt, sizeof(pt), wire);
        CHECK(r_main_r.decrypt(wire, wl, plain) == sizeof(pt));
        CHECK(std::memcmp(plain, pt, sizeof(pt)) == 0);
    }
    {
        const uint8_t pt[] = "control main r->i";
        uint8_t wire[128]{}, plain[128]{};
        size_t wl = r_main_s.encrypt(pt, sizeof(pt), wire);
        CHECK(i_main_r.decrypt(wire, wl, plain) == sizeof(pt));
        CHECK(std::memcmp(plain, pt, sizeof(pt)) == 0);
    }

    // Aux channel round-trip (only host->client used today, but both work).
    {
        const uint8_t pt[] = "opus frame r->i";
        uint8_t wire[128]{}, plain[128]{};
        size_t wl = r_aux_s.encrypt(pt, sizeof(pt), wire);
        CHECK(i_aux_r.decrypt(wire, wl, plain) == sizeof(pt));
        CHECK(std::memcmp(plain, pt, sizeof(pt)) == 0);
    }

    // Cross-channel must fail: a main-channel ciphertext must not decrypt
    // on the aux recv cipher, even though both sit at nonce 0.  This is the
    // whole point of splitting to 4 keys — a key-mixup bug can't silently
    // work.
    {
        const uint8_t pt[] = "main-only payload";
        uint8_t wire[128]{}, plain[128]{};
        size_t wl = i_main_s.encrypt(pt, sizeof(pt), wire);
        // Reset recv nonce state so replay guard isn't what rejects it — we
        // want AEAD auth to be the gate.  Fresh aux_recv has never seen a
        // packet, so strict-greater nonce accepts 0; auth must still fail.
        CHECK(r_aux_r.decrypt(wire, wl, plain) == SIZE_MAX);
    }
}

} // namespace

int main() {
    test_handshake_roundtrip_and_transport();
    test_wrong_host_key_fails_handshake();
    test_replay_rejected();
    test_tamper_rejected();
    test_hex_roundtrip();
    test_host_identity_persist_and_reload();
    test_zero_length_payload();
    test_sliding_replay_window();
    test_finalize_4key_main_and_aux();

    std::printf("ran %d checks, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
