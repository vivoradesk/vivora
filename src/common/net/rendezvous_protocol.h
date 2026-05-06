#pragma once

#include "common/net/socket.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace deskbeam::net::rdv {

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

enum class MsgType : uint8_t {
    Register        = 0x01,  // host → server: claim a peer id
    RegisterAck     = 0x02,  // server → host: confirms registration + reflexive
    Lookup          = 0x03,  // client → server: ask for peer's reflexive
    LookupResponse  = 0x04,  // server → client: peer's reflexive (or not-found)
    PunchHint       = 0x05,  // server → host: a client is trying to reach you
    Keepalive       = 0x06,  // host → server: refresh registration TTL
};

// Payload layouts — fixed-size, no length prefixes inside.

// Register: host announces its Curve25519 long-term public key.  Server
// stores `pubkey → (sender_ip, sender_port)` from the UDP source address
// (more trustworthy than what the host would self-report; it's the actual
// reflexive binding the rendezvous saw).
struct RegisterPayload {
    uint8_t  pubkey[32];   // host's Noise_NK static public key
};
static_assert(sizeof(RegisterPayload) == 32, "RegisterPayload must be packed");

// RegisterAck: tells the host what reflexive endpoint the rendezvous
// recorded.  Host can compare against its own STUN-discovered address and
// log a warning on mismatch (catches multi-NAT hairpinning weirdness).
struct RegisterAckPayload {
    uint32_t reflexive_ip;     // network byte order
    uint16_t reflexive_port;   // host byte order
    uint16_t ttl_seconds;      // when registration expires unless refreshed
};
static_assert(sizeof(RegisterAckPayload) == 8, "RegisterAckPayload must be packed");

// Lookup: client wants to reach the host identified by pubkey.
struct LookupPayload {
    uint8_t  pubkey[32];
};
static_assert(sizeof(LookupPayload) == 32, "LookupPayload must be packed");

// LookupResponse: server returns the host's reflexive endpoint.
//   found = 0 → not registered (or registration expired); ip/port are 0.
//   found = 1 → ip/port carry the reflexive endpoint to punch toward.
struct LookupResponsePayload {
    uint8_t  pubkey[32];
    uint32_t host_ip;
    uint16_t host_port;
    uint8_t  found;
    uint8_t  reserved;
};
static_assert(sizeof(LookupResponsePayload) == 40, "LookupResponsePayload must be packed");

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

// Decoders read from already-validated payload bytes (post-parse_header).
// Return false if payload size is wrong for the expected type.
bool decode_register     (const uint8_t* p, size_t len, RegisterPayload& out);
bool decode_register_ack (const uint8_t* p, size_t len, RegisterAckPayload& out);
bool decode_lookup       (const uint8_t* p, size_t len, LookupPayload& out);
bool decode_lookup_resp  (const uint8_t* p, size_t len, LookupResponsePayload& out);
bool decode_punch_hint   (const uint8_t* p, size_t len, PunchHintPayload& out);
bool decode_keepalive    (const uint8_t* p, size_t len, KeepalivePayload& out);

// Hex-encode a 32-byte pubkey for log output.
void pubkey_to_hex(const uint8_t pubkey[32], char out[65]);

} // namespace deskbeam::net::rdv
