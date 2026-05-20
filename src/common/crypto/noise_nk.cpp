#include "common/crypto/noise_nk.h"
#include "common/crypto/random.h"

extern "C" {
#include "monocypher.h"
}

#include <cstring>

namespace vivora::crypto {

// Protocol name fixes the handshake pattern, DH, cipher, and hash so that
// any mismatch between peers shows up as a handshake failure rather than
// silent interop. See Noise spec §8.
static const char kProtocolName[] = "Noise_NK_25519_ChaChaPoly_BLAKE2b";
// BLAKE2b produces 64-byte digests — Noise HASHLEN for this variant.
static constexpr size_t HASHLEN    = 64;
// BLAKE2b internal block size — needed for HMAC construction.
static constexpr size_t BLOCK_SIZE = 128;
// X25519 key + AEAD tag — both 32 and 16 bytes respectively.
static constexpr size_t DHLEN = 32;
static constexpr size_t TAGLEN = 16;

// --- small helpers ----------------------------------------------------------

static void blake2b512(uint8_t out[HASHLEN], const uint8_t* data, size_t len) {
    crypto_blake2b(out, HASHLEN, data, len);
}

// HMAC-BLAKE2b-512.  Noise uses HMAC (not BLAKE2b's keyed mode) because the
// spec is parametric in the hash: substituting HMAC keeps the same security
// proof regardless of which hash we plug in.
static void hmac_blake2b(uint8_t out[HASHLEN],
                         const uint8_t* key, size_t key_len,
                         const uint8_t* msg, size_t msg_len) {
    uint8_t block_key[BLOCK_SIZE];
    std::memset(block_key, 0, BLOCK_SIZE);
    if (key_len > BLOCK_SIZE) {
        blake2b512(block_key, key, key_len);
        // remaining bytes already zero
    } else {
        std::memcpy(block_key, key, key_len);
    }

    uint8_t i_key[BLOCK_SIZE];
    uint8_t o_key[BLOCK_SIZE];
    for (size_t i = 0; i < BLOCK_SIZE; ++i) {
        i_key[i] = block_key[i] ^ 0x36;
        o_key[i] = block_key[i] ^ 0x5c;
    }

    // inner = BLAKE2b(i_key || msg)
    uint8_t inner[HASHLEN];
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, HASHLEN);
    crypto_blake2b_update(&ctx, i_key, BLOCK_SIZE);
    if (msg_len) crypto_blake2b_update(&ctx, msg, msg_len);
    crypto_blake2b_final(&ctx, inner);

    // outer = BLAKE2b(o_key || inner)
    crypto_blake2b_init(&ctx, HASHLEN);
    crypto_blake2b_update(&ctx, o_key, BLOCK_SIZE);
    crypto_blake2b_update(&ctx, inner, HASHLEN);
    crypto_blake2b_final(&ctx, out);

    crypto_wipe(block_key, sizeof(block_key));
    crypto_wipe(i_key, sizeof(i_key));
    crypto_wipe(o_key, sizeof(o_key));
    crypto_wipe(inner, sizeof(inner));
}

// Noise HKDF(ck, ikm) -> (output1, output2[, output3]). We always request 2.
static void noise_hkdf2(uint8_t out1[HASHLEN], uint8_t out2[HASHLEN],
                        const uint8_t ck[HASHLEN],
                        const uint8_t* ikm, size_t ikm_len) {
    uint8_t temp_k[HASHLEN];
    hmac_blake2b(temp_k, ck, HASHLEN, ikm, ikm_len);
    uint8_t one = 0x01;
    hmac_blake2b(out1, temp_k, HASHLEN, &one, 1);
    uint8_t msg2_buf[HASHLEN + 1];
    std::memcpy(msg2_buf, out1, HASHLEN);
    msg2_buf[HASHLEN] = 0x02;
    hmac_blake2b(out2, temp_k, HASHLEN, msg2_buf, HASHLEN + 1);
    crypto_wipe(temp_k, sizeof(temp_k));
    crypto_wipe(msg2_buf, sizeof(msg2_buf));
}

