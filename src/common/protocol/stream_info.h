#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace deskbeam::protocol {

// Real (pre-encoder-padding) frame dimensions advertised by the host.
// Encoders like QSV align frame size to 16 pixels, so a 1920x1080 stream
// becomes a 1920x1088 texture on the client.  The H.264 SPS carries the
// correct crop, but Media Foundation's decoder reports the padded size.
// The host therefore sends the real dimensions out-of-band on a Control
// channel packet so the client can trim padding from the renderer and
// use the right values for mouse-coordinate mapping.
//
// Sent on PacketType::StreamInfo; repeated on every keyframe so a fresh
// client catches the values quickly and a mid-session resolution change
// is picked up without needing a handshake round-trip.
//
// Wire format:
//   width(2B LE) | height(2B LE) = 4 bytes
struct StreamInfoMessage {
    uint16_t width  = 0;
    uint16_t height = 0;

    static constexpr size_t WIRE_SIZE = 4;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(WIRE_SIZE);
        buf[0] = static_cast<uint8_t>(width  & 0xFF);
        buf[1] = static_cast<uint8_t>((width  >> 8) & 0xFF);
        buf[2] = static_cast<uint8_t>(height & 0xFF);
        buf[3] = static_cast<uint8_t>((height >> 8) & 0xFF);
        return buf;
    }

    static bool deserialize(const uint8_t* data, size_t len, StreamInfoMessage& out) {
        if (len < WIRE_SIZE) return false;
        out.width  = static_cast<uint16_t>(data[0] | (data[1] << 8));
        out.height = static_cast<uint16_t>(data[2] | (data[3] << 8));
        return true;
    }
};

} // namespace deskbeam::protocol
