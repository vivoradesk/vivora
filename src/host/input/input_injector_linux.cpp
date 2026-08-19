#ifdef VIVORA_LINUX

#include "host/input/input_injector.h"

#include <string>
#include "common/utils/log.h"

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace vivora::host {

namespace {

constexpr const char* TAG = "INJECT";

// ABS_X / ABS_Y axis maximum reported to uinput.  Higher = finer
// resolution for the host-side mapping.  32767 is the conventional
// "tablet-like" max — apps treat this as 0..1.0 anyway.
constexpr int32_t ABS_RANGE_MAX = 32767;

// Map a Windows virtual-key code (what the client sends in vk_code) to
// the Linux input-event keycode.  Covers the common keyboard surface;
// extend as needed.  Returns 0 for unmapped (the injector then drops the
// event rather than fabricate a wrong key).
uint16_t vk_to_linux_key(uint16_t vk) {
    // Letters: VK_A..VK_Z = 0x41..0x5A, Linux KEY_A..KEY_Z = 30..54
    // (no nice contiguous formula — use lookup).
    if (vk >= 0x41 && vk <= 0x5A) {
        static const uint16_t letters[26] = {
            KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H,
            KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P,
            KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X,
            KEY_Y, KEY_Z,
        };
        return letters[vk - 0x41];
    }
    // Digits row: VK_0..VK_9 = 0x30..0x39, Linux KEY_0..KEY_9 = 11..2 (!).
    if (vk >= 0x30 && vk <= 0x39) {
        static const uint16_t digits[10] = {
            KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
            KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
        };
        return digits[vk - 0x30];
    }
    // F1..F12: VK_F1..VK_F12 = 0x70..0x7B, Linux KEY_F1..KEY_F12 = 59..68 (then 87,88).
    if (vk >= 0x70 && vk <= 0x7B) {
        static const uint16_t fkeys[12] = {
            KEY_F1, KEY_F2, KEY_F3, KEY_F4,  KEY_F5,  KEY_F6,
            KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
        };
        return fkeys[vk - 0x70];
    }
    switch (vk) {
        case 0x08: return KEY_BACKSPACE;
        case 0x09: return KEY_TAB;
        case 0x0D: return KEY_ENTER;
        case 0x10: return KEY_LEFTSHIFT;     // generic Shift
        case 0x11: return KEY_LEFTCTRL;      // generic Ctrl
        case 0x12: return KEY_LEFTALT;       // generic Alt
        case 0x13: return KEY_PAUSE;
        case 0x14: return KEY_CAPSLOCK;
        case 0x1B: return KEY_ESC;
        case 0x20: return KEY_SPACE;
        case 0x21: return KEY_PAGEUP;
        case 0x22: return KEY_PAGEDOWN;
        case 0x23: return KEY_END;
        case 0x24: return KEY_HOME;
        case 0x25: return KEY_LEFT;
        case 0x26: return KEY_UP;
        case 0x27: return KEY_RIGHT;
        case 0x28: return KEY_DOWN;
        case 0x2D: return KEY_INSERT;
        case 0x2E: return KEY_DELETE;
        case 0x5B: return KEY_LEFTMETA;      // Win / Super
        case 0xBA: return KEY_SEMICOLON;
        case 0xBB: return KEY_EQUAL;
        case 0xBC: return KEY_COMMA;
        case 0xBD: return KEY_MINUS;
        case 0xBE: return KEY_DOT;
        case 0xBF: return KEY_SLASH;
        case 0xC0: return KEY_GRAVE;         // backtick
        case 0xDB: return KEY_LEFTBRACE;
        case 0xDC: return KEY_BACKSLASH;
        case 0xDD: return KEY_RIGHTBRACE;
        case 0xDE: return KEY_APOSTROPHE;
        case 0xA0: return KEY_LEFTSHIFT;
        case 0xA1: return KEY_RIGHTSHIFT;
        case 0xA2: return KEY_LEFTCTRL;
        case 0xA3: return KEY_RIGHTCTRL;
        case 0xA4: return KEY_LEFTALT;
        case 0xA5: return KEY_RIGHTALT;
        default:   return 0;
    }
}

class UinputInjector : public InputInjector {
public:
    UinputInjector() = default;
    ~UinputInjector() override { close_dev(); }

    bool open_dev() {
        fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (fd_ < 0) {
            log::error(TAG, "open(/dev/uinput) failed: %s "
                       "(add user to 'input' group: "
                       "sudo usermod -aG input $USER && relog)",
                       std::strerror(errno));
            return false;
        }

        // Primary device = absolute pointer + keyboard.  It deliberately
        // does NOT carry EV_REL: libinput classifies any device exposing
        // REL_X/REL_Y as a *relative* mouse and then silently ignores the
        // EV_ABS motion we send for normal cursor positioning (verified on
        // X11 — a REL+ABS device leaves the cursor frozen, an ABS-only one
        // jumps to the right spot).  Relative motion + wheel live on a
        // separate device (fd_rel_) created below.
        ::ioctl(fd_, UI_SET_EVBIT, EV_KEY);
        ::ioctl(fd_, UI_SET_EVBIT, EV_ABS);
        ::ioctl(fd_, UI_SET_EVBIT, EV_SYN);

        // Mouse buttons.
        ::ioctl(fd_, UI_SET_KEYBIT, BTN_LEFT);
        ::ioctl(fd_, UI_SET_KEYBIT, BTN_RIGHT);
        ::ioctl(fd_, UI_SET_KEYBIT, BTN_MIDDLE);
        ::ioctl(fd_, UI_SET_KEYBIT, BTN_SIDE);
        ::ioctl(fd_, UI_SET_KEYBIT, BTN_EXTRA);

        // Absolute axes for the standard pointer path.
        ::ioctl(fd_, UI_SET_ABSBIT, ABS_X);
        ::ioctl(fd_, UI_SET_ABSBIT, ABS_Y);

        // Mark the device as an (indirect) pointer.  Without this, libinput
        // on X11/Wayland sees a device carrying BOTH REL_X/Y and ABS_X/Y and
        // classifies it as a plain relative mouse — silently ignoring the
        // EV_ABS motion we send for normal (non-locked) cursor control, so
        // the host cursor never moves even though keyboard injection works.
        // INPUT_PROP_POINTER tells the stack the absolute axes map to the
        // screen like a tablet/pointer (not a touchscreen → not DIRECT).
        ::ioctl(fd_, UI_SET_PROPBIT, INPUT_PROP_POINTER);

        // Register every keyboard key we know how to translate to.
        // Letters + digits + F1-F12.
        for (uint16_t k = KEY_1; k <= KEY_EQUAL; ++k)   ::ioctl(fd_, UI_SET_KEYBIT, k);
        for (uint16_t k = KEY_Q; k <= KEY_RIGHTBRACE; ++k) ::ioctl(fd_, UI_SET_KEYBIT, k);
        for (uint16_t k = KEY_A; k <= KEY_GRAVE; ++k)   ::ioctl(fd_, UI_SET_KEYBIT, k);
        for (uint16_t k = KEY_BACKSLASH; k <= KEY_SLASH; ++k) ::ioctl(fd_, UI_SET_KEYBIT, k);
        for (uint16_t k = KEY_F1; k <= KEY_F12; ++k)    ::ioctl(fd_, UI_SET_KEYBIT, k);
        // Modifiers + navigation.
        const uint16_t extra[] = {
            KEY_BACKSPACE, KEY_TAB, KEY_ENTER, KEY_LEFTSHIFT, KEY_RIGHTSHIFT,
            KEY_LEFTCTRL,  KEY_RIGHTCTRL, KEY_LEFTALT, KEY_RIGHTALT,
            KEY_PAUSE, KEY_CAPSLOCK, KEY_ESC, KEY_SPACE,
            KEY_PAGEUP, KEY_PAGEDOWN, KEY_END, KEY_HOME,
            KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN,
            KEY_INSERT, KEY_DELETE, KEY_LEFTMETA, KEY_RIGHTMETA,
        };
        for (uint16_t k : extra) ::ioctl(fd_, UI_SET_KEYBIT, k);

        // Configure the absolute axis range.
        uinput_abs_setup abs_x{};
        abs_x.code = ABS_X;
        abs_x.absinfo.minimum = 0;
        abs_x.absinfo.maximum = ABS_RANGE_MAX;
        ::ioctl(fd_, UI_ABS_SETUP, &abs_x);
        uinput_abs_setup abs_y{};
        abs_y.code = ABS_Y;
        abs_y.absinfo.minimum = 0;
        abs_y.absinfo.maximum = ABS_RANGE_MAX;
        ::ioctl(fd_, UI_ABS_SETUP, &abs_y);

        uinput_setup usetup{};
        usetup.id.bustype = BUS_USB;
        usetup.id.vendor  = 0xDE5B;     // "DESB"
        usetup.id.product = 0x0001;
        std::strncpy(usetup.name, "Vivora Virtual Input", sizeof(usetup.name) - 1);
        if (::ioctl(fd_, UI_DEV_SETUP, &usetup) < 0) {
            log::error(TAG, "UI_DEV_SETUP failed: %s", std::strerror(errno));
            close_dev();
            return false;
        }
        if (::ioctl(fd_, UI_DEV_CREATE) < 0) {
            log::error(TAG, "UI_DEV_CREATE failed: %s", std::strerror(errno));
            close_dev();
            return false;
        }

        // Secondary device: relative pointer (game-mode locked cursor) +
        // wheel.  Kept separate from the absolute device above so each gets
        // an unambiguous libinput classification.  A button is declared so
        // libinput treats it as a mouse rather than a bare axis device.
        open_rel_dev();

        log::info(TAG, "uinput device created (abs pointer + keyboard%s)",
                  fd_rel_ >= 0 ? ", rel pointer" : "");
        return true;
    }

    // Best-effort — relative motion / scroll just won't work if this fails,
    // but absolute positioning (the common path) still does.
    void open_rel_dev() {
        fd_rel_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (fd_rel_ < 0) {
            log::warn(TAG, "open(/dev/uinput) for relative device failed: %s",
                      std::strerror(errno));
            return;
        }
        ::ioctl(fd_rel_, UI_SET_EVBIT, EV_KEY);
        ::ioctl(fd_rel_, UI_SET_EVBIT, EV_REL);
        ::ioctl(fd_rel_, UI_SET_EVBIT, EV_SYN);
        ::ioctl(fd_rel_, UI_SET_KEYBIT, BTN_LEFT);
        ::ioctl(fd_rel_, UI_SET_RELBIT, REL_X);
        ::ioctl(fd_rel_, UI_SET_RELBIT, REL_Y);
        ::ioctl(fd_rel_, UI_SET_RELBIT, REL_WHEEL);
        ::ioctl(fd_rel_, UI_SET_RELBIT, REL_HWHEEL);
        ::ioctl(fd_rel_, UI_SET_PROPBIT, INPUT_PROP_POINTER);

        uinput_setup us{};
        us.id.bustype = BUS_USB;
        us.id.vendor  = 0xDE5B;
        us.id.product = 0x0002;
        std::strncpy(us.name, "Vivora Virtual Pointer (rel)", sizeof(us.name) - 1);
        if (::ioctl(fd_rel_, UI_DEV_SETUP, &us) < 0 ||
            ::ioctl(fd_rel_, UI_DEV_CREATE) < 0) {
            log::warn(TAG, "relative uinput device setup failed: %s",
                      std::strerror(errno));
            ::close(fd_rel_);
            fd_rel_ = -1;
        }
    }

    void set_screen_resolution(uint32_t width, uint32_t height) override {
        screen_w_ = width;
        screen_h_ = height;
    }

    void inject(const protocol::InputEvent& event) override {
        if (fd_ < 0) return;
        switch (event.type) {
        case protocol::InputEventType::MouseMove: {
            // Map normalised 0..1 → 0..ABS_RANGE_MAX.
            int32_t x = static_cast<int32_t>(event.x_norm * ABS_RANGE_MAX + 0.5f);
            int32_t y = static_cast<int32_t>(event.y_norm * ABS_RANGE_MAX + 0.5f);
            if (x < 0) x = 0; if (x > ABS_RANGE_MAX) x = ABS_RANGE_MAX;
            if (y < 0) y = 0; if (y > ABS_RANGE_MAX) y = ABS_RANGE_MAX;
            emit(EV_ABS, ABS_X, x);
            emit(EV_ABS, ABS_Y, y);
            sync();
            break;
        }
        case protocol::InputEventType::MouseMoveRelative:
            if (event.dx != 0) emit_rel(REL_X, event.dx);
            if (event.dy != 0) emit_rel(REL_Y, event.dy);
            sync_rel();
            break;
        case protocol::InputEventType::MouseButton: {
            uint16_t btn = 0;
            switch (event.button) {
                case protocol::MouseButton::Left:   btn = BTN_LEFT;   break;
                case protocol::MouseButton::Right:  btn = BTN_RIGHT;  break;
                case protocol::MouseButton::Middle: btn = BTN_MIDDLE; break;
                case protocol::MouseButton::X1:     btn = BTN_SIDE;   break;
                case protocol::MouseButton::X2:     btn = BTN_EXTRA;  break;
            }
            if (btn) {
                emit(EV_KEY, btn, event.pressed ? 1 : 0);
                sync();
            }
            break;
        }
        case protocol::InputEventType::MouseScroll: {
            // Wire format is in WHEEL_DELTA units (120 per notch). Linux
            // REL_WHEEL is in notches (positive = up).
            int dy_notches = event.scroll_dy / 120;
            int dx_notches = event.scroll_dx / 120;
            if (dy_notches == 0 && event.scroll_dy != 0) dy_notches = event.scroll_dy > 0 ? 1 : -1;
            if (dx_notches == 0 && event.scroll_dx != 0) dx_notches = event.scroll_dx > 0 ? 1 : -1;
            if (dy_notches) emit_rel(REL_WHEEL,  dy_notches);
            if (dx_notches) emit_rel(REL_HWHEEL, dx_notches);
            if (dy_notches || dx_notches) sync_rel();
            break;
        }
        case protocol::InputEventType::KeyDown:
        case protocol::InputEventType::KeyUp: {
            uint16_t k = vk_to_linux_key(event.vk_code);
            if (!k) return;
            emit(EV_KEY, k, event.type == protocol::InputEventType::KeyDown ? 1 : 0);
            sync();
            break;
        }
        }
    }

private:
    void emit(uint16_t type, uint16_t code, int32_t value) {
        input_event ev{};
        ev.type  = type;
        ev.code  = code;
        ev.value = value;
        ssize_t n = ::write(fd_, &ev, sizeof(ev));
        (void)n;  // best-effort — uinput writes don't fail in practice
    }
    void sync() { emit(EV_SYN, SYN_REPORT, 0); }

    // Relative / wheel events go to the secondary device (fd_rel_).
    void emit_rel(uint16_t code, int32_t value) {
        if (fd_rel_ < 0) return;
        input_event ev{};
        ev.type  = EV_REL;
        ev.code  = code;
        ev.value = value;
        ssize_t n = ::write(fd_rel_, &ev, sizeof(ev));
        (void)n;
    }
    void sync_rel() {
        if (fd_rel_ < 0) return;
        input_event ev{};
        ev.type = EV_SYN; ev.code = SYN_REPORT; ev.value = 0;
        ssize_t n = ::write(fd_rel_, &ev, sizeof(ev));
        (void)n;
    }

    void close_dev() {
        if (fd_ >= 0) {
            ::ioctl(fd_, UI_DEV_DESTROY);
            ::close(fd_);
            fd_ = -1;
        }
        if (fd_rel_ >= 0) {
            ::ioctl(fd_rel_, UI_DEV_DESTROY);
            ::close(fd_rel_);
            fd_rel_ = -1;
        }
    }

    int fd_ = -1;
    int fd_rel_ = -1;
    uint32_t screen_w_ = 1920;
    uint32_t screen_h_ = 1080;
};

} // namespace

std::unique_ptr<InputInjector> InputInjector::create() {
    auto inj = std::make_unique<UinputInjector>();
    if (!inj->open_dev()) return nullptr;
    return inj;
}

std::string InputInjector::unavailable_reason() {
    // Distinguish the two failures the user can actually act on: the module
    // is not loaded at all, or it is loaded and we are not in a group that
    // may write to it.  Everything else falls through to the generic text.
    if (::access("/dev/uinput", F_OK) != 0) {
        return "Remote keyboard and mouse are unavailable: /dev/uinput does not "
               "exist. Load the module with `sudo modprobe uinput`, then install "
               "the udev rule shipped in packaging/linux/.";
    }
    if (::access("/dev/uinput", W_OK) != 0) {
        return "Remote keyboard and mouse are unavailable: no write access to "
               "/dev/uinput. Install the udev rule from packaging/linux/ and run "
               "`sudo usermod -aG input $USER`, then log out and back in.";
    }
    return {};
}

} // namespace vivora::host

#endif // VIVORA_LINUX