// 12-byte IETF ChaCha20-Poly1305 nonce: 4 zero bytes || 8-byte LE counter.
// Noise spec §5.2 pins this encoding to keep the derived cipher-state nonce
// interoperable across implementations.
static void build_aead_nonce(uint8_t nonce[12], uint64_t counter) {
    nonce[0] = nonce[1] = nonce[2] = nonce[3] = 0;
    for (int i = 0; i < 8; ++i) {
        nonce[4 + i] = static_cast<uint8_t>((counter >> (8 * i)) & 0xFF);
    }
}

// --- Noise SymmetricState ---------------------------------------------------

struct SymmetricState {
    uint8_t  h [HASHLEN];
    uint8_t  ck[HASHLEN];
    uint8_t  k [32];
    bool     has_key;
    uint64_t n;  // handshake-time AEAD nonce counter

    void init_symmetric(const char* protocol_name) {
        const size_t name_len = std::strlen(protocol_name);
        if (name_len <= HASHLEN) {
            std::memcpy(h, protocol_name, name_len);
            std::memset(h + name_len, 0, HASHLEN - name_len);
        } else {
            blake2b512(h, reinterpret_cast<const uint8_t*>(protocol_name), name_len);
        }
        std::memcpy(ck, h, HASHLEN);
        std::memset(k, 0, 32);
        has_key = false;
        n = 0;
    }

    void mix_hash(const uint8_t* data, size_t len) {
        uint8_t combined_len = 0;  // unused, kept to mirror Noise spec
        (void)combined_len;
        crypto_blake2b_ctx ctx;
        crypto_blake2b_init(&ctx, HASHLEN);
        crypto_blake2b_update(&ctx, h, HASHLEN);
        if (len) crypto_blake2b_update(&ctx, data, len);
        crypto_blake2b_final(&ctx, h);
    }

    void mix_key(const uint8_t* input_key_material, size_t len) {
        uint8_t new_ck[HASHLEN];
        uint8_t temp_k[HASHLEN];
        noise_hkdf2(new_ck, temp_k, ck, input_key_material, len);
        std::memcpy(ck, new_ck, HASHLEN);
        // Noise uses the first 32 bytes of the second HKDF output as the key.
        std::memcpy(k, temp_k, 32);
        has_key = true;
        n = 0;
        crypto_wipe(new_ck, sizeof(new_ck));
        crypto_wipe(temp_k, sizeof(temp_k));
    }

    // EncryptAndHash: in place, writes ciphertext + 16-byte tag into `buf`.
    // Caller MUST size buf for payload_len + TAGLEN bytes.
    void encrypt_and_hash(uint8_t* buf, size_t payload_len) {
        if (!has_key) {
            mix_hash(buf, payload_len);
            return;
        }
        uint8_t nonce[12];
        build_aead_nonce(nonce, n);
        // IETF ChaCha20-Poly1305 variant (12-byte nonce) via Monocypher's
        // streaming API.  The single-shot crypto_aead_lock uses XChaCha
        // (24-byte nonce), which doesn't match the Noise spec for
        // Noise_*_*_ChaChaPoly_*.
        crypto_aead_ctx ctx;
        crypto_aead_init_ietf(&ctx, k, nonce);
        // AD = h, mac written just past ciphertext, in-place encrypt.
        crypto_aead_write(&ctx, buf, buf + payload_len,
                          h, HASHLEN, buf, payload_len);
        crypto_wipe(&ctx, sizeof(ctx));
        n++;
        mix_hash(buf, payload_len + TAGLEN);
    }

