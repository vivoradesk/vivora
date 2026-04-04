#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace deskbeam::protocol {

// Input event types
enum class InputEventType : uint8_t {
    MouseMove    = 0x01,
    MouseButton  = 0x02,
    MouseScroll  = 0x03,
    KeyDown      = 0x04,
    KeyUp        = 0x05,
};

enum class MouseButton : uint8_t {
    Left   = 0x01,
    Right  = 0x02,
    Middle = 0x03,
    X1     = 0x04,
    X2     = 0x05,
};

// Wire format for input events:
//
// MouseMove:   type(1) | x_norm(4 float) | y_norm(4 float)              = 9 bytes
//              x_norm, y_norm: 0.0..1.0 relative to host screen
//
// MouseButton: type(1) | button(1) | pressed(1)                         = 3 bytes
//
// MouseScroll: type(1) | dx(2 int16) | dy(2 int16)                      = 5 bytes
//              dy > 0 = scroll up, dy < 0 = scroll down
//
// KeyDown/Up:  type(1) | scan_code(2 uint16) | vk_code(2 uint16)        = 5 bytes

struct InputEvent {
    InputEventType type;

    // MouseMove
    float x_norm = 0.0f;  // 0.0 .. 1.0
    float y_norm = 0.0f;

    // MouseButton
    MouseButton button = MouseButton::Left;
    bool pressed = false;

    // MouseScroll
    int16_t scroll_dx = 0;
    int16_t scroll_dy = 0;

    // Key
    uint16_t scan_code = 0;
    uint16_t vk_code = 0;

    // Serialize to bytes (compact wire format)
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf;
        buf.push_back(static_cast<uint8_t>(type));

        switch (type) {
        case InputEventType::MouseMove:
            buf.resize(1 + 8);
            std::memcpy(buf.data() + 1, &x_norm, 4);
            std::memcpy(buf.data() + 5, &y_norm, 4);
            break;
        case InputEventType::MouseButton:
            buf.push_back(static_cast<uint8_t>(button));
            buf.push_back(pressed ? 1 : 0);
            break;
        case InputEventType::MouseScroll:
            buf.resize(1 + 4);
            buf[1] = static_cast<uint8_t>(scroll_dx & 0xFF);
            buf[2] = static_cast<uint8_t>((scroll_dx >> 8) & 0xFF);
            buf[3] = static_cast<uint8_t>(scroll_dy & 0xFF);
            buf[4] = static_cast<uint8_t>((scroll_dy >> 8) & 0xFF);
            break;
        case InputEventType::KeyDown:
        case InputEventType::KeyUp:
            buf.resize(1 + 4);
            buf[1] = static_cast<uint8_t>(scan_code & 0xFF);
            buf[2] = static_cast<uint8_t>((scan_code >> 8) & 0xFF);
            buf[3] = static_cast<uint8_t>(vk_code & 0xFF);
            buf[4] = static_cast<uint8_t>((vk_code >> 8) & 0xFF);
            break;
        }
        return buf;
    }

    // Deserialize from bytes. Returns false if invalid.
    static bool deserialize(const uint8_t* data, size_t len, InputEvent& out) {
        if (len < 1) return false;
        out.type = static_cast<InputEventType>(data[0]);

        switch (out.type) {
        case InputEventType::MouseMove:
            if (len < 9) return false;
            std::memcpy(&out.x_norm, data + 1, 4);
            std::memcpy(&out.y_norm, data + 5, 4);
            return true;
        case InputEventType::MouseButton:
            if (len < 3) return false;
            out.button = static_cast<MouseButton>(data[1]);
            out.pressed = data[2] != 0;
            return true;
        case InputEventType::MouseScroll:
            if (len < 5) return false;
            out.scroll_dx = static_cast<int16_t>(data[1] | (data[2] << 8));
            out.scroll_dy = static_cast<int16_t>(data[3] | (data[4] << 8));
            return true;
        case InputEventType::KeyDown:
        case InputEventType::KeyUp:
            if (len < 5) return false;
            out.scan_code = static_cast<uint16_t>(data[1] | (data[2] << 8));
            out.vk_code = static_cast<uint16_t>(data[3] | (data[4] << 8));
            return true;
        }
        return false;
    }
};

} // namespace deskbeam::protocol
