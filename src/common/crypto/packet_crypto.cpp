#include "common/crypto/packet_crypto.h"

#include <cstring>

namespace deskbeam::crypto {

static constexpr size_t HDR = protocol::PacketHeader::WIRE_SIZE;

size_t seal_packet(const uint8_t* wire, size_t wire_len,
                   CipherState& cs, uint8_t* out) {
    if (wire_len < HDR) return 0;
    const size_t plen = wire_len - HDR;
    const size_t new_plen = plen + CipherState::OVERHEAD;
    if (new_plen > 0xFFFF) return 0;

    // Copy 10-byte header verbatim, then rewrite payload_len (LE u16 at [8..10)).
    std::memcpy(out, wire, HDR);
    out[8] = static_cast<uint8_t>(new_plen & 0xFF);
    out[9] = static_cast<uint8_t>((new_plen >> 8) & 0xFF);

    const size_t written = cs.encrypt(wire + HDR, plen, out + HDR);
    if (written == 0) return 0;
    return HDR + written;
}

size_t open_packet(const uint8_t* wire, size_t wire_len,
                   CipherState& cs, uint8_t* out) {
    if (wire_len < HDR + CipherState::OVERHEAD) return 0;
    const size_t plen = wire_len - HDR;
    const size_t new_plen = plen - CipherState::OVERHEAD;

    std::memcpy(out, wire, HDR);
    out[8] = static_cast<uint8_t>(new_plen & 0xFF);
    out[9] = static_cast<uint8_t>((new_plen >> 8) & 0xFF);

    const size_t written = cs.decrypt(wire + HDR, plen, out + HDR);
    if (written == 0) return 0;
    return HDR + written;
}

} // namespace deskbeam::crypto