    // DecryptAndHash: in place, validates tag and decrypts.
    // `buf` contains ciphertext + 16-byte tag, total `cipher_and_tag_len`.
    // On success: plaintext is in buf[0..cipher_and_tag_len - TAGLEN).
    // Returns true if tag validates, false otherwise (plaintext left garbled).
    bool decrypt_and_hash(uint8_t* buf, size_t cipher_and_tag_len) {
        if (!has_key) {
            mix_hash(buf, cipher_and_tag_len);
            return true;
        }
        if (cipher_and_tag_len < TAGLEN) return false;
        size_t payload_len = cipher_and_tag_len - TAGLEN;
        uint8_t nonce[12];
        build_aead_nonce(nonce, n);
        uint8_t saved_h[HASHLEN];
        std::memcpy(saved_h, h, HASHLEN);
        // MixHash must see the *ciphertext*, not the decrypted bytes, per
        // Noise spec §5.1.  Hash it first before decrypting in place.
        mix_hash(buf, cipher_and_tag_len);
        const uint8_t* tag = buf + payload_len;
        crypto_aead_ctx ctx;
        crypto_aead_init_ietf(&ctx, k, nonce);
        int rc = crypto_aead_read(&ctx, buf, tag, saved_h, HASHLEN,
                                  buf, payload_len);
        crypto_wipe(&ctx, sizeof(ctx));
        if (rc != 0) {
            // Restore h on failure so caller doesn't advance state.
            std::memcpy(h, saved_h, HASHLEN);
            crypto_wipe(saved_h, sizeof(saved_h));
            return false;
        }
        n++;
        crypto_wipe(saved_h, sizeof(saved_h));
        return true;
    }

    // Split: derive two 32-byte cipher keys for transport phase.
    void split(uint8_t key1[32], uint8_t key2[32]) {
        uint8_t t1[HASHLEN];
        uint8_t t2[HASHLEN];
        noise_hkdf2(t1, t2, ck, nullptr, 0);
        std::memcpy(key1, t1, 32);
        std::memcpy(key2, t2, 32);
        crypto_wipe(t1, sizeof(t1));
        crypto_wipe(t2, sizeof(t2));
    }

    // Split4: extend the HKDF-Expand chain with two more output blocks so a
    // single Noise session can key two independent directions (e.g. main
    // video + audio socket) without a second handshake.  T1/T2 are the
    // standard Noise split outputs (first two cipher keys); T3/T4 are
    // additional domain-separated blocks produced by continuing the chain.
    void split4(uint8_t key1[32], uint8_t key2[32],
                uint8_t key3[32], uint8_t key4[32]) {
        // Mirror noise_hkdf2's construction and extend to 4 blocks.
        uint8_t temp_k[HASHLEN];
        hmac_blake2b(temp_k, ck, HASHLEN, nullptr, 0);
        uint8_t t1[HASHLEN], t2[HASHLEN], t3[HASHLEN], t4[HASHLEN];
        uint8_t byte1 = 0x01;
        hmac_blake2b(t1, temp_k, HASHLEN, &byte1, 1);
        uint8_t buf[HASHLEN + 1];
        std::memcpy(buf, t1, HASHLEN); buf[HASHLEN] = 0x02;
        hmac_blake2b(t2, temp_k, HASHLEN, buf, HASHLEN + 1);
        std::memcpy(buf, t2, HASHLEN); buf[HASHLEN] = 0x03;
        hmac_blake2b(t3, temp_k, HASHLEN, buf, HASHLEN + 1);
        std::memcpy(buf, t3, HASHLEN); buf[HASHLEN] = 0x04;
        hmac_blake2b(t4, temp_k, HASHLEN, buf, HASHLEN + 1);
        std::memcpy(key1, t1, 32);
        std::memcpy(key2, t2, 32);
        std::memcpy(key3, t3, 32);
        std::memcpy(key4, t4, 32);
        crypto_wipe(temp_k, sizeof(temp_k));
        crypto_wipe(t1, sizeof(t1));
        crypto_wipe(t2, sizeof(t2));
        crypto_wipe(t3, sizeof(t3));
        crypto_wipe(t4, sizeof(t4));
        crypto_wipe(buf, sizeof(buf));
    }
};

// --- HandshakeState internals -----------------------------------------------

