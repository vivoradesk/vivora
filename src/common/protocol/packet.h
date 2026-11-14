// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

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
                                // Host → client: the codec the live encoder now
                                // produces (VIV-147).  Payload is a single
                                // VideoCodec byte, same encoding as the
                                // HELLO_ACK codec byte.  Sent to every client
                                // whenever the wire codec changes mid-session
                                // (negotiation, or a monitor switch promoting
                                // H.264 to HEVC on an HDR display), and again
                                // in answer to each IdrRequest so a lost
                                // announcement heals.  Clients older than this
                                // ignore the unknown direction.
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
    // The next two never travel in a Disconnect packet.  The viewer sets them
    // itself from an AccessReply (below), so the status text and the
    // no-reconnect latch work the same way as for the reasons above.
    WrongPassword = 6,  // The unattended-access password was wrong.
    PasswordLocked = 7, // Too many wrong passwords; the host refuses more for now.
};

// Host → viewer answer to a password handshake that failed (VIV-151).  It
// rides a Control packet in place of Noise msg2, unencrypted, because no keys
// exist yet:
//     "VVAU" | code(1) | retry_after_s(2, LE)
// Being unauthenticated, it is only a hint for the viewer's status text.  A
// forged one can make a viewer stop early, which anyone able to inject
// packets on the path could do anyway by dropping msg2.  It is 7 bytes, so it
// can never be mistaken for a msg2 (48 bytes or more).
enum class AccessReplyCode : uint8_t {
    WrongPassword = 1,
    LockedOut     = 2,
};

struct AccessReply {
    static constexpr size_t WIRE_SIZE = 7;
    AccessReplyCode code = AccessReplyCode::WrongPassword;
    uint16_t retry_after_s = 0;   // LockedOut only; 0 = unknown

    void serialize(uint8_t out[WIRE_SIZE]) const {
        out[0] = 'V'; out[1] = 'V'; out[2] = 'A'; out[3] = 'U';
        out[4] = static_cast<uint8_t>(code);
        out[5] = static_cast<uint8_t>(retry_after_s & 0xFF);
        out[6] = static_cast<uint8_t>(retry_after_s >> 8);
    }
    static bool parse(const uint8_t* p, size_t len, AccessReply& out) {
        if (len != WIRE_SIZE || p[0] != 'V' || p[1] != 'V' || p[2] != 'A' || p[3] != 'U')
            return false;
        if (p[4] != 1 && p[4] != 2) return false;
        out.code = static_cast<AccessReplyCode>(p[4]);
        out.retry_after_s = static_cast<uint16_t>(p[5] | (p[6] << 8));
        return true;
    }
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
