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

// Helper: emit the 4-byte (count + 3 reserved) + N×8 LAN tail used by
// both Register and LookupResponse.  Caller passes the count and array.
namespace {
size_t put_lan_tail(uint8_t* buf, uint8_t count, const LanCandidate lan[MAX_LAN_CANDIDATES]) {
    buf[0] = count;
    buf[1] = 0; buf[2] = 0; buf[3] = 0;   // reserved
    size_t off = 4;
    for (uint8_t i = 0; i < MAX_LAN_CANDIDATES; ++i) {
        put_u32_le(buf + off + 0, lan[i].ip);
        put_u16_le(buf + off + 4, lan[i].port);
        put_u16_le(buf + off + 6, 0);
        off += 8;
    }
    return off;
}

bool get_lan_tail(const uint8_t* buf, uint8_t& count, LanCandidate lan[MAX_LAN_CANDIDATES]) {
    count = buf[0];
    if (count > MAX_LAN_CANDIDATES) return false;
    size_t off = 4;
    for (uint8_t i = 0; i < MAX_LAN_CANDIDATES; ++i) {
        lan[i].ip       = get_u32_le(buf + off + 0);
        lan[i].port     = get_u16_le(buf + off + 4);
        lan[i].reserved = 0;
        off += 8;
    }
    return true;
}
} // namespace

size_t encode_register(uint8_t* buf, size_t buf_len, const RegisterPayload& p) {
    const size_t total = encode_header(buf, buf_len, MsgType::Register, sizeof(p));
    if (total == 0) return 0;
    std::memcpy(buf + HEADER_SIZE, p.pubkey, 32);
    put_lan_tail(buf + HEADER_SIZE + 32, p.lan_count, p.lan);
    return total;
}

size_t encode_register_ack(uint8_t* buf, size_t buf_len, const RegisterAckPayload& p) {
    // Bare 8 B if no relay; 8 + 4 + 2 + 32 = 46 B with relay.
    const size_t plen = (p.relay_ip != 0) ? 46u : 8u;
    const size_t total = encode_header(buf, buf_len, MsgType::RegisterAck, plen);
    if (total == 0) return 0;
    put_u32_le(buf + HEADER_SIZE + 0, p.reflexive_ip);
    put_u16_le(buf + HEADER_SIZE + 4, p.reflexive_port);
    put_u16_le(buf + HEADER_SIZE + 6, p.ttl_seconds);
    if (p.relay_ip != 0) {
        put_u32_le(buf + HEADER_SIZE +  8, p.relay_ip);
        put_u16_le(buf + HEADER_SIZE + 12, p.relay_port);
        std::memcpy(buf + HEADER_SIZE + 14, p.session_id, 32);
    }
    return total;
}

size_t encode_lookup(uint8_t* buf, size_t buf_len, const LookupPayload& p) {
    const size_t total = encode_header(buf, buf_len, MsgType::Lookup, sizeof(p));
    if (total == 0) return 0;
    std::memcpy(buf + HEADER_SIZE, p.pubkey, 32);
    return total;
}

