// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <cstddef>

namespace vivora::crypto {

// Compact binary license token, signed by the Vivora private key and
// verified offline by anyone holding the matching public key (the
// managed relay, eventually the address-book sync API).  We use a
// purpose-built layout instead of JWT — no JSON parser, no base64,
// fixed 95 bytes on the wire, signature verifies in microseconds.
//
// Layout (95 bytes total):
//   magic[4]      = "DBLT"
//   version[1]    = 1
//   reserved[1]   = 0
//   tier[1]       = 0=trial, 1=pro
//   exp[8]        = unix seconds, big-endian
//   subject[16]   = arbitrary id (account UUID, hash, etc.)
//   signature[64] = Ed25519 over bytes [0..30] (everything except sig)
//
// The relay uses verify() once per BIND; clients store the raw 95 bytes
// in a file and ship them verbatim to the server.

constexpr size_t LICENSE_TOKEN_SIZE = 95;
constexpr size_t LICENSE_SIGNED_SIZE = 31;        // bytes covered by Ed25519
constexpr uint8_t LICENSE_VERSION = 1;

enum class LicenseTier : uint8_t {
    Trial = 0,
    Pro   = 1,
};

struct LicenseClaims {
    LicenseTier tier = LicenseTier::Trial;
    int64_t     exp_unix = 0;
    uint8_t     subject[16] = {};
};

// Sign a token.  `secret_key[64]` is monocypher's expanded eddsa secret
// (from crypto_eddsa_key_pair).  Returns true on success; false only if
// `out_token` is null.
bool sign_license(const LicenseClaims& claims,
                  const uint8_t secret_key[64],
                  uint8_t out_token[LICENSE_TOKEN_SIZE]);

// Verify a token against `public_key`.  On success populates `out_claims`
// and returns true.  Returns false on bad magic, version mismatch, bad
// signature, or expiry already past `now_unix`.
bool verify_license(const uint8_t token[LICENSE_TOKEN_SIZE],
                    const uint8_t public_key[32],
                    int64_t now_unix,
                    LicenseClaims& out_claims);

} // namespace vivora::crypto
