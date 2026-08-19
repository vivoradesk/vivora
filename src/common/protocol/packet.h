#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vivora::protocol {

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
    PerfReport     = 0x15,  // Client → host: sustainable framerate + decoder load
    HostStats      = 0x16,  // Host → client: current encoder target bitrate (kbps)
    MonitorListRequest = 0x17,  // Client → host: enumerate capturable displays (VIV-50)
    MonitorList        = 0x18,  // Host → client: list of displays (MonitorListMessage)
    SelectMonitor      = 0x19,  // Client → host: switch capture to display index
    Clipboard          = 0x1A,  // Both ways: fragmented ClipboardMessage (VIV-22)
    Disconnect         = 0x1B,  // Host → client: explicit terminal disconnect
                                // (sealed).  Payload is a single reason byte
                                // (DisconnectReason).  Suppresses the client's
                                // auto-reconnect — unlike a silent link drop,
                                // this says "do not come back" (VIV-52).
    CodecRenegotiate   = 0x1C,  // Client → host: the client could not decode the
                                // negotiated codec at runtime — renegotiate down
                                // (VIV-112).  Payload is a single VideoCodecCaps
                                // byte: the codecs the client can still decode.
                                // The host re-picks from the intersection and
                                // switches its live encoder.
};

// Reason carried in a Disconnect packet's single-byte payload.  Lets the
// viewer show a correct terminal status ("Connection declined" vs "Removed
// from your account") and, crucially, distinguish an intentional host-side
// teardown from a network blip so it does not auto-reconnect.  Unknown values
// are treated as a plain terminal disconnect (still no reconnect).
enum class DisconnectReason : uint8_t {
    Rejected      = 1,  // Host declined the approval prompt.
    Kicked        = 2,  // Device removed from the account / key revoked.
    HostShutdown  = 3,  // Host is shutting down.
    EncoderFailed = 4,  // Host has no working video encoder.  Terminal: the
                        // viewer must not auto-reconnect, because retrying
                        // hits the same missing encoder every time.
    IdleTimeout   = 5,  // No input for the host's idle window.  Also terminal:
                        // an auto-reconnect here would immediately restart the
                        // idle timer and undo the disconnect.
};

enum PacketFlags : uint8_t {
    FLAG_NONE       = 0x00,
    FLAG_KEYFRAME   = 0x01,
    FLAG_FEC        = 0x02,
    FLAG_FRAGMENT   = 0x04,  // packet is a fragment of a larger frame
    FLAG_LAST_FRAG  = 0x08,  // last fragment of a frame
    FLAG_RETX       = 0x10,  // packet is a NACK retransmission (set on wire by sender)
    FLAG_HEARTBEAT  = 0x20,  // host heartbeat re-encode of last frame; client
                             // must not count it toward adaptive-framerate
                             // reject/drop metrics or trigger an IDR cycle
                             // on decode failure (next heartbeat replaces).
    FLAG_FEC_RANGED = 0x40,  // FEC parity uses the ranged header (VIV-82):
                             // group data packets are a CONSECUTIVE key range
                             // [base_key .. base_key+K-1], so the parity carries
                             // only base_key (4B) instead of a K-long key+len
                             // list — lets K span a whole frame (pooled parity)
                             // without the parity packet exceeding the MTU.
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

} // namespace vivora::protocol
