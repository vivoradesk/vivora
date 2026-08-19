#ifdef VIVORA_WINDOWS

#include "host/input/input_injector.h"

#include <algorithm>

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

    void set_screen_origin(int32_t x, int32_t y) override {
        origin_x_ = x;
        origin_y_ = y;
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
                // Map through the VIRTUAL DESKTOP so the position lands on
                // the captured display even when it's not the primary
                // (VIV-50 monitor switch).  Plain ABSOLUTE spans only the
                // primary display: after switching to a secondary monitor
                // the injected cursor kept moving on the primary — invisible
                // in the stream, which also trapped the client in relative
                // (hidden-cursor) mode.
                input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE
                                 | MOUSEEVENTF_VIRTUALDESK;
                const double vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const double vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const double vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
                const double vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
                // Clamp to the captured display's LAST pixel, not one past it.
                // xn/yn arrive clamped to [0,1]; at 1.0 the naive origin+size
                // is the first pixel of the ADJACENT monitor, so a cursor at
                // the stream's right/bottom edge bled onto the host's next
                // display (VIV-50, reported on multi-monitor hosts).
                double px = static_cast<double>(origin_x_)
                          + static_cast<double>(xn) * screen_w_;
                double py = static_cast<double>(origin_y_)
                          + static_cast<double>(yn) * screen_h_;
                const double max_x = static_cast<double>(origin_x_) + screen_w_ - 1;
                const double max_y = static_cast<double>(origin_y_) + screen_h_ - 1;
                if (px > max_x) px = max_x;
                if (py > max_y) py = max_y;
                if (px < origin_x_) px = origin_x_;
                if (py < origin_y_) py = origin_y_;
                input.mi.dx = static_cast<LONG>((px - vx) * 65535.0 / vw);
                input.mi.dy = static_cast<LONG>((py - vy) * 65535.0 / vh);
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
    int32_t  origin_x_ = 0;   // captured display's virtual-desktop origin (VIV-50)
    int32_t  origin_y_ = 0;
    float    last_xn_       = 0.0f;
    float    last_yn_       = 0.0f;
    bool     last_xn_valid_ = false;
};

} // namespace

std::unique_ptr<InputInjector> InputInjector::create() {
    return std::make_unique<WinInputInjector>();
}

// SendInput needs no permission beyond running as the user.
std::string InputInjector::unavailable_reason() { return {}; }

} // namespace vivora::host

#endif // VIVORA_WINDOWS
