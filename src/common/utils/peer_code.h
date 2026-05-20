#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace vivora::peer_code {

// Maximum length of a peer code, including the trailing null.  Layout is
// "adjective-noun-NNNN\0" where each word is up to 7 chars, so the worst
// case is 7+1+7+1+4+1 = 21 bytes.  Round to 24 for safe wire alignment.
constexpr size_t MAX_CODE_LEN = 24;

// Deterministic memorable code derived from a 32-byte Curve25519 pubkey.
// Format: "<adjective>-<noun>-NNNN" where the number is a 4-digit decimal
// zero-padded.  ~26 bits of effective entropy (128 adjectives × 128 nouns
// × 10000 numbers ≈ 164M codes) — collisions are detected on the server
// side and logged.
//
// `out` must point to at least MAX_CODE_LEN bytes.  Returns the code length
// (excluding the null terminator).
size_t encode(const uint8_t pubkey[32], char out[MAX_CODE_LEN]);

// Convenience wrapper around encode() returning a std::string.
std::string encode(const uint8_t pubkey[32]);

// Validate the code shape: <word>-<word>-NNNN, words from our lists, NNNN
// is exactly 4 ASCII digits.  Cheap input filter for the rendezvous server
// before doing the map lookup.  Doesn't touch the pubkey map.
bool is_well_formed(const char* s);

// True if `s` looks like a 64-char lowercase hex pubkey.  Used by the CLI
// layer to auto-detect whether --peer wants a hex pin or a code lookup.
bool looks_like_hex_pubkey(const char* s);

} // namespace vivora::peer_code
