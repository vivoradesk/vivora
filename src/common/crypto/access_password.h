// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vivora::crypto {

// Unattended-access password (VIV-151).
//
// The password never travels.  Both sides stretch it with Argon2id into a
// 32-byte key, salted with the host's public key, and the viewer runs the
// IKpsk1 handshake with that key (noise_nk.h).  A wrong password fails the
// AEAD tag on msg1's payload, so every guess costs the attacker a full
// handshake with the host, which rate-limits it (access_throttle.h).  An
// eavesdropper learns nothing to test guesses against offline: the payload
// key also depends on DH secrets only the two peers hold.
//
// The host stores the derived key, not the password.  The file sits next to
// the host key and is exactly as sensitive: whoever can read one can read
// the other.

constexpr size_t ACCESS_PSK_SIZE = 32;
// Shorter passwords are refused when one is set.  Argon2 and the throttle
// make online guessing slow, not impossible.
constexpr size_t MIN_ACCESS_PASSWORD_LEN = 8;

// Argon2id(password, salt = "vivora-access-v1" || host_pk), 64 MiB, 3 passes.
// Takes a few hundred milliseconds by design; call it off the UI thread.
// Returns false only when the 64 MiB work area cannot be allocated.
bool derive_access_psk(const std::string& password, const uint8_t host_pk[32],
                       uint8_t out[ACCESS_PSK_SIZE]);

// "access_psk" in the same directory as `host_key_path` (empty = the
// per-user default from host_identity.h).
std::string access_psk_path_for(const std::string& host_key_path = {});

// File layout: "VAP1" | host_pk(32) | psk(32).  The stored host key ties the
// password to one identity, so a regenerated host key makes the old password
// stop working instead of silently pairing with the wrong key.
//
// load: false when the file is missing, malformed, or for another host key.
bool load_access_psk(const std::string& path, const uint8_t host_pk[32],
                     uint8_t out[ACCESS_PSK_SIZE]);
// save: writes atomically enough for a settings change (temp + rename) and
// restricts the file to its owner on POSIX.
bool save_access_psk(const std::string& path, const uint8_t host_pk[32],
                     const uint8_t psk[ACCESS_PSK_SIZE]);
// clear: removes the file.  True when it is gone afterwards.
bool clear_access_psk(const std::string& path);

} // namespace vivora::crypto