size_t encode_lookup_resp(uint8_t* buf, size_t buf_len, const LookupResponsePayload& p) {
    // Legacy form (no relay): 40 + 32 = 72 bytes.
    // With relay tail: 72 + 4 + 2 + 32 = 110 bytes.
    constexpr size_t LEGACY_LEN = 40 + 8 * MAX_LAN_CANDIDATES;
    const size_t plen = (p.relay_ip != 0) ? LEGACY_LEN + 38 : LEGACY_LEN;
    const size_t total = encode_header(buf, buf_len, MsgType::LookupResponse, plen);
    if (total == 0) return 0;
    std::memcpy(buf + HEADER_SIZE, p.pubkey, 32);
    put_u32_le(buf + HEADER_SIZE + 32, p.host_ip);
    put_u16_le(buf + HEADER_SIZE + 36, p.host_port);
    buf[HEADER_SIZE + 38] = p.found;
    buf[HEADER_SIZE + 39] = p.lan_count;
    size_t off = HEADER_SIZE + 40;
    for (uint8_t i = 0; i < MAX_LAN_CANDIDATES; ++i) {
        put_u32_le(buf + off + 0, p.lan[i].ip);
        put_u16_le(buf + off + 4, p.lan[i].port);
        put_u16_le(buf + off + 6, 0);
        off += 8;
    }
    if (p.relay_ip != 0) {
        put_u32_le(buf + off +  0, p.relay_ip);
        put_u16_le(buf + off +  4, p.relay_port);
        std::memcpy(buf + off + 6, p.session_id, 32);
    }
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

size_t encode_lookup_code(uint8_t* buf, size_t buf_len, const LookupByCodePayload& p) {
    const size_t total = encode_header(buf, buf_len, MsgType::LookupByCode, sizeof(p));
    if (total == 0) return 0;
    std::memcpy(buf + HEADER_SIZE, p.code, sizeof(p.code));
    return total;
}

// Decoders -------------------------------------------------------------------

bool decode_register(const uint8_t* p, size_t len, RegisterPayload& out) {
    if (len != sizeof(out)) return false;
    std::memcpy(out.pubkey, p, 32);
    return get_lan_tail(p + 32, out.lan_count, out.lan);
}

bool decode_register_ack(const uint8_t* p, size_t len, RegisterAckPayload& out) {
    if (len != 8 && len != 46) return false;
    out.reflexive_ip   = get_u32_le(p + 0);
    out.reflexive_port = get_u16_le(p + 4);
    out.ttl_seconds    = get_u16_le(p + 6);
    out.relay_ip   = 0;
    out.relay_port = 0;
    std::memset(out.session_id, 0, 32);
    if (len == 46) {
        out.relay_ip   = get_u32_le(p +  8);
        out.relay_port = get_u16_le(p + 12);
        std::memcpy(out.session_id, p + 14, 32);
    }
    return true;
}

bool decode_lookup(const uint8_t* p, size_t len, LookupPayload& out) {
    if (len != 32) return false;
    std::memcpy(out.pubkey, p, 32);
    return true;
}

bool decode_lookup_resp(const uint8_t* p, size_t len, LookupResponsePayload& out) {
    constexpr size_t LEGACY_LEN = 40 + 8 * MAX_LAN_CANDIDATES;     // 72
    constexpr size_t WITH_RELAY = LEGACY_LEN + 38;                  // 110
    if (len != LEGACY_LEN && len != WITH_RELAY) return false;
    std::memcpy(out.pubkey, p, 32);
    out.host_ip   = get_u32_le(p + 32);
    out.host_port = get_u16_le(p + 36);
    out.found     = p[38];
    out.lan_count = p[39];
    if (out.lan_count > MAX_LAN_CANDIDATES) return false;
    size_t off = 40;
    for (uint8_t i = 0; i < MAX_LAN_CANDIDATES; ++i) {
        out.lan[i].ip       = get_u32_le(p + off + 0);
        out.lan[i].port     = get_u16_le(p + off + 4);
        out.lan[i].reserved = 0;
        off += 8;
    }
    out.relay_ip   = 0;
    out.relay_port = 0;
    std::memset(out.session_id, 0, 32);
    if (len == WITH_RELAY) {
        out.relay_ip   = get_u32_le(p + off +  0);
        out.relay_port = get_u16_le(p + off +  4);
        std::memcpy(out.session_id, p + off + 6, 32);
    }
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

bool decode_lookup_code(const uint8_t* p, size_t len, LookupByCodePayload& out) {
    if (len != sizeof(out.code)) return false;
    std::memcpy(out.code, p, sizeof(out.code));
    // Force terminator in case the sender forgot.
    out.code[sizeof(out.code) - 1] = '\0';
    return true;
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
