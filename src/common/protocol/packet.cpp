// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "common/protocol/packet.h"
#include <cstring>
#include <stdexcept>

namespace vivora::protocol {

// Little-endian helpers
static void write_u16(uint8_t* buf, uint16_t val) {
    buf[0] = static_cast<uint8_t>(val & 0xFF);
    buf[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

static void write_u32(uint8_t* buf, uint32_t val) {
    buf[0] = static_cast<uint8_t>(val & 0xFF);
    buf[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    buf[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
    buf[3] = static_cast<uint8_t>((val >> 24) & 0xFF);
}

static uint16_t read_u16(const uint8_t* buf) {
    return static_cast<uint16_t>(buf[0]) |
           (static_cast<uint16_t>(buf[1]) << 8);
}

static uint32_t read_u32(const uint8_t* buf) {
    return static_cast<uint32_t>(buf[0]) |
           (static_cast<uint32_t>(buf[1]) << 8) |
           (static_cast<uint32_t>(buf[2]) << 16) |
           (static_cast<uint32_t>(buf[3]) << 24);
}

void PacketHeader::serialize(uint8_t* buf) const {
    buf[0] = static_cast<uint8_t>(type);
    write_u16(buf + 1, seq_no);
    write_u32(buf + 3, timestamp);
    buf[7] = flags;
    write_u16(buf + 8, payload_len);
}

PacketHeader PacketHeader::deserialize(const uint8_t* buf) {
    PacketHeader h;
    h.type = static_cast<PacketType>(buf[0]);
    h.seq_no = read_u16(buf + 1);
    h.timestamp = read_u32(buf + 3);
    h.flags = buf[7];
    h.payload_len = read_u16(buf + 8);
    return h;
}

std::vector<uint8_t> Packet::serialize() const {
    std::vector<uint8_t> buf(PacketHeader::WIRE_SIZE + payload.size());
    header.serialize(buf.data());
    if (!payload.empty()) {
        std::memcpy(buf.data() + PacketHeader::WIRE_SIZE, payload.data(), payload.size());
    }
    return buf;
}

Packet Packet::deserialize(const uint8_t* data, size_t len) {
    if (len < PacketHeader::WIRE_SIZE) {
        throw std::runtime_error("Packet too short for header");
    }

    Packet pkt;
    pkt.header = PacketHeader::deserialize(data);

    size_t payload_bytes = len - PacketHeader::WIRE_SIZE;
    if (payload_bytes > 0) {
        pkt.payload.assign(data + PacketHeader::WIRE_SIZE,
                           data + PacketHeader::WIRE_SIZE + payload_bytes);
    }
    return pkt;
}

} // namespace vivora::protocol
