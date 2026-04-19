#pragma once

#include <cstdint>
#include <vector>

namespace deskbeam::protocol {

enum class PacketType : uint8_t {
    Video       = 0x01,
    Audio       = 0x02,
    Input       = 0x03,
    Control     = 0x04,
    IdrRequest  = 0x05,  // Client requests IDR from host
    NackRequest = 0x06,  // Client requests retransmit of lost fragments
    FecReport   = 0x07,  // Client reports packet loss rate for adaptive FEC
    BwProbe     = 0x08,  // Host → client: bandwidth probe burst packet
    BwProbeAck  = 0x09,  // Client → host: measured bandwidth from probe
    Ping        = 0x10,
    Pong        = 0x11,
    CursorShape    = 0x12,  // Host → client: cursor bitmap + hotspot (sent on shape change)
    CursorPosition = 0x13,  // Host → client: cursor x/y/visible + shape_id (per-frame)
    StreamInfo     = 0x14,  // Host → client: real (cropped) frame dimensions
};

enum PacketFlags : uint8_t {
    FLAG_NONE       = 0x00,
    FLAG_KEYFRAME   = 0x01,
    FLAG_FEC        = 0x02,
    FLAG_FRAGMENT   = 0x04,  // packet is a fragment of a larger frame
    FLAG_LAST_FRAG  = 0x08,  // last fragment of a frame
    FLAG_RETX       = 0x10,  // packet is a NACK retransmission (set on wire by sender)
};

// Wire format: 10 bytes header
// Type(1) | SeqNo(2) | Timestamp(4) | Flags(1) | PayloadLen(2) | Payload...
struct PacketHeader {
    PacketType type;
    uint16_t seq_no;
    uint32_t timestamp;  // microseconds, wrapping
    uint8_t flags;
    uint16_t payload_len;

    static constexpr size_t WIRE_SIZE = 10;

    // Serialize header to buffer (must be at least WIRE_SIZE bytes)
    void serialize(uint8_t* buf) const;

    // Deserialize header from buffer
    static PacketHeader deserialize(const uint8_t* buf);
};

struct Packet {
    PacketHeader header;
    std::vector<uint8_t> payload;

    // Serialize entire packet (header + payload) into buffer
    std::vector<uint8_t> serialize() const;

    // Deserialize from raw bytes
    static Packet deserialize(const uint8_t* data, size_t len);
};

} // namespace deskbeam::protocol
