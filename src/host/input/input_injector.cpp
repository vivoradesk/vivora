#ifdef DESKBEAM_WINDOWS

#include "host/input/input_injector.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace deskbeam::host {

void InputInjector::set_screen_resolution(uint32_t width, uint32_t height) {
    screen_w_ = width;
    screen_h_ = height;
}

void InputInjector::inject(const protocol::InputEvent& event) {
    INPUT input = {};

    switch (event.type) {
    case protocol::InputEventType::MouseMove: {
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        // SendInput absolute coords: 0..65535
        input.mi.dx = static_cast<LONG>(event.x_norm * 65535.0f);
        input.mi.dy = static_cast<LONG>(event.y_norm * 65535.0f);
        SendInput(1, &input, sizeof(INPUT));
        break;
    }
    case protocol::InputEventType::MouseButton: {
        input.type = INPUT_MOUSE;
        switch (event.button) {
        case protocol::MouseButton::Left:
            input.mi.dwFlags = event.pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case protocol::MouseButton::Right:
            input.mi.dwFlags = event.pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case protocol::MouseButton::Middle:
            input.mi.dwFlags = event.pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        case protocol::MouseButton::X1:
            input.mi.dwFlags = event.pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            input.mi.mouseData = XBUTTON1;
            break;
        case protocol::MouseButton::X2:
            input.mi.dwFlags = event.pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            input.mi.mouseData = XBUTTON2;
            break;
        }
        SendInput(1, &input, sizeof(INPUT));
        break;
    }
    case protocol::InputEventType::MouseScroll: {
        if (event.scroll_dy != 0) {
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_WHEEL;
            input.mi.mouseData = static_cast<DWORD>(event.scroll_dy);
            SendInput(1, &input, sizeof(INPUT));
        }
        if (event.scroll_dx != 0) {
            input = {};
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
            input.mi.mouseData = static_cast<DWORD>(event.scroll_dx);
            SendInput(1, &input, sizeof(INPUT));
        }
        break;
    }
    case protocol::InputEventType::KeyDown:
    case protocol::InputEventType::KeyUp: {
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = event.scan_code;
        input.ki.wVk = event.vk_code;
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
        if (event.type == protocol::InputEventType::KeyUp)
            input.ki.dwFlags |= KEYEVENTF_KEYUP;
        // Extended key detection (right ctrl, right alt, arrows, etc.)
        if (event.scan_code > 0xFF ||
            event.vk_code == VK_RIGHT || event.vk_code == VK_LEFT ||
            event.vk_code == VK_UP || event.vk_code == VK_DOWN ||
            event.vk_code == VK_INSERT || event.vk_code == VK_DELETE ||
            event.vk_code == VK_HOME || event.vk_code == VK_END ||
            event.vk_code == VK_PRIOR || event.vk_code == VK_NEXT ||
            event.vk_code == VK_RCONTROL || event.vk_code == VK_RMENU) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        SendInput(1, &input, sizeof(INPUT));
        break;
    }
    }
}

} // namespace deskbeam::host

#endif // DESKBEAM_WINDOWS