struct HandshakeStateNK::State {
    SymmetricState ss{};
    KeyPair        local_static{};     // responder only
    KeyPair        local_ephemeral{};  // both roles
    uint8_t        remote_static[32] = {};  // initiator only
    uint8_t        remote_ephemeral[32] = {};
};

// --- KeyPair ----------------------------------------------------------------

void KeyPair::wipe() {
    crypto_wipe(public_key, sizeof(public_key));
    crypto_wipe(secret_key, sizeof(secret_key));
}

bool generate_x25519_keypair(KeyPair& out) {
    if (!random_bytes(out.secret_key, 32)) return false;
    // Monocypher's crypto_x25519_public_key internally clamps the scalar,
    // so we don't need to pre-clamp the random bytes ourselves.
    crypto_x25519_public_key(out.public_key, out.secret_key);
    return true;
}

// --- HandshakeStateNK public API --------------------------------------------

HandshakeStateNK::HandshakeStateNK() : s_(std::make_unique<State>()) {}
HandshakeStateNK::~HandshakeStateNK() {
    if (s_) {
        s_->local_static.wipe();
        s_->local_ephemeral.wipe();
    }
}

bool HandshakeStateNK::init_initiator(const uint8_t remote_static_pk[32]) {
    if (!remote_static_pk) return false;
    role_ = Role::Initiator;
    complete_ = false;
    step_ = 0;
    s_->ss.init_symmetric(kProtocolName);
    // NK pre-message: hash the responder's static public key into h so both
    // sides start from the same state.  The responder side mirrors this.
    std::memcpy(s_->remote_static, remote_static_pk, 32);
    s_->ss.mix_hash(s_->remote_static, 32);
    return true;
}

bool HandshakeStateNK::init_responder(const KeyPair& local_static) {
    role_ = Role::Responder;
    complete_ = false;
    step_ = 0;
    s_->ss.init_symmetric(kProtocolName);
    s_->local_static = local_static;
    s_->ss.mix_hash(s_->local_static.public_key, 32);
    return true;
}

size_t HandshakeStateNK::write_message(const uint8_t* payload, size_t payload_len,
                                       uint8_t* out, size_t out_capacity) {
    if (complete_) return 0;
    const size_t needed = 32 + payload_len + TAGLEN;
    if (out_capacity < needed) return 0;

    if (role_ == Role::Initiator && step_ == 0) {
        // -> e, es
        if (!generate_x25519_keypair(s_->local_ephemeral)) return 0;
        std::memcpy(out, s_->local_ephemeral.public_key, 32);
        s_->ss.mix_hash(s_->local_ephemeral.public_key, 32);

        // es: DH(initiator_e_priv, responder_s_pub)
        uint8_t dh[DHLEN];
        crypto_x25519(dh, s_->local_ephemeral.secret_key, s_->remote_static);
        s_->ss.mix_key(dh, DHLEN);
        crypto_wipe(dh, sizeof(dh));

        // Write payload right after the ephemeral key, then EncryptAndHash
        // encrypts in place and appends the tag.
        if (payload_len) std::memcpy(out + 32, payload, payload_len);
        s_->ss.encrypt_and_hash(out + 32, payload_len);
        step_ = 1;
        return needed;
    }

    if (role_ == Role::Responder && step_ == 1) {
        // <- e, ee
        if (!generate_x25519_keypair(s_->local_ephemeral)) return 0;
        std::memcpy(out, s_->local_ephemeral.public_key, 32);
        s_->ss.mix_hash(s_->local_ephemeral.public_key, 32);

        // ee: DH(responder_e_priv, initiator_e_pub)
        uint8_t dh[DHLEN];
        crypto_x25519(dh, s_->local_ephemeral.secret_key, s_->remote_ephemeral);
        s_->ss.mix_key(dh, DHLEN);
        crypto_wipe(dh, sizeof(dh));

        if (payload_len) std::memcpy(out + 32, payload, payload_len);
        s_->ss.encrypt_and_hash(out + 32, payload_len);
        step_ = 2;
        complete_ = true;
        return needed;
    }

    return 0;  // out-of-sequence call
}

