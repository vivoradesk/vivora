#include "common/net/rendezvous_protocol.h"

#include <cstring>

namespace deskbeam::net::rdv {

namespace {

// Little-endian uint16 helpers — keep wire layout deterministic across
// hosts even if the compiler decides to byte-pack differently.
inline void put_u16_le(uint8_t* buf, uint16_t v) {
    buf[0] = static_cast<uint8_t>(v & 0xff);
    buf[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}
inline uint16_t get_u16_le(const uint8_t* buf) {
    return static_cast<uint16_t>(buf[0]) |
           (static_cast<uint16_t>(buf[1]) << 8);
}
inline void put_u32_le(uint8_t* buf, uint32_t v) {
    buf[0] = static_cast<uint8_t>(v & 0xff);
    buf[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    buf[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    buf[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}
inline uint32_t get_u32_le(const uint8_t* buf) {
    return  static_cast<uint32_t>(buf[0])        |
           (static_cast<uint32_t>(buf[1]) <<  8) |
           (static_cast<uint32_t>(buf[2]) << 16) |
           (static_cast<uint32_t>(buf[3]) << 24);
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

// Encoders -------------------------------------------------------------------

size_t encode_register(uint8_t* buf, size_t buf_len, const RegisterPayload& p) {
    const size_t total = encode_header(buf, buf_len, MsgType::Register, sizeof(p));
    if (total == 0) return 0;
    std::memcpy(buf + HEADER_SIZE, p.pubkey, 32);
    return total;
}

size_t encode_register_ack(uint8_t* buf, size_t buf_len, const RegisterAckPayload& p) {
    const size_t total = encode_header(buf, buf_len, MsgType::RegisterAck, 8);
    if (total == 0) return 0;
    put_u32_le(buf + HEADER_SIZE + 0, p.reflexive_ip);
    put_u16_le(buf + HEADER_SIZE + 4, p.reflexive_port);
    put_u16_le(buf + HEADER_SIZE + 6, p.ttl_seconds);
    return total;
}

size_t encode_lookup(uint8_t* buf, size_t buf_len, const LookupPayload& p) {
    const size_t total = encode_header(buf, buf_len, MsgType::Lookup, sizeof(p));
    if (total == 0) return 0;
    std::memcpy(buf + HEADER_SIZE, p.pubkey, 32);
    return total;
}

size_t encode_lookup_resp(uint8_t* buf, size_t buf_len, const LookupResponsePayload& p) {
    const size_t total = encode_header(buf, buf_len, MsgType::LookupResponse, 40);
    if (total == 0) return 0;
    std::memcpy(buf + HEADER_SIZE, p.pubkey, 32);
    put_u32_le(buf + HEADER_SIZE + 32, p.host_ip);
    put_u16_le(buf + HEADER_SIZE + 36, p.host_port);
    buf[HEADER_SIZE + 38] = p.found;
    buf[HEADER_SIZE + 39] = 0;
    return total;
}

size_t encode_punch_hint(uint8_t* buf, size_t buf_len, const PunchHintPayload& p) {
    const size_t total = encode_header(buf, buf_len, MsgType::PunchHint, 8);
    if (total == 0) return 0;
    put_u32_le(buf + HEADER_SIZE + 0, p.client_ip);
    put_u16_le(buf + HEADER_SIZE + 4, p.client_port);
    put_u16_le(buf + HEADER_SIZE + 6, 0);
    return total;
}

size_t encode_keepalive(uint8_t* buf, size_t buf_len, const KeepalivePayload& p) {
    const size_t total = encode_header(buf, buf_len, MsgType::Keepalive, sizeof(p));
    if (total == 0) return 0;
    std::memcpy(buf + HEADER_SIZE, p.pubkey, 32);
    return total;
}

// Decoders -------------------------------------------------------------------

bool decode_register(const uint8_t* p, size_t len, RegisterPayload& out) {
    if (len != 32) return false;
    std::memcpy(out.pubkey, p, 32);
    return true;
}

bool decode_register_ack(const uint8_t* p, size_t len, RegisterAckPayload& out) {
    if (len != 8) return false;
    out.reflexive_ip   = get_u32_le(p + 0);
    out.reflexive_port = get_u16_le(p + 4);
    out.ttl_seconds    = get_u16_le(p + 6);
    return true;
}

bool decode_lookup(const uint8_t* p, size_t len, LookupPayload& out) {
    if (len != 32) return false;
    std::memcpy(out.pubkey, p, 32);
    return true;
}

bool decode_lookup_resp(const uint8_t* p, size_t len, LookupResponsePayload& out) {
    if (len != 40) return false;
    std::memcpy(out.pubkey, p, 32);
    out.host_ip   = get_u32_le(p + 32);
    out.host_port = get_u16_le(p + 36);
    out.found     = p[38];
    out.reserved  = p[39];
    return true;
}

bool decode_punch_hint(const uint8_t* p, size_t len, PunchHintPayload& out) {
    if (len != 8) return false;
    out.client_ip   = get_u32_le(p + 0);
    out.client_port = get_u16_le(p + 4);
    out.reserved    = get_u16_le(p + 6);
    return true;
}

bool decode_keepalive(const uint8_t* p, size_t len, KeepalivePayload& out) {
    return decode_register(p, len, out);
}

void pubkey_to_hex(const uint8_t pubkey[32], char out[65]) {
    static const char* HEX = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[i * 2 + 0] = HEX[(pubkey[i] >> 4) & 0xf];
        out[i * 2 + 1] = HEX[ pubkey[i]       & 0xf];
    }
    out[64] = '\0';
}

} // namespace deskbeam::net::rdv
