#ifdef DESKBEAM_MACOS

#include "host/input/input_injector.h"
#include "common/utils/log.h"

#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>
#include <algorithm>

namespace deskbeam::host {

namespace {

static constexpr const char* TAG = "MAC_INPUT";

// Windows VK → Mac kVK. Covers the same keys as the forward table in
// mac_video_view.mm. Returns -1 if unknown.
static int win_vk_to_mac_kc(uint16_t vk) {
    switch (vk) {
        // Letters (A-Z)
        case 0x41: return 0x00; // A
        case 0x42: return 0x0B; // B
        case 0x43: return 0x08; // C
        case 0x44: return 0x02; // D
        case 0x45: return 0x0E; // E
        case 0x46: return 0x03; // F
        case 0x47: return 0x05; // G
        case 0x48: return 0x04; // H
        case 0x49: return 0x22; // I
        case 0x4A: return 0x26; // J
        case 0x4B: return 0x28; // K
        case 0x4C: return 0x25; // L
        case 0x4D: return 0x2E; // M
        case 0x4E: return 0x2D; // N
        case 0x4F: return 0x1F; // O
        case 0x50: return 0x23; // P
        case 0x51: return 0x0C; // Q
        case 0x52: return 0x0F; // R
        case 0x53: return 0x01; // S
        case 0x54: return 0x11; // T
        case 0x55: return 0x20; // U
        case 0x56: return 0x09; // V
        case 0x57: return 0x0D; // W
        case 0x58: return 0x07; // X
        case 0x59: return 0x10; // Y
        case 0x5A: return 0x06; // Z

        // Digits
        case 0x30: return 0x1D; // 0
        case 0x31: return 0x12; // 1
        case 0x32: return 0x13; // 2
        case 0x33: return 0x14; // 3
        case 0x34: return 0x15; // 4
        case 0x35: return 0x17; // 5
        case 0x36: return 0x16; // 6
        case 0x37: return 0x1A; // 7
        case 0x38: return 0x1C; // 8
        case 0x39: return 0x19; // 9

        // Punctuation (VK_OEM_*)
        case 0xBD: return 0x1B; // - _
        case 0xBB: return 0x18; // = +
        case 0xDB: return 0x21; // [ {
        case 0xDD: return 0x1E; // ] }
        case 0xDC: return 0x2A; // backslash |
        case 0xBA: return 0x29; // ; :
        case 0xDE: return 0x27; // ' "
        case 0xC0: return 0x32; // ` ~
        case 0xBC: return 0x2B; // , <
        case 0xBE: return 0x2F; // . >
        case 0xBF: return 0x2C; // / ?

        // Control
        case 0x0D: return 0x24; // Return
        case 0x09: return 0x30; // Tab
        case 0x20: return 0x31; // Space
        case 0x08: return 0x33; // Backspace
        case 0x1B: return 0x35; // Escape

        // Navigation
        case 0x2E: return 0x75; // Delete (forward)
        case 0x24: return 0x73; // Home
        case 0x23: return 0x77; // End
        case 0x21: return 0x74; // PageUp
        case 0x22: return 0x79; // PageDown
        case 0x25: return 0x7B; // Left
        case 0x27: return 0x7C; // Right
        case 0x28: return 0x7D; // Down
        case 0x26: return 0x7E; // Up

        // Modifiers
        case 0xA0: return 0x38; // LShift
        case 0xA1: return 0x3C; // RShift
        case 0xA2: return 0x3B; // LControl
        case 0xA3: return 0x3E; // RControl
        case 0xA4: return 0x3A; // LAlt → LOption
        case 0xA5: return 0x3D; // RAlt → ROption
        case 0x5B: return 0x37; // LWin → LCommand
        case 0x5C: return 0x36; // RWin → RCommand
        case 0x14: return 0x39; // CapsLock

        // F-keys
        case 0x70: return 0x7A; // F1
        case 0x71: return 0x78; // F2
        case 0x72: return 0x63; // F3
        case 0x73: return 0x76; // F4
        case 0x74: return 0x60; // F5
        case 0x75: return 0x61; // F6
        case 0x76: return 0x62; // F7
        case 0x77: return 0x64; // F8
        case 0x78: return 0x65; // F9
        case 0x79: return 0x6D; // F10
        case 0x7A: return 0x67; // F11
        case 0x7B: return 0x6F; // F12

        default: return -1;
    }
}

class MacInputInjector : public InputInjector {
public:
    MacInputInjector() {
        event_source_ = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
        if (!event_source_) {
            log::error(TAG, "CGEventSourceCreate failed");
        }
    }

    ~MacInputInjector() override {
        if (event_source_) CFRelease(event_source_);
    }

    void set_screen_resolution(uint32_t width, uint32_t height) override {
        screen_w_ = width;
        screen_h_ = height;
    }

