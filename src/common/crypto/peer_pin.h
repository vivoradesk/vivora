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
// This is the CLI behaviour (auto-TOFU on first contact).  The GUI uses
// query_peer_pin() + pin_peer() below so the user consents first (VIV-23).
//
// `path` empty → default_peer_pins_path().
PinResult check_or_pin_peer(const std::string& code,
                            const uint8_t pubkey[32],
                            const std::string& path = "");

// Read-only lookup (VIV-23).  Never modifies the file.  A missing or
// unreadable file simply means the code is Unknown — there is no IO
// error distinction on the read path (first run has no file at all).
// On Mismatch, `stored_hex_out` (when non-null) receives the pinned hex
// so the caller can show "was <old fp>, now <new fp>".
enum class PinQuery {
    Unknown,        // Code not present — first connect, ask the user.
    Match,          // Stored pubkey matches.
    Mismatch,       // Stored pubkey differs — possible MITM, ask the user.
};
PinQuery query_peer_pin(const std::string& code,
                        const uint8_t pubkey[32],
                        std::string* stored_hex_out = nullptr,
                        const std::string& path = "");

// Insert or REPLACE the pin for `code` (VIV-23 "Trust new key").  Rewrites
// the file in place, preserving comments and unrelated entries.  Returns
// false on IO failure.
bool pin_peer(const std::string& code,
              const uint8_t pubkey[32],
              const std::string& path = "");

// Remove every pin line whose code equals `code` OR whose pubkey equals
// `pubkey_hex` (either argument may be empty to skip that match).  Used by
// the address-book "Forget" action (VIV-23).  Returns false only on IO
// failure; removing nothing is success.
bool forget_peer_pin(const std::string& code,
                     const std::string& pubkey_hex,
                     const std::string& path = "");

} // namespace vivora::crypto
