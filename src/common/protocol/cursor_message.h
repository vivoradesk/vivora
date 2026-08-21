// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace vivora::protocol {

// Cursor shape: a BGRA bitmap + hotspot. Sent on a Control-channel
// PacketType::CursorShape whenever the host cursor's shape changes.
// Host sends the same shape 2-3 times on change to survive UDP loss
// (the stream is unreliable, no ACK loop).
//
// Wire format:
//   shape_id(4B LE) | w(2B LE) | h(2B LE) | hx(2B LE) | hy(2B LE) | bgra[w*h*4]
struct CursorShapeMessage {
    uint32_t shape_id = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t hotspot_x = 0;
    uint16_t hotspot_y = 0;
    std::vector<uint8_t> bgra;  // w * h * 4 bytes

    static constexpr size_t HEADER_SIZE = 12;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(HEADER_SIZE + bgra.size());
        uint8_t* p = buf.data();
        std::memcpy(p, &shape_id, 4);  p += 4;
        std::memcpy(p, &width,    2);  p += 2;
        std::memcpy(p, &height,   2);  p += 2;
        std::memcpy(p, &hotspot_x,2);  p += 2;
        std::memcpy(p, &hotspot_y,2);  p += 2;
        if (!bgra.empty()) std::memcpy(p, bgra.data(), bgra.size());
        return buf;
    }

    static bool deserialize(const uint8_t* data, size_t len, CursorShapeMessage& out) {
        if (len < HEADER_SIZE) return false;
        std::memcpy(&out.shape_id, data,      4);
        std::memcpy(&out.width,    data + 4,  2);
        std::memcpy(&out.height,   data + 6,  2);
        std::memcpy(&out.hotspot_x,data + 8,  2);
        std::memcpy(&out.hotspot_y,data + 10, 2);
        size_t pixels = static_cast<size_t>(out.width) * out.height * 4u;
        if (len < HEADER_SIZE + pixels) return false;
        out.bgra.assign(data + HEADER_SIZE, data + HEADER_SIZE + pixels);
        return true;
    }
};

// Cursor position: sent per-frame on PacketType::CursorPosition (unreliable).
// Position is normalized 0..1 against host screen so client doesn't need to
// know host resolution. shape_id references the last CursorShape the client
// should use; if the client hasn't received that shape yet it falls back to
// hiding the overlay for this frame.
//
// Wire format:
//   x_norm(4B float LE) | y_norm(4B float LE) | visible(1B) | shape_id(4B LE) = 13B
struct CursorPositionMessage {
    float x_norm = 0.0f;
    float y_norm = 0.0f;
    bool visible = false;
    uint32_t shape_id = 0;

    static constexpr size_t WIRE_SIZE = 13;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(WIRE_SIZE);
        std::memcpy(buf.data(),     &x_norm,   4);
        std::memcpy(buf.data() + 4, &y_norm,   4);
        buf[8] = visible ? 1 : 0;
        std::memcpy(buf.data() + 9, &shape_id, 4);
        return buf;
    }

    static bool deserialize(const uint8_t* data, size_t len, CursorPositionMessage& out) {
        if (len < WIRE_SIZE) return false;
        std::memcpy(&out.x_norm,   data,     4);
        std::memcpy(&out.y_norm,   data + 4, 4);
        out.visible = data[8] != 0;
        std::memcpy(&out.shape_id, data + 9, 4);
        return true;
    }
};

} // namespace vivora::protocol