    void inject(const protocol::InputEvent& event) override {
        switch (event.type) {
        case protocol::InputEventType::MouseMove:
            handle_mouse_move(event.x_norm, event.y_norm);
            break;
        case protocol::InputEventType::MouseMoveRelative:
            handle_mouse_move_relative(event.dx, event.dy);
            break;
        case protocol::InputEventType::MouseButton:
            handle_mouse_button(event.button, event.pressed);
            break;
        case protocol::InputEventType::MouseScroll:
            handle_mouse_scroll(event.scroll_dx, event.scroll_dy);
            break;
        case protocol::InputEventType::KeyDown:
        case protocol::InputEventType::KeyUp:
            handle_key(event.vk_code,
                       event.type == protocol::InputEventType::KeyDown);
            break;
        }
    }

private:
    void handle_mouse_move(float x_norm, float y_norm) {
        last_mouse_x_ = x_norm * static_cast<float>(screen_w_);
        last_mouse_y_ = y_norm * static_cast<float>(screen_h_);
        CGPoint p = CGPointMake(last_mouse_x_, last_mouse_y_);

        // Use the button state to decide whether this is a drag event.
        CGEventType type = kCGEventMouseMoved;
        CGMouseButton button = kCGMouseButtonLeft;
        if (buttons_down_ & (1 << 0)) { type = kCGEventLeftMouseDragged;   button = kCGMouseButtonLeft; }
        else if (buttons_down_ & (1 << 1)) { type = kCGEventRightMouseDragged;  button = kCGMouseButtonRight; }
        else if (buttons_down_ & (1 << 2)) { type = kCGEventOtherMouseDragged;  button = kCGMouseButtonCenter; }

        CGEventRef ev = CGEventCreateMouseEvent(event_source_, type, p, button);
        if (ev) {
            CGEventPost(kCGHIDEventTap, ev);
            CFRelease(ev);
        }
    }

    void handle_mouse_move_relative(int32_t dx, int32_t dy) {
        // Games on macOS read raw HID deltas via the kCGMouseEventDeltaX/Y
        // fields.  We still post an absolute mouseMoved with the cursor
        // clamped to the screen so ordinary apps also see the movement.
        last_mouse_x_ = std::max(0.0f, std::min(static_cast<float>(screen_w_ - 1),
                                                last_mouse_x_ + static_cast<float>(dx)));
        last_mouse_y_ = std::max(0.0f, std::min(static_cast<float>(screen_h_ - 1),
                                                last_mouse_y_ + static_cast<float>(dy)));
        CGPoint p = CGPointMake(last_mouse_x_, last_mouse_y_);

        CGEventType type = kCGEventMouseMoved;
        CGMouseButton button = kCGMouseButtonLeft;
        if (buttons_down_ & (1 << 0)) { type = kCGEventLeftMouseDragged;  button = kCGMouseButtonLeft; }
        else if (buttons_down_ & (1 << 1)) { type = kCGEventRightMouseDragged; button = kCGMouseButtonRight; }
        else if (buttons_down_ & (1 << 2)) { type = kCGEventOtherMouseDragged; button = kCGMouseButtonCenter; }

        CGEventRef ev = CGEventCreateMouseEvent(event_source_, type, p, button);
        if (ev) {
            CGEventSetIntegerValueField(ev, kCGMouseEventDeltaX, dx);
            CGEventSetIntegerValueField(ev, kCGMouseEventDeltaY, dy);
            CGEventPost(kCGHIDEventTap, ev);
            CFRelease(ev);
        }
    }

    void handle_mouse_button(protocol::MouseButton btn, bool pressed) {
        CGPoint p = CGPointMake(last_mouse_x_, last_mouse_y_);

        CGEventType type;
        CGMouseButton mbtn;
        int bit = 0;
        switch (btn) {
        case protocol::MouseButton::Left:
            mbtn = kCGMouseButtonLeft;
            type = pressed ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
            bit = 0;
            break;
        case protocol::MouseButton::Right:
            mbtn = kCGMouseButtonRight;
            type = pressed ? kCGEventRightMouseDown : kCGEventRightMouseUp;
            bit = 1;
            break;
        case protocol::MouseButton::Middle:
            mbtn = kCGMouseButtonCenter;
            type = pressed ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
            bit = 2;
            break;
        default:
            return; // X1/X2 not supported on Mac
        }

        if (pressed) buttons_down_ |= (1u << bit);
        else         buttons_down_ &= ~(1u << bit);

        CGEventRef ev = CGEventCreateMouseEvent(event_source_, type, p, mbtn);
        if (ev) {
            CGEventPost(kCGHIDEventTap, ev);
            CFRelease(ev);
        }
    }

    void handle_mouse_scroll(int16_t dx, int16_t dy) {
        // Mac scroll is in "lines" for kCGScrollEventUnitLine. Windows sends 120/notch;
        // divide by ~40 to map to reasonable line count.
        int32_t lines_y = dy / 40;
        int32_t lines_x = dx / 40;
        if (lines_y == 0 && lines_x == 0) {
            if (dy != 0) lines_y = dy > 0 ? 1 : -1;
            if (dx != 0) lines_x = dx > 0 ? 1 : -1;
        }

        CGEventRef ev = CGEventCreateScrollWheelEvent(
            event_source_, kCGScrollEventUnitLine, 2, lines_y, lines_x);
        if (ev) {
            CGEventPost(kCGHIDEventTap, ev);
            CFRelease(ev);
        }
    }

    void handle_key(uint16_t vk, bool down) {
        int mac_kc = win_vk_to_mac_kc(vk);
        if (mac_kc < 0) return;

        CGEventRef ev = CGEventCreateKeyboardEvent(
            event_source_, static_cast<CGKeyCode>(mac_kc), down);
        if (ev) {
            CGEventPost(kCGHIDEventTap, ev);
            CFRelease(ev);
        }
    }

    CGEventSourceRef event_source_ = nullptr;
    uint32_t screen_w_ = 1920;
    uint32_t screen_h_ = 1080;
    float last_mouse_x_ = 0.0f;
    float last_mouse_y_ = 0.0f;
    uint32_t buttons_down_ = 0; // bit0=Left, bit1=Right, bit2=Middle
};

} // namespace

std::unique_ptr<InputInjector> InputInjector::create() {
    return std::make_unique<MacInputInjector>();
}

} // namespace deskbeam::host

#endif // DESKBEAM_MACOS
