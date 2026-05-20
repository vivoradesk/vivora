#pragma once

#include <cstdint>
#include <string>

namespace vivora::crypto {

// Trust-on-first-use pinning for rendezvous-resolved peer codes.
//
// The rendezvous server returns a pubkey alongside the reflexive endpoint
// in response to a LookupByCode.  Without a separate trust signal we'd
// have to take the server's word for that pubkey — a malicious / compromised
// rendezvous could MITM by handing the client a fake pubkey on first
// connect.  Pinning records the first pubkey seen for each code in a
// per-user file; subsequent lookups verify the pubkey hasn't changed.
//
// File format (plaintext, one entry per line, '#' starts a comment):
//   <code>  <64-hex-pubkey>
// Lines are matched case-sensitively on the code.

// Path the implementation reads/writes.  Same shape as default_host_key_path
// (per-user, platform-conventional location).  Exposed for diagnostics so
// the error path can tell the user where to edit.
std::string default_peer_pins_path();

enum class PinResult {
    NewlyPinned,    // First time this code was seen — pubkey persisted.
    Match,          // Stored pubkey matches the one we just received.
    Mismatch,       // Stored pubkey is DIFFERENT — possible MITM.
    IoError,        // Couldn't read or write the pin file.
};

// Look up `code` in the pin file.  If absent: append the new entry and
// return NewlyPinned.  If present and pubkey matches: return Match.
// If present and pubkey differs: return Mismatch (file is NOT modified —
// caller must decide whether to overwrite, delete the line, or refuse).
//
// `path` empty → default_peer_pins_path().
PinResult check_or_pin_peer(const std::string& code,
                            const uint8_t pubkey[32],
                            const std::string& path = "");

} // namespace vivora::crypto
