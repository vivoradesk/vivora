// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "common/net/relay_protocol.h"

#include <cstring>

namespace vivora::net::relay {

namespace {
inline void put_u16_le(uint8_t* buf, uint16_t v) {
    buf[0] = static_cast<uint8_t>(v & 0xff);
    buf[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}
inline uint16_t get_u16_le(const uint8_t* buf) {
    return static_cast<uint16_t>(buf[0]) |
           (static_cast<uint16_t>(buf[1]) << 8);
}
} // namespace

size_t encode_header(uint8_t* buf, size_t buf_len, MsgType type, size_t payload_len) {
    if (buf_len < HEADER_SIZE + payload_len) return 0;
    if (payload_len > 0xffff)                return 0;
    std::memcpy(buf, MAGIC.data(), 4);
    buf[4] = VERSION;
    buf[5] = static_cast<uint8_t>(type);
    put_u16_le(buf + 6, static_cast<uint16_t>(payload_len));
    return HEADER_SIZE + payload_len;
}

bool parse_header(const uint8_t* buf, size_t len,
                  MsgType& type, size_t& payload_off, size_t& payload_len) {
    if (len < HEADER_SIZE)                            return false;
    if (std::memcmp(buf, MAGIC.data(), 4) != 0)       return false;
    if (buf[4] != VERSION)                            return false;
    const uint16_t plen = get_u16_le(buf + 6);
    if (HEADER_SIZE + plen > len)                     return false;
    type        = static_cast<MsgType>(buf[5]);
    payload_off = HEADER_SIZE;
    payload_len = plen;
    return true;
}

// --- encoders --------------------------------------------------------------

// Wire-format payload lengths — fixed regardless of struct padding.
constexpr size_t kBindWireBare    = 32;          // session_id only
constexpr size_t kBindWireLicense = 32 + 95;     // session_id + license token
constexpr size_t kBindAckWire     = 11;
constexpr size_t kKeepaliveWire   = 8;

size_t encode_bind(uint8_t* buf, size_t buf_len, const BindPayload& p) {
    const size_t plen = p.has_license ? kBindWireLicense : kBindWireBare;
    const size_t total = encode_header(buf, buf_len, MsgType::Bind, plen);
    if (total == 0) return 0;
    std::memcpy(buf + HEADER_SIZE, p.session_id, 32);
    if (p.has_license) {
        std::memcpy(buf + HEADER_SIZE + 32, p.license, 95);
    }
    return total;
}

size_t encode_bind_ack(uint8_t* buf, size_t buf_len, const BindAckPayload& p) {
    const size_t total = encode_header(buf, buf_len, MsgType::BindAck, kBindAckWire);
    if (total == 0) return 0;
    std::memcpy(buf + HEADER_SIZE, p.alloc_id, 8);
    buf[HEADER_SIZE + 8] = p.paired;
    put_u16_le(buf + HEADER_SIZE + 9, p.ttl_seconds);
    return total;
}

size_t encode_keepalive(uint8_t* buf, size_t buf_len, const KeepalivePayload& p) {
    const size_t total = encode_header(buf, buf_len, MsgType::Keepalive, kKeepaliveWire);
    if (total == 0) return 0;
    std::memcpy(buf + HEADER_SIZE, p.alloc_id, 8);
    return total;
}

size_t encode_data(uint8_t* buf, size_t buf_len,
                   const uint8_t alloc_id[8],
                   const uint8_t* data, size_t data_len) {
    const size_t payload_len = 8 + data_len;
    const size_t total = encode_header(buf, buf_len, MsgType::Data, payload_len);
    if (total == 0) return 0;
    std::memcpy(buf + HEADER_SIZE,     alloc_id, 8);
    std::memcpy(buf + HEADER_SIZE + 8, data,     data_len);
    return total;
}

// --- decoders --------------------------------------------------------------

bool decode_bind(const uint8_t* p, size_t len, BindPayload& out) {
    if (len == kBindWireBare) {
        std::memcpy(out.session_id, p, 32);
        out.has_license = false;
        return true;
    }
    if (len == kBindWireLicense) {
        std::memcpy(out.session_id, p, 32);
        std::memcpy(out.license, p + 32, 95);
        out.has_license = true;
        return true;
    }
    return false;
}

bool decode_bind_ack(const uint8_t* p, size_t len, BindAckPayload& out) {
    if (len != kBindAckWire) return false;
    std::memcpy(out.alloc_id, p, 8);
    out.paired      = p[8];
    out.ttl_seconds = get_u16_le(p + 9);
    return true;
}

bool decode_keepalive(const uint8_t* p, size_t len, KeepalivePayload& out) {
    if (len != kKeepaliveWire) return false;
    std::memcpy(out.alloc_id, p, 8);
    return true;
}

bool decode_data(const uint8_t* p, size_t len,
                 uint8_t alloc_id_out[8],
                 const uint8_t** data_out, size_t* data_len_out) {
    if (len < 8) return false;
    std::memcpy(alloc_id_out, p, 8);
    *data_out     = p + 8;
    *data_len_out = len - 8;
    return true;
}

} // namespace vivora::net::relay