int HandshakeStateNK::read_message(const uint8_t* wire, size_t wire_len,
                                   uint8_t* payload_out, size_t payload_out_capacity) {
    if (complete_) return -1;
    if (!wire || wire_len < 32 + TAGLEN) return -1;
    const size_t payload_len = wire_len - 32 - TAGLEN;
    if (payload_out_capacity < payload_len) return -1;

    if (role_ == Role::Responder && step_ == 0) {
        // -> e, es (inbound)
        std::memcpy(s_->remote_ephemeral, wire, 32);
        s_->ss.mix_hash(s_->remote_ephemeral, 32);

        uint8_t dh[DHLEN];
        crypto_x25519(dh, s_->local_static.secret_key, s_->remote_ephemeral);
        s_->ss.mix_key(dh, DHLEN);
        crypto_wipe(dh, sizeof(dh));

        // Decrypt payload in place in a local buffer, then copy out.
        uint8_t tmp[1500];
        if (payload_len + TAGLEN > sizeof(tmp)) return -1;
        std::memcpy(tmp, wire + 32, payload_len + TAGLEN);
        if (!s_->ss.decrypt_and_hash(tmp, payload_len + TAGLEN)) return -1;
        if (payload_len) std::memcpy(payload_out, tmp, payload_len);
        crypto_wipe(tmp, sizeof(tmp));
        step_ = 1;
        return static_cast<int>(payload_len);
    }

    if (role_ == Role::Initiator && step_ == 1) {
        // <- e, ee (inbound)
        std::memcpy(s_->remote_ephemeral, wire, 32);
        s_->ss.mix_hash(s_->remote_ephemeral, 32);

        uint8_t dh[DHLEN];
        crypto_x25519(dh, s_->local_ephemeral.secret_key, s_->remote_ephemeral);
        s_->ss.mix_key(dh, DHLEN);
        crypto_wipe(dh, sizeof(dh));

        uint8_t tmp[1500];
        if (payload_len + TAGLEN > sizeof(tmp)) return -1;
        std::memcpy(tmp, wire + 32, payload_len + TAGLEN);
        if (!s_->ss.decrypt_and_hash(tmp, payload_len + TAGLEN)) return -1;
        if (payload_len) std::memcpy(payload_out, tmp, payload_len);
        crypto_wipe(tmp, sizeof(tmp));
        step_ = 2;
        complete_ = true;
        return static_cast<int>(payload_len);
    }

    return -1;
}

bool HandshakeStateNK::finalize(CipherState& send_cs, CipherState& recv_cs) {
    if (!complete_) return false;
    uint8_t k1[32];
    uint8_t k2[32];
    s_->ss.split(k1, k2);
    // Noise convention: k1 = initiator-to-responder, k2 = responder-to-initiator.
    if (role_ == Role::Initiator) {
        send_cs.set_key(k1);
        recv_cs.set_key(k2);
    } else {
        send_cs.set_key(k2);
        recv_cs.set_key(k1);
    }
    crypto_wipe(k1, sizeof(k1));
    crypto_wipe(k2, sizeof(k2));
    return true;
}

bool HandshakeStateNK::finalize(CipherState& main_send, CipherState& main_recv,
                                CipherState& aux_send,  CipherState& aux_recv) {
    if (!complete_) return false;
    uint8_t k1[32], k2[32], k3[32], k4[32];
    s_->ss.split4(k1, k2, k3, k4);
    // Same Noise direction convention as 2-key finalize: k1 = I->R, k2 = R->I.
    // Aux channel uses k3 = I->R, k4 = R->I so both sides pair the right keys.
    if (role_ == Role::Initiator) {
        main_send.set_key(k1);
        main_recv.set_key(k2);
        aux_send.set_key(k3);
        aux_recv.set_key(k4);
    } else {
        main_send.set_key(k2);
        main_recv.set_key(k1);
        aux_send.set_key(k4);
        aux_recv.set_key(k3);
    }
    crypto_wipe(k1, sizeof(k1));
    crypto_wipe(k2, sizeof(k2));
    crypto_wipe(k3, sizeof(k3));
    crypto_wipe(k4, sizeof(k4));
    return true;
}

