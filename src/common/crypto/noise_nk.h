#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace deskbeam::crypto {

// Noise_NK_25519_ChaChaPoly_BLAKE2b — a Noise handshake pattern where the
// responder's static public key is known to the initiator in advance.
//
// Pattern:
//     <- s                   (pre-message: responder's static is pre-shared)
//     ...
//     -> e, es               (msg1: initiator sends ephemeral, mixes DH(e,rs))
//     <- e, ee               (msg2: responder sends ephemeral, mixes DH(ee))
//
// After these two messages both sides hold a pair of CipherStates — one for
// each direction — and the handshake is complete.  All subsequent data is
// authenticated + encrypted with ChaCha20-Poly1305.
//
// We pick NK (not XX or IK) because:
//   - The host's long-term public key is its peer identity; we ship it
//     out-of-band (QR / copy-paste / --host-key CLI) before connecting.
//   - NK gives forward secrecy (ephemeral DH) and responder authentication
//     (static DH) in one round-trip — the minimum for a working UDP session.
//   - The initiator does not authenticate with a static key; that's fine
//     for a remote-desktop client where the UI handles auth at a higher
//     layer (PIN, pairing flow).
//
// Cryptography is provided by Monocypher:
//   - X25519 for DH
//   - ChaCha20-Poly1305 (IETF 12-byte-nonce variant) for AEAD
//   - BLAKE2b-512 for hashing; HKDF uses HMAC-BLAKE2b as per Noise spec.

struct KeyPair {
    uint8_t public_key[32];
    uint8_t secret_key[32];

    // Overwrite both buffers with zero.  Call before dropping a secret that
    // hasn't lived its full lifetime (error path, test teardown, etc.).
    void wipe();
};

// Generate a fresh Curve25519 keypair from the platform CSPRNG.
// Returns false on CSPRNG failure — treat as fatal.
bool generate_x25519_keypair(KeyPair& out);

// One direction of an established session.  After Noise's Split() step we
// end up with two of these: one we encrypt outbound traffic with, one we
// decrypt inbound traffic with.  Never cross the two.
class CipherState {
public:
    // Transport overhead added on top of every plaintext packet.
    // = 8-byte explicit nonce counter + 16-byte Poly1305 tag.
    static constexpr size_t OVERHEAD = 8 + 16;

    // Encrypt `plaintext` (len bytes) into `out`.  `out` must be at least
    // `len + OVERHEAD` bytes. Wire layout:
    //   [0..8)    : nonce (u64 LE, monotonically increasing per call)
    //   [8..8+len): ciphertext
    //   [8+len..] : 16-byte Poly1305 tag
    // Returns total wire length on success (len + OVERHEAD).
    // Returns 0 on nonce exhaustion (2^64 packets per session — would take
    //   centuries at any real bitrate, but we still check).
    size_t encrypt(const uint8_t* plaintext, size_t len, uint8_t* out);

    // Decrypt a wire packet produced by encrypt().  `wire` points at the
    // full encrypted record (nonce + ciphertext + tag).  On success writes
    // plaintext into `out` (must be at least `wire_len - OVERHEAD` bytes)
    // and returns plaintext length.  Returns 0 on any auth failure — this
    // includes tampering, wrong key, or replay of an old nonce.
    //
    // Replay protection with a 64-nonce sliding window.  A nonce is accepted
    // if (a) it's strictly greater than the highest seen so far, advancing
    // the window, or (b) it's within the last 64 nonces and the matching
    // bit in recv_window_ isn't set yet (i.e. not a replay).  Rejection
    // reasons: older than the window, duplicate within the window, or AEAD
    // auth failure.  A window of 64 tolerates typical UDP bursts/reorder
    // (including late originals that race retransmissions) without losing
    // data, while still catching attacker replays.
    size_t decrypt(const uint8_t* wire, size_t wire_len, uint8_t* out);

