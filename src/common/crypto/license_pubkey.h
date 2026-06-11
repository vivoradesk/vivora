#pragma once

#include <cstdint>

namespace vivora::crypto {

// Vivora's license-signing **public** key (Ed25519, 32 bytes), embedded so
// the client can verify license tokens fully offline — no server round-trip
// in the hot path.  The matching private key (license.sk) lives only in the
// mint CLI / vivora-cloud and never ships.  This mirrors the bytes of
// `license.pk` at the repo root; being public, embedding it is harmless and
// patching it out of a fork buys nothing (the managed relay verifies with the
// same key server-side).
inline constexpr uint8_t LICENSE_PUBLIC_KEY[32] = {
    0xde, 0x1f, 0xc5, 0x7f, 0x83, 0xd4, 0x63, 0xd2,
    0x72, 0xea, 0xfc, 0x57, 0xbb, 0x66, 0xf7, 0x95,
    0xb0, 0xda, 0x25, 0x1a, 0xab, 0x31, 0xb1, 0x0a,
    0x44, 0xae, 0xb2, 0x5d, 0xf9, 0x4e, 0x35, 0x53,
};

} // namespace vivora::crypto
