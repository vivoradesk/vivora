#pragma once

#include "common/net/socket.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace deskbeam::net::relay {

// Relay wire protocol v1.  Lightweight UDP forwarding layer for the case
// where direct hole-punching can't establish a peer-to-peer link
// (symmetric NAT, CGNAT, blocking firewalls).  Both peers BIND to a
// shared relay; once paired, every DATA packet from one side gets
// forwarded to the other.  All session-level encryption (Noise_NK)
// happens above this layer — the relay only sees opaque payload bytes.
//
// Header (8 bytes, little-endian where applicable):
//   "DBRL"        4 B   magic
//   version       1 B   currently 1
//   msg_type      1 B   see MsgType
//   payload_len   2 B   little-endian uint16
//
// Payloads come in fixed-size flavours for control messages (Bind,
// BindAck, Keepalive) and variable-length for Data.  Total packet stays
// inside one UDP datagram (cap at 1400 B in practice — leave room for the
// 16-byte relay overhead on top of the underlying media MTU).

constexpr std::array<uint8_t, 4> MAGIC   = { 'D', 'B', 'R', 'L' };
constexpr uint8_t                VERSION = 1;
constexpr size_t                 HEADER_SIZE = 8;
// Cap for control messages.  Data payloads can be up to MTU-ish.
constexpr size_t                 MAX_CONTROL_PACKET = 256;
// Generous cap for forwarded data — a fragment + room for the relay
// header.  Real fragments come in around 1280 B.
constexpr size_t                 MAX_DATA_PACKET    = 1500;

enum class MsgType : uint8_t {
    Bind      = 0x01,  // peer → relay: claim a pairing slot
    BindAck   = 0x02,  // relay → peer: returns alloc_id + pairing status
    Data      = 0x03,  // peer ↔ relay: opaque bytes to forward to the paired peer
    Keepalive = 0x04,  // peer → relay: refresh TTL on an existing binding
};

// Bind: peer announces itself + the peer it wants to talk to.  The relay
// records (sender_endpoint, my_pubkey, peer_pubkey) and looks for the
// mirror binding (peer_pubkey == other.my_pubkey AND my_pubkey ==
// other.peer_pubkey).  When both halves arrive the relay links them.
// Wire layout for these structs is fixed by the encode_/decode_ helpers
// below; the struct shape is just a convenience for callers.  No static
// asserts on sizeof — padding from short fields would trip them and the
// raw layout never depends on it.
struct BindPayload {
    uint8_t my_pubkey[32];
    uint8_t peer_pubkey[32];
};

// BindAck: returns the relay-assigned 8-byte allocation id.  All future
// DATA / KEEPALIVE packets reference this id instead of pubkeys to keep
// per-packet overhead tiny.  `paired = 1` if the other peer was already
// bound at the time we replied — else 0 (peer will discover the pairing
// when its own BIND lands; the relay does NOT proactively notify it).
struct BindAckPayload {
    uint8_t  alloc_id[8];
    uint8_t  paired;
    uint16_t ttl_seconds;
};

// Keepalive: refreshes the binding's TTL.  Sent by peers every TTL/2.
struct KeepalivePayload {
    uint8_t alloc_id[8];
};

// ---------------------------------------------------------------------------
// Encoding / decoding helpers — same shape as rendezvous_protocol: small,
// allocation-free, caller supplies the buffer.

size_t encode_header(uint8_t* buf, size_t buf_len, MsgType type, size_t payload_len);
bool   parse_header(const uint8_t* buf, size_t len,
                    MsgType& type, size_t& payload_off, size_t& payload_len);

size_t encode_bind     (uint8_t* buf, size_t buf_len, const BindPayload& p);
size_t encode_bind_ack (uint8_t* buf, size_t buf_len, const BindAckPayload& p);
size_t encode_keepalive(uint8_t* buf, size_t buf_len, const KeepalivePayload& p);
// Data: writes [DBRL header | alloc_id[8] | data[data_len]] into buf.
// Returns total wire length on success, 0 if buf can't hold it.
size_t encode_data     (uint8_t* buf, size_t buf_len,
                        const uint8_t alloc_id[8],
                        const uint8_t* data, size_t data_len);

bool decode_bind     (const uint8_t* p, size_t len, BindPayload& out);
bool decode_bind_ack (const uint8_t* p, size_t len, BindAckPayload& out);
bool decode_keepalive(const uint8_t* p, size_t len, KeepalivePayload& out);
// Data decode: returns false if payload is < 8 bytes (no alloc_id).
// On success, *data_out points into the original buffer (no copy) and
// *data_len_out is the forwarded payload length.
bool decode_data     (const uint8_t* p, size_t len,
                      uint8_t alloc_id_out[8],
                      const uint8_t** data_out, size_t* data_len_out);

} // namespace deskbeam::net::relay
