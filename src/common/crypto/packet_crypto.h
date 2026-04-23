#pragma once

#include "common/crypto/noise_nk.h"
#include "common/protocol/packet.h"

#include <cstddef>
#include <cstdint>

namespace deskbeam::crypto {

// Transport AEAD helpers built on top of CipherState.  They operate on the
// raw wire form produced by Packet::serialize() — a 10-byte header followed
// by `header.payload_len` payload bytes.
//
// Rationale: we encrypt ONLY the payload, not the header.  Three reasons:
//   1. FEC operates on wire bytes (header + payload) and must see a
//      consistent payload shape on both peers; keeping the header plaintext
//      lets the FEC decoder route parity vs. data wires without a key.
//   2. Per-destination sealing for multi-client is cheap — same payload
//      bytes, different cipher, different nonce.
//   3. The 10-byte header contains only seq_no / flags / type — no secrets.
//      An attacker could flip bits but the AEAD tag over the payload will
//      reject any such tampered packet on the recv side.
//
// Wire layout after seal_packet():
//   [ header(10, payload_len adjusted) ][ 8B nonce ][ ciphertext ][ 16B tag ]
// open_packet() is the exact inverse.

// Seal a plaintext wire packet for transmission.
//   `wire`   : plaintext wire bytes (10B header + payload).
//   `cs`     : outbound CipherState from the Noise handshake.
//   `out`    : destination buffer; must be at least wire_len + OVERHEAD bytes.
// Returns the total sealed wire length, or 0 on error (nonce exhaustion,
// underlength input, payload_len overflow).
size_t seal_packet(const uint8_t* wire, size_t wire_len,
                   CipherState& cs, uint8_t* out);

// Open a sealed wire packet.  Inverse of seal_packet().
//   `wire`   : sealed wire bytes.
//   `cs`     : inbound CipherState from the Noise handshake.
//   `out`    : destination buffer; must be at least wire_len bytes.
// Returns the total plaintext wire length, or 0 on any failure (bad auth
// tag, replay, underlength input).
size_t open_packet(const uint8_t* wire, size_t wire_len,
                   CipherState& cs, uint8_t* out);

} // namespace deskbeam::crypto