    // Raw access for key derivation from an established handshake.
    // Caller MUST NOT use this for anything other than the `Split` step.
    void set_key(const uint8_t key[32]);

private:
    // Check the replay window for `nonce`.  Returns true if the nonce is
    // fresh (AEAD auth may still reject it); returns false for replays and
    // nonces that fell off the back of the window.  Callers advance the
    // window themselves only after AEAD succeeds — see decrypt().
    bool window_accept(uint64_t nonce) const;
    // Commit an accepted, AEAD-verified nonce to the window.
    void window_commit(uint64_t nonce);

    uint8_t  key_[32] = {};
    uint64_t send_nonce_ = 0;      // next nonce we'll use on encrypt()
    uint64_t max_recv_nonce_ = 0;  // highest nonce whose AEAD verified
    // Bitmask covering the 64 nonces [max_recv_nonce_-63 .. max_recv_nonce_].
    // Bit 0 corresponds to max_recv_nonce_ itself; bit i to max_recv_nonce_-i.
    uint64_t recv_window_ = 0;
    bool     recv_started_ = false;
};

class HandshakeStateNK {
public:
    enum class Role { Initiator, Responder };

    // Size of each handshake message on the wire: ephemeral pubkey (32) +
    // encrypted payload + Poly1305 tag (16).  Min = 48 bytes (empty payload).
    // Max depends on payload; callers bound it themselves.
    static constexpr size_t HANDSHAKE_OVERHEAD = 32 + 16;

    // Initiator configuration: must supply the responder's public key
    // (obtained out-of-band — host identity).
    bool init_initiator(const uint8_t remote_static_pk[32]);
    // Responder configuration: must supply its own long-term keypair.
    bool init_responder(const KeyPair& local_static);

    // Build the next outbound handshake message.
    //   - Initiator calls this once to produce msg1.
    //   - Responder calls this after read_message(msg1) to produce msg2.
    // `payload` is application data piggy-backed on the handshake — used by
    // DeskBeam to carry HELLO_MAGIC / HELLO_ACK in the same round-trip.
    // `out` must have room for `payload_len + HANDSHAKE_OVERHEAD` bytes.
    // Returns wire length on success, 0 on error.
    size_t write_message(const uint8_t* payload, size_t payload_len,
                         uint8_t* out, size_t out_capacity);

    // Consume an inbound handshake message.
    //   - Responder calls this to consume msg1.
    //   - Initiator calls this to consume msg2.
    // On success, extracts the application payload into `payload_out`.
    // Returns payload length, or -1 on authentication / structure failure.
    // `payload_out_capacity` must be at least `wire_len - HANDSHAKE_OVERHEAD`.
    int read_message(const uint8_t* wire, size_t wire_len,
                     uint8_t* payload_out, size_t payload_out_capacity);

    // After initiator has processed msg2 (or responder has written msg2),
    // derive the two CipherStates.  Returns true on success.
    // `send_cs` encrypts traffic we send; `recv_cs` decrypts traffic we
    // receive.  The pairing is swapped between Initiator and Responder so
    // both ends agree on which key covers which direction.
    bool finalize(CipherState& send_cs, CipherState& recv_cs);

    // Variant that derives an auxiliary cipher pair in the same HKDF step.
    // Used to key DeskBeam's audio socket without running a second Noise
    // handshake: the main HKDF chain is extended with two extra output
    // blocks (T3/T4) so the audio cipher is cryptographically independent
    // from the video cipher but shares the same forward-secret material.
    // Nonces on each CipherState start fresh at 0, so the audio channel
    // must never cross-decrypt with the main channel.
    bool finalize(CipherState& main_send, CipherState& main_recv,
                  CipherState& aux_send,  CipherState& aux_recv);

    bool complete() const { return complete_; }

    HandshakeStateNK();
    ~HandshakeStateNK();
    HandshakeStateNK(const HandshakeStateNK&) = delete;
    HandshakeStateNK& operator=(const HandshakeStateNK&) = delete;

private:
    struct State;
    std::unique_ptr<State> s_;
    Role role_ = Role::Initiator;
    bool complete_ = false;
    // Which handshake step we expect next. NK has exactly two messages.
    int  step_ = 0;
};

} // namespace deskbeam::crypto
