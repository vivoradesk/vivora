// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace vivora::crypto {

// Fill `out` with `len` bytes from the platform CSPRNG.
//   Windows: BCryptGenRandom(BCRYPT_USE_SYSTEM_PREFERRED_RNG)
//   Linux:   getrandom(GRND_NONBLOCK) with /dev/urandom fallback
//   macOS:   getentropy()
// Returns true on success. On failure, `out` is not guaranteed to be
// untouched — treat any false return as fatal for the caller.
//
// Cryptographic requirement: every handshake (ephemeral key) and static
// host key calls this. A non-CSPRNG source would undermine the entire
// Noise session.  Never replace with rand()/std::mt19937.
bool random_bytes(uint8_t* out, size_t len);

} // namespace vivora::crypto
