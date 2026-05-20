#ifdef VIVORA_WINDOWS

#include "host/input/input_injector.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace vivora::host {

namespace {

class WinInputInjector : public InputInjector {
public:
    void set_screen_resolution(uint32_t width, uint32_t height) override {
        screen_w_ = width;
        screen_h_ = height;
    }

    void inject(const protocol::InputEvent& event) override {
        INPUT input = {};

        switch (event.type) {
        case protocol::InputEventType::MouseMove: {
            // Detect locked-cursor mode (games with ClipCursor + raw input).
            // When clipped, SendInput ABSOLUTE would give WM_INPUT events
            // carrying absolute screen coords (xn*65535) that FPS games
            // misread as huge relative deltas → camera spin. In that mode
            // we convert the absolute position update into a relative delta
            // and inject as MOUSEEVENTF_MOVE so raw input reports correct
            // per-event deltas.
            const float xn = event.x_norm;
            const float yn = event.y_norm;
            const bool clipped = cursor_clipped_tight();

            if (clipped && last_xn_valid_) {
                const double dx_f = (double)(xn - last_xn_) * (double)screen_w_;
                const double dy_f = (double)(yn - last_yn_) * (double)screen_h_;
                const LONG dx = static_cast<LONG>(dx_f);
                const LONG dy = static_cast<LONG>(dy_f);
                if (dx != 0 || dy != 0) {
                    input.type = INPUT_MOUSE;
                    input.mi.dwFlags = MOUSEEVENTF_MOVE;  // relative
                    input.mi.dx = dx;
                    input.mi.dy = dy;
                    SendInput(1, &input, sizeof(INPUT));
                }
            } else {
                input.type = INPUT_MOUSE;
                input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
                // SendInput absolute coords: 0..65535
                // TODO(multi-monitor): MOUSEEVENTF_VIRTUALDESK required for
                // multi-monitor capture; today host pins monitor 0.
                input.mi.dx = static_cast<LONG>(xn * 65535.0f);
                input.mi.dy = static_cast<LONG>(yn * 65535.0f);
                SendInput(1, &input, sizeof(INPUT));
            }

            last_xn_ = xn;
            last_yn_ = yn;
            last_xn_valid_ = true;
            break;
        }
        case protocol::InputEventType::MouseMoveRelative: {
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            input.mi.dx = static_cast<LONG>(event.dx);
            input.mi.dy = static_cast<LONG>(event.dy);
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
            // Linux/Mac clients can't produce a Windows scancode locally
            // and send scan_code=0 with a valid vk_code — derive the
            // scancode from VK on this host's active layout. Windows
            // client sends both natively (matching the user's physical
            // keyboard), so we keep that path identical.
            uint16_t scan = event.scan_code;
            if (scan == 0 && event.vk_code != 0) {
                scan = static_cast<uint16_t>(
                    MapVirtualKeyW(event.vk_code, MAPVK_VK_TO_VSC));
            }
            input.ki.wScan = scan;
            input.ki.wVk = event.vk_code;
            input.ki.dwFlags = KEYEVENTF_SCANCODE;
            if (event.type == protocol::InputEventType::KeyUp)
                input.ki.dwFlags |= KEYEVENTF_KEYUP;
            // Extended key detection (right ctrl, right alt, arrows, etc.)
            if (scan > 0xFF ||
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

private:
    // True if the foreground app has restricted the cursor clip rect below
    // primary monitor size (ClipCursor with a tight rect — standard pattern
    // for FPS games that lock the cursor center-screen).
    bool cursor_clipped_tight() const {
        RECT clip{};
        if (!GetClipCursor(&clip)) return false;
        const int cw = clip.right  - clip.left;
        const int ch = clip.bottom - clip.top;
        const int mw = GetSystemMetrics(SM_CXSCREEN);
        const int mh = GetSystemMetrics(SM_CYSCREEN);
        // Any meaningful shrinkage → treat as game mode. Use a small slack
        // (a few px) to absorb DWM / task bar quirks.
        return (cw + 4 < mw) || (ch + 4 < mh);
    }

    uint32_t screen_w_ = 1920;
    uint32_t screen_h_ = 1080;
    float    last_xn_       = 0.0f;
    float    last_yn_       = 0.0f;
    bool     last_xn_valid_ = false;
};

} // namespace

std::unique_ptr<InputInjector> InputInjector::create() {
    return std::make_unique<WinInputInjector>();
}

} // namespace vivora::host

#endif // VIVORA_WINDOWS
