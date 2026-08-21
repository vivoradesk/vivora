// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace vivora::protocol {

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
//   width(2B LE) | height(2B LE) | target_fps(2B LE) = 6 bytes
// target_fps is the host's currently APPLIED framerate target — the user
// cap (VIV-67) after adaptive clamping — so the client HUD can show the
// effective rate instead of its own optimistic PerfReport request.  The
// field was appended later: decoders accept the legacy 4-byte form and
// leave target_fps at 0 (= unknown, HUD falls back to the local request).
struct StreamInfoMessage {
    uint16_t width      = 0;
    uint16_t height     = 0;
    uint16_t target_fps = 0;

    static constexpr size_t WIRE_SIZE        = 6;
    static constexpr size_t WIRE_SIZE_LEGACY = 4;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(WIRE_SIZE);
        buf[0] = static_cast<uint8_t>(width  & 0xFF);
        buf[1] = static_cast<uint8_t>((width  >> 8) & 0xFF);
        buf[2] = static_cast<uint8_t>(height & 0xFF);
        buf[3] = static_cast<uint8_t>((height >> 8) & 0xFF);
        buf[4] = static_cast<uint8_t>(target_fps & 0xFF);
        buf[5] = static_cast<uint8_t>((target_fps >> 8) & 0xFF);
        return buf;
    }

    static bool deserialize(const uint8_t* data, size_t len, StreamInfoMessage& out) {
        if (len < WIRE_SIZE_LEGACY) return false;
        out.width  = static_cast<uint16_t>(data[0] | (data[1] << 8));
        out.height = static_cast<uint16_t>(data[2] | (data[3] << 8));
        out.target_fps = len >= WIRE_SIZE
            ? static_cast<uint16_t>(data[4] | (data[5] << 8))
            : 0;
        return true;
    }
};

} // namespace vivora::protocol
