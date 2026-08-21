// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "common/net/socket.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace vivora::net::rdv {

// Rendezvous wire protocol v1.  Lightweight UDP signalling layer that lives
// outside the encrypted media session — peers exchange reflexive endpoints
// through a public rendezvous server, then hole-punch directly.
//
// Header (8 bytes, all little-endian where applicable):
//   "DBRV"        4 B   magic (ASCII)
//   version       1 B   currently 1
//   msg_type      1 B   see MsgType
//   payload_len   2 B   little-endian uint16
// Followed by `payload_len` bytes of message body.
//
// All messages fit in a single UDP packet (<= 256 B).  No fragmentation,
// no FEC — signalling is fire-and-retry-with-timeout, not bulk transfer.

constexpr std::array<uint8_t, 4> MAGIC   = { 'D', 'B', 'R', 'V' };
constexpr uint8_t                VERSION = 1;
constexpr size_t                 HEADER_SIZE = 8;
constexpr size_t                 MAX_PACKET  = 256;
// Cap on the number of LAN candidate addresses peers can advertise.  Real
// machines rarely have more than two routable IPv4 interfaces; four gives
// headroom for VPN / docker bridge while keeping the wire payload small.
constexpr size_t                 MAX_LAN_CANDIDATES = 4;
// Length of the client-chosen anti-spoofing nonce carried in Lookup /
// LookupByCode and echoed verbatim in LookupResponse.  A client only
// accepts a response whose nonce matches the one it just sent, so an
// on-path attacker can't pre-forge a LookupResponse (it can't predict the
// nonce) — this hardens the TOFU pin against MITM (VIV-92).
constexpr size_t                 LOOKUP_NONCE_LEN = 8;

enum class MsgType : uint8_t {
    Register        = 0x01,  // host → server: claim a peer id
    RegisterAck     = 0x02,  // server → host: confirms registration + reflexive
    Lookup          = 0x03,  // client → server: ask for peer's reflexive
    LookupResponse  = 0x04,  // server → client: peer's reflexive (or not-found)
    PunchHint       = 0x05,  // server → host: a client is trying to reach you
    Keepalive       = 0x06,  // host → server: refresh registration TTL
    LookupByCode    = 0x07,  // client → server: ask by short memorable code
};

// Payload layouts — fixed-size, no length prefixes inside.

// Register: host announces its Curve25519 long-term public key plus a
// short list of LAN candidate endpoints.  Server stores `pubkey →
// (sender_ip, sender_port, lan_candidates)` — sender_ip/port comes from
// the UDP source address (more trustworthy than self-report; it's the
// actual reflexive binding the rendezvous saw).  LAN candidates are the
// host's own enumerated routable IPv4 + bound port, used by clients to
// short-circuit hairpin NAT when both peers turn out to be behind the
// same router.
struct LanCandidate {
    uint32_t ip;            // network byte order
    uint16_t port;          // host byte order
    uint16_t reserved;
};
static_assert(sizeof(LanCandidate) == 8, "LanCandidate must be packed");

struct RegisterPayload {
    uint8_t  pubkey[32];                                // host's Noise_NK static public key
    uint8_t  lan_count;                                 // 0..MAX_LAN_CANDIDATES
    uint8_t  reserved[3];
    LanCandidate lan[MAX_LAN_CANDIDATES];               // valid entries: [0..lan_count)
};
static_assert(sizeof(RegisterPayload) == 32 + 4 + 8 * MAX_LAN_CANDIDATES,
              "RegisterPayload must be packed");

// RegisterAck: tells the host what reflexive endpoint the rendezvous
// recorded.  Host can compare against its own STUN-discovered address and
// log a warning on mismatch (catches multi-NAT hairpinning weirdness).
//
// Optional relay assignment.  When the rendezvous server is configured
// with --relay-endpoint, every RegisterAck also carries the relay
// endpoint plus a server-minted 32-byte session_id that's specific to
// this host registration.  Both peers (host here, and the client when
// it later does Lookup) get the same session_id, so no manual
// --relay-session HEX64 coordination is needed any more.
//
// Wire shape:
//   relay_ip = 0 → bare 8-byte payload (legacy, no relay info)
//   relay_ip != 0 → 8 + 38 = 46 bytes total
struct RegisterAckPayload {
    uint32_t reflexive_ip;     // network byte order
    uint16_t reflexive_port;   // host byte order
    uint16_t ttl_seconds;      // when registration expires unless refreshed
    uint32_t relay_ip = 0;     // 0 means "no relay configured by this rdv"
    uint16_t relay_port = 0;
    uint8_t  session_id[32] = {};
};

// Lookup: client wants to reach the host identified by pubkey.
//   Wire shape:
//     32 bytes         → legacy (no nonce), nonce treated as absent
//     32 + 8 = 40 B    → pubkey + client nonce
// `has_nonce` is not on the wire — the decoder sets it from the length so
// the server knows whether to echo a nonce in its response.
struct LookupPayload {
    uint8_t  pubkey[32];
    uint8_t  nonce[LOOKUP_NONCE_LEN] = {};
    bool     has_nonce = false;
};