// --- CipherState ------------------------------------------------------------

void CipherState::set_key(const uint8_t key[32]) {
    std::memcpy(key_, key, 32);
    send_nonce_ = 0;
    max_recv_nonce_ = 0;
    recv_window_ = 0;
    recv_started_ = false;
}

bool CipherState::window_accept(uint64_t nonce) const {
    if (!recv_started_) return true;
    if (nonce > max_recv_nonce_) return true;
    const uint64_t age = max_recv_nonce_ - nonce;
    if (age >= 64) return false;                 // fell off the back
    const uint64_t bit = 1ULL << age;
    return (recv_window_ & bit) == 0;            // reject if already seen
}

void CipherState::window_commit(uint64_t nonce) {
    if (!recv_started_) {
        recv_started_ = true;
        max_recv_nonce_ = nonce;
        recv_window_ = 1ULL;                     // bit 0 = max_recv_nonce_
        return;
    }
    if (nonce > max_recv_nonce_) {
        const uint64_t shift = nonce - max_recv_nonce_;
        recv_window_ = (shift >= 64) ? 0ULL : (recv_window_ << shift);
        recv_window_ |= 1ULL;                    // mark the new top nonce
        max_recv_nonce_ = nonce;
    } else {
        const uint64_t age = max_recv_nonce_ - nonce;
        recv_window_ |= (1ULL << age);
    }
}

size_t CipherState::encrypt(const uint8_t* plaintext, size_t len, uint8_t* out) {
    if (send_nonce_ == UINT64_MAX) return 0;  // exhaustion — rekey needed
    uint64_t nonce_val = send_nonce_++;
    // Pack nonce at front, little-endian.
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((nonce_val >> (8 * i)) & 0xFF);
    }
    uint8_t aead_nonce[12];
    build_aead_nonce(aead_nonce, nonce_val);
    // AD = empty for transport (the wire-level nonce is public but doesn't
    // need separate authentication — it's part of the input to AEAD).
    // Write ciphertext at out+8, tag at out+8+len.
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key_, aead_nonce);
    crypto_aead_write(&ctx, out + 8, out + 8 + len,
                      nullptr, 0, plaintext, len);
    crypto_wipe(&ctx, sizeof(ctx));
    return 8 + len + TAGLEN;
}

size_t CipherState::decrypt(const uint8_t* wire, size_t wire_len, uint8_t* out) {
    // Returns SIZE_MAX on failure (too-short input, replay, AEAD reject).
    // Zero-byte plaintext is a valid success — packets like IdrRequest carry
    // no payload, and confusing 0 with failure was a real bug.
    if (wire_len < OVERHEAD) return SIZE_MAX;
    const size_t ct_len = wire_len - OVERHEAD;

    uint64_t nonce_val = 0;
    for (int i = 0; i < 8; ++i) {
        nonce_val |= static_cast<uint64_t>(wire[i]) << (8 * i);
    }
    // Replay protection: sliding window of the last 64 nonces.
    // Cheap check first — no AEAD call for replays / too-old nonces.
    if (!window_accept(nonce_val)) return SIZE_MAX;

    uint8_t aead_nonce[12];
    build_aead_nonce(aead_nonce, nonce_val);
    const uint8_t* ct  = wire + 8;
    const uint8_t* tag = wire + 8 + ct_len;
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key_, aead_nonce);
    int rc = crypto_aead_read(&ctx, out, tag, nullptr, 0, ct, ct_len);
    crypto_wipe(&ctx, sizeof(ctx));
    if (rc != 0) return SIZE_MAX;
    // AEAD succeeded — only now is it safe to mark this nonce as used.
    window_commit(nonce_val);
    return ct_len;
}

} // namespace vivora::crypto
