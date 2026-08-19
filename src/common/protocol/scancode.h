#pragma once

#include <cstdint>

namespace vivora::protocol {

// Canonical keyboard scancode space for the input channel (VIV-6).
//
// The wire carries a physical key, not a character: `InputEvent::scan_code`
// says WHICH KEY was pressed and the host decides what that key means under
// its own active layout.  Anything layout-derived breaks the moment the two
// machines disagree — a client on a Cyrillic layout produces key codes that
// no Latin-keyed table can map, and the events were simply dropped.
//
// The canonical space is **PS/2 scancode set 1** (the AT "make code"), chosen
// because Windows already speaks it natively: SendInput with
// KEYEVENTF_SCANCODE takes exactly these values, and Qt on Windows hands them
// straight out of the WM_KEYDOWN lParam.  So the busiest path costs nothing to
// translate, and the other platforms convert at their edge.
//
// Extended keys (the ones a real PS/2 keyboard prefixes with 0xE0 — right
// Ctrl/Alt, the arrow cluster, keypad Enter and slash, the Meta keys) are
// encoded here as 0xE0nn, so a scancode is self-describing and does not need a
// virtual-key code alongside it to be unambiguous.  That matters: set 1 gives
// Left Arrow and Keypad-4 the same make code, and only the prefix separates
// them.
//
// Both directions return 0 for anything they cannot map, and callers treat 0
// as "fall back to vk_code" — which is what keeps older peers, and macOS,
// working unchanged.

// Linux evdev code -> set 1.
//
// The first 83 evdev codes were deliberately assigned to match AT set 1 make
// codes one-for-one (KEY_ESC 1 = 0x01, KEY_A 30 = 0x1E, KEY_KPDOT 83 = 0x53),
// as were KEY_102ND/F11/F12 at 86..88.  Everything past that is the extended
// block and needs the table.
inline uint16_t evdev_to_set1(uint16_t evdev) {
    if (evdev >= 1 && evdev <= 83) return evdev;
    switch (evdev) {
        case 86:  return 0x56;      // KEY_102ND
        case 87:  return 0x57;      // KEY_F11
        case 88:  return 0x58;      // KEY_F12
        case 96:  return 0xE01C;    // KEY_KPENTER
        case 97:  return 0xE01D;    // KEY_RIGHTCTRL
        case 98:  return 0xE035;    // KEY_KPSLASH
        case 99:  return 0xE037;    // KEY_SYSRQ (PrintScreen)
        case 100: return 0xE038;    // KEY_RIGHTALT
        case 102: return 0xE047;    // KEY_HOME
        case 103: return 0xE048;    // KEY_UP
        case 104: return 0xE049;    // KEY_PAGEUP
        case 105: return 0xE04B;    // KEY_LEFT
        case 106: return 0xE04D;    // KEY_RIGHT
        case 107: return 0xE04F;    // KEY_END
        case 108: return 0xE050;    // KEY_DOWN
        case 109: return 0xE051;    // KEY_PAGEDOWN
        case 110: return 0xE052;    // KEY_INSERT
        case 111: return 0xE053;    // KEY_DELETE
        case 113: return 0xE020;    // KEY_MUTE
        case 114: return 0xE02E;    // KEY_VOLUMEDOWN
        case 115: return 0xE030;    // KEY_VOLUMEUP
        case 125: return 0xE05B;    // KEY_LEFTMETA
        case 126: return 0xE05C;    // KEY_RIGHTMETA
        case 127: return 0xE05D;    // KEY_COMPOSE (menu key)
        // KEY_PAUSE is 0xE1 1D 45 on the wire — a three-byte sequence with no
        // single-code representation.  Left unmapped; vk_code carries it.
        default:  return 0;
    }
}

// Set 1 -> Linux evdev code.  Inverse of the above.
inline uint16_t set1_to_evdev(uint16_t set1) {
    if (set1 >= 1 && set1 <= 83) return set1;
    switch (set1) {
        case 0x56:   return 86;
        case 0x57:   return 87;
        case 0x58:   return 88;
        case 0xE01C: return 96;
        case 0xE01D: return 97;
        case 0xE035: return 98;
        case 0xE037: return 99;
        case 0xE038: return 100;
        case 0xE047: return 102;
        case 0xE048: return 103;
        case 0xE049: return 104;
        case 0xE04B: return 105;
        case 0xE04D: return 106;
        case 0xE04F: return 107;
        case 0xE050: return 108;
        case 0xE051: return 109;
        case 0xE052: return 110;
        case 0xE053: return 111;
        case 0xE020: return 113;
        case 0xE02E: return 114;
        case 0xE030: return 115;
        case 0xE05B: return 125;
        case 0xE05C: return 126;
        case 0xE05D: return 127;
        default:     return 0;
    }
}

// True for a code that a PS/2 keyboard would prefix with 0xE0.  Windows wants
// these split back into an 8-bit make code plus KEYEVENTF_EXTENDEDKEY.
inline bool set1_is_extended(uint16_t set1) { return (set1 & 0xFF00) == 0xE000; }

// The 8-bit make code, with any 0xE0 prefix stripped.
inline uint8_t set1_make_code(uint16_t set1) {
    return static_cast<uint8_t>(set1 & 0x00FF);
}

// Whether a Windows virtual-key code belongs to a key that a PS/2 keyboard
// prefixes with 0xE0.
//
// Needed because Qt on Windows reports nativeScanCode() as the bare 8-bit make
// code with no prefix, and set 1 gives Left Arrow and Keypad-4 the same one.
// A Windows viewer therefore has to reconstruct the prefix from the virtual
// key before putting a scancode on the wire, or a Linux host receiving 0x4B
// would faithfully press Keypad-4.
//
// Raw VK numbers rather than the VK_* macros so this header stays usable off
// Windows, where the table is still needed to reason about what arrives.
inline bool win_vk_is_extended(uint16_t vk) {
    switch (vk) {
        case 0x21:  // VK_PRIOR    PageUp
        case 0x22:  // VK_NEXT     PageDown
        case 0x23:  // VK_END
        case 0x24:  // VK_HOME
        case 0x25:  // VK_LEFT
        case 0x26:  // VK_UP
        case 0x27:  // VK_RIGHT
        case 0x28:  // VK_DOWN
        case 0x2C:  // VK_SNAPSHOT PrintScreen
        case 0x2D:  // VK_INSERT
        case 0x2E:  // VK_DELETE
        case 0x5B:  // VK_LWIN
        case 0x5C:  // VK_RWIN
        case 0x5D:  // VK_APPS     menu key
        case 0x6F:  // VK_DIVIDE   keypad /
        case 0xA3:  // VK_RCONTROL
        case 0xA5:  // VK_RMENU    right Alt
            return true;
        default:
            return false;
    }
}

// Build a canonical set-1 code from a bare Windows make code plus its virtual
// key: adds the 0xE0 prefix where the key needs one.
inline uint16_t win_scan_to_set1(uint16_t make_code, uint16_t vk) {
    if (make_code == 0) return 0;
    if ((make_code & 0xFF00) != 0) return make_code;   // already prefixed
    return win_vk_is_extended(vk) ? static_cast<uint16_t>(0xE000u | make_code)
                                  : make_code;
}

// Qt's nativeScanCode() on Linux -> set 1.
//
// Both the xcb and the Wayland platform plugins report keys in the X11
// convention, which is the evdev code plus 8.  Values below 9 are not real
// keys there, so they are rejected rather than wrapped around.
inline uint16_t qt_native_scan_to_set1(uint32_t native) {
    if (native < 9) return 0;
    return evdev_to_set1(static_cast<uint16_t>(native - 8));
}

} // namespace vivora::protocol