// LookupResponse: server returns the host's reflexive endpoint and the
// LAN candidates it advertised at registration time.
//   found = 0 → not registered (or registration expired); ip/port are 0.
//   found = 1 → ip/port carry the reflexive endpoint to punch toward.
//             Plus any LAN candidates the host advertised.
struct LookupResponsePayload {
    uint8_t  pubkey[32];
    uint32_t host_ip;
    uint16_t host_port;
    uint8_t  found;
    uint8_t  lan_count;                                 // 0..MAX_LAN_CANDIDATES
    LanCandidate lan[MAX_LAN_CANDIDATES];               // valid: [0..lan_count)
    // Optional relay info — same shape as RegisterAckPayload's tail.
    // relay_ip = 0 → wire payload is the legacy 72-byte form (no relay).
    uint32_t relay_ip = 0;
    uint16_t relay_port = 0;
    uint8_t  session_id[32] = {};
    // Anti-spoofing nonce echoed from the client's Lookup.  When has_nonce
    // is set an 8-byte nonce tail is appended after the (optional) relay
    // tail, giving wire lengths 72/80/110/118.  The server sets this iff the
    // client's request carried a nonce, so legacy clients still get 72/110.
    uint8_t  nonce[LOOKUP_NONCE_LEN] = {};
    bool     has_nonce = false;
};

// PunchHint: when a Lookup arrives, the rendezvous proactively tells the
// host the client's reflexive endpoint.  Both sides then start sending
// HELLO packets at each other simultaneously — typical cone NATs punch
// through within a few packets.
struct PunchHintPayload {
    uint32_t client_ip;
    uint16_t client_port;
    uint16_t reserved;
};
static_assert(sizeof(PunchHintPayload) == 8, "PunchHintPayload must be packed");

// Keepalive: identical body to Register, server treats it as a registration
// refresh.  Sent by the host every TTL/2 seconds.
using KeepalivePayload = RegisterPayload;

// LookupByCode: client wants to reach the host identified by a short
// memorable code (`<adjective>-<noun>-NNNN`, see common/utils/peer_code.h).
// Server resolves the code to a pubkey on its side and answers with the
// usual LookupResponse — the response carries the pubkey so the client
// can drive Noise_NK without needing to know it up front.
struct LookupByCodePayload {
    // Null-terminated ASCII, fits MAX_CODE_LEN.  Padded with NULs.
    char code[24];
    // Same anti-spoofing nonce as LookupPayload.  Wire shape:
    //   24 bytes       → legacy (no nonce)
    //   24 + 8 = 32 B  → code + client nonce
    uint8_t nonce[LOOKUP_NONCE_LEN] = {};
    bool    has_nonce = false;
};

// ---------------------------------------------------------------------------
// Encoding / decoding helpers.  All functions are header-only-friendly: no
// dynamic allocation, no streams, just plain memcpy into a caller-supplied
// buffer.  Return value is bytes written (encode) or true/false (decode).

// Build a packet header at `buf` and return total packet size on success.
// Returns 0 if `buf_len` can't hold the message.
size_t encode_header(uint8_t* buf, size_t buf_len, MsgType type, size_t payload_len);

// Parse a packet header.  On success writes the type + payload offset/len
// and returns true.  False on bad magic / bad version / truncated header.
bool   parse_header(const uint8_t* buf, size_t len,
                    MsgType& type, size_t& payload_off, size_t& payload_len);

// Convenience builders for each message type.  Caller provides a buffer
// of at least MAX_PACKET bytes; functions return the total packet length.
size_t encode_register     (uint8_t* buf, size_t buf_len, const RegisterPayload& p);
size_t encode_register_ack (uint8_t* buf, size_t buf_len, const RegisterAckPayload& p);
size_t encode_lookup       (uint8_t* buf, size_t buf_len, const LookupPayload& p);
size_t encode_lookup_resp  (uint8_t* buf, size_t buf_len, const LookupResponsePayload& p);
size_t encode_punch_hint   (uint8_t* buf, size_t buf_len, const PunchHintPayload& p);
size_t encode_keepalive    (uint8_t* buf, size_t buf_len, const KeepalivePayload& p);
size_t encode_lookup_code  (uint8_t* buf, size_t buf_len, const LookupByCodePayload& p);

// Decoders read from already-validated payload bytes (post-parse_header).
// Return false if payload size is wrong for the expected type.
bool decode_register     (const uint8_t* p, size_t len, RegisterPayload& out);
bool decode_register_ack (const uint8_t* p, size_t len, RegisterAckPayload& out);
bool decode_lookup       (const uint8_t* p, size_t len, LookupPayload& out);
bool decode_lookup_resp  (const uint8_t* p, size_t len, LookupResponsePayload& out);
bool decode_punch_hint   (const uint8_t* p, size_t len, PunchHintPayload& out);
bool decode_keepalive    (const uint8_t* p, size_t len, KeepalivePayload& out);
bool decode_lookup_code  (const uint8_t* p, size_t len, LookupByCodePayload& out);

// Hex-encode a 32-byte pubkey for log output.
void pubkey_to_hex(const uint8_t pubkey[32], char out[65]);

} // namespace vivora::net::rdv
