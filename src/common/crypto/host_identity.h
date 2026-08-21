// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "common/crypto/noise_nk.h"

#include <string>

namespace vivora::crypto {

// Where the host's long-term Curve25519 keypair lives on disk.
//   Windows: %APPDATA%\Vivora\host_key
//   Linux:   $XDG_CONFIG_HOME/vivora/host_key  (or ~/.config/vivora/...)
//   macOS:   ~/Library/Application Support/Vivora/host_key
// Wire format is raw 64 bytes: secret_key[32] || public_key[32].  No header,
// no checksum — this is a private file, permissions (0600 on POSIX) are our
// integrity story.
std::string default_host_key_path();

// Generate-or-load.  Opens `path` (which may be empty to mean the default
// above).  If the file exists and is 64 bytes, loads it.  If it does not
// exist, generates a fresh Curve25519 keypair via the platform CSPRNG, writes
// it (creating parent dirs as needed), and returns it.  On POSIX the file is
// chmod'd 0600.
//
// Returns true on success.  The only contract the caller has is: after true,
// `out` contains a valid keypair — either recovered from disk or freshly
// minted.  Which is fine for Noise_NK's TOFU model.
bool load_or_create_host_identity(KeyPair& out, const std::string& path = {});

// Convenience: hex-encode a 32-byte public key as 64 lowercase hex chars.
// Used when the host logs its identity on startup so the operator can paste
// it into the client (`--host-key HEX`).
std::string hex_encode(const uint8_t* bytes, size_t len);

// Parse a 64-char hex string into exactly 32 raw bytes.  Returns false on any
// non-hex char or wrong length.  Used by the client's `--host-key HEX` CLI.
bool hex_decode_32(const std::string& hex, uint8_t out[32]);

// Canonical human-comparable key fingerprint (VIV-23): the first 16 hex
// chars of the pubkey, uppercased and grouped in fours — "6D2E 0C4A 7F3B
// 9E11".  Short enough to read over the phone, long enough (64 bits) that
// a targeted second-preimage is out of reach for a MITM in real time.
// Every UI surface (trust dialogs, Settings) MUST use this one helper so
// the two sides always compare like with like.
std::string key_fingerprint(const uint8_t pubkey[32]);
// Same, from a 64-char hex string (e.g. a pin-file entry).  Malformed or
// short input is grouped as-is after uppercasing — never fails, so a
// corrupt pin line still shows *something* comparable in the dialog.
std::string key_fingerprint_hex(const std::string& pubkey_hex);

} // namespace vivora::crypto
