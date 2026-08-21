// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

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
        // The keys around the space bar on a Japanese layout.  Here because
        // JIS Macs carry them: without these rows a JIS viewer sends nothing
        // for four of its keys.
        case 89:  return 0x73;      // KEY_RO (backslash / underscore)
        case 92:  return 0x79;      // KEY_HENKAN
        case 93:  return 0x70;      // KEY_KATAKANAHIRAGANA
        case 94:  return 0x7B;      // KEY_MUHENKAN
        case 95:  return 0x7E;      // KEY_KPJPCOMMA
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
        case 124: return 0x7D;      // KEY_YEN
        case 125: return 0xE05B;    // KEY_LEFTMETA
        case 126: return 0xE05C;    // KEY_RIGHTMETA
        case 127: return 0xE05D;    // KEY_COMPOSE (menu key)
        // F13..F20 -- the extra function row a full Apple keyboard has and a
        // typical PC one does not.
        case 183: return 0x64;      // KEY_F13
        case 184: return 0x65;      // KEY_F14
        case 185: return 0x66;      // KEY_F15
        case 186: return 0x67;      // KEY_F16
        case 187: return 0x68;      // KEY_F17
        case 188: return 0x69;      // KEY_F18
        case 189: return 0x6A;      // KEY_F19
        case 190: return 0x6B;      // KEY_F20
        // KEY_PAUSE is 0xE1 1D 45 on the wire -- a three-byte sequence with no
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
        case 0x64:   return 183;
        case 0x65:   return 184;
        case 0x66:   return 185;
        case 0x67:   return 186;
        case 0x68:   return 187;
        case 0x69:   return 188;
        case 0x6A:   return 189;
        case 0x6B:   return 190;
        case 0x70:   return 93;
        case 0x73:   return 89;
        case 0x79:   return 92;
        case 0x7B:   return 94;
        case 0x7D:   return 124;
        case 0x7E:   return 95;
        case 0xE01C: return 96;
        case 0xE01D: return 97;
        case 0xE020: return 113;
        case 0xE02E: return 114;
        case 0xE030: return 115;
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

// macOS virtual keycode (kVK_*) -> set 1.
//
// The mac keycodes are a positional space of their own with no arithmetic
// relationship to set 1, so this is a plain table.  It lives in the shared
// header rather than in the macOS sources for two reasons: the host injector
// and the viewer need exact inverses of each other, and a table that lives
// here is exercised by scancode_test on every platform — which is the only
// way it gets tested at all while macOS has no CI.
//
// Apple modifiers keep their Apple meaning: Command travels as the Meta key
// (what a PC calls Win / Super) and Option as Alt, which is what the rest of
// the input path already assumed.
inline uint16_t mac_kc_to_set1(uint16_t kc) {
    switch (kc) {
        // Letters
        case 0x00: return 0x1E;   // A
        case 0x01: return 0x1F;   // S
        case 0x02: return 0x20;   // D
        case 0x03: return 0x21;   // F
        case 0x04: return 0x23;   // H
        case 0x05: return 0x22;   // G
        case 0x06: return 0x2C;   // Z
        case 0x07: return 0x2D;   // X
        case 0x08: return 0x2E;   // C
        case 0x09: return 0x2F;   // V
        case 0x0B: return 0x30;   // B
        case 0x0C: return 0x10;   // Q
        case 0x0D: return 0x11;   // W
        case 0x0E: return 0x12;   // E
        case 0x0F: return 0x13;   // R
        case 0x10: return 0x15;   // Y
        case 0x11: return 0x14;   // T
        case 0x1F: return 0x18;   // O
        case 0x20: return 0x16;   // U
        case 0x22: return 0x17;   // I
        case 0x23: return 0x19;   // P
        case 0x25: return 0x26;   // L
        case 0x26: return 0x24;   // J
        case 0x28: return 0x25;   // K
        case 0x2D: return 0x31;   // N
        case 0x2E: return 0x32;   // M

        // Digit row
        case 0x12: return 0x02;   // 1
        case 0x13: return 0x03;   // 2
        case 0x14: return 0x04;   // 3
        case 0x15: return 0x05;   // 4
        case 0x17: return 0x06;   // 5
        case 0x16: return 0x07;   // 6
        case 0x1A: return 0x08;   // 7
        case 0x1C: return 0x09;   // 8
        case 0x19: return 0x0A;   // 9
        case 0x1D: return 0x0B;   // 0

        // Punctuation
        case 0x1B: return 0x0C;   // minus
        case 0x18: return 0x0D;   // equals
        case 0x21: return 0x1A;   // left bracket
        case 0x1E: return 0x1B;   // right bracket
        case 0x2A: return 0x2B;   // backslash
        case 0x29: return 0x27;   // semicolon
        case 0x27: return 0x28;   // quote
        case 0x32: return 0x29;   // grave
        case 0x2B: return 0x33;   // comma
        case 0x2F: return 0x34;   // period
        case 0x2C: return 0x35;   // slash
        case 0x0A: return 0x56;   // kVK_ISO_Section — the 102nd key

        // Control cluster
        case 0x24: return 0x1C;   // Return
        case 0x30: return 0x0F;   // Tab
        case 0x31: return 0x39;   // Space
        case 0x33: return 0x0E;   // Delete -> Backspace
        case 0x35: return 0x01;   // Escape

        // Navigation.  All extended, so the arrows cannot collide with the
        // keypad digits that share their make codes.
        case 0x72: return 0xE052; // Help -> Insert
        case 0x73: return 0xE047; // Home
        case 0x74: return 0xE049; // PageUp
        case 0x75: return 0xE053; // ForwardDelete
        case 0x77: return 0xE04F; // End
        case 0x79: return 0xE051; // PageDown
        case 0x7B: return 0xE04B; // Left
        case 0x7C: return 0xE04D; // Right
        case 0x7D: return 0xE050; // Down
        case 0x7E: return 0xE048; // Up

        // Modifiers
        case 0x38: return 0x2A;   // Shift
        case 0x3C: return 0x36;   // RightShift
        case 0x3B: return 0x1D;   // Control
        case 0x3E: return 0xE01D; // RightControl
        case 0x3A: return 0x38;   // Option -> Alt
        case 0x3D: return 0xE038; // RightOption -> right Alt
        case 0x37: return 0xE05B; // Command -> left Meta
        case 0x36: return 0xE05C; // RightCommand -> right Meta
        case 0x39: return 0x3A;   // CapsLock

        // Function row
        case 0x7A: return 0x3B;   // F1
        case 0x78: return 0x3C;   // F2
        case 0x63: return 0x3D;   // F3
        case 0x76: return 0x3E;   // F4
        case 0x60: return 0x3F;   // F5
        case 0x61: return 0x40;   // F6
        case 0x62: return 0x41;   // F7
        case 0x64: return 0x42;   // F8
        case 0x65: return 0x43;   // F9
        case 0x6D: return 0x44;   // F10
        case 0x67: return 0x57;   // F11
        case 0x6F: return 0x58;   // F12
        case 0x69: return 0x64;   // F13
        case 0x6B: return 0x65;   // F14
        case 0x71: return 0x66;   // F15
        case 0x6A: return 0x67;   // F16
        case 0x40: return 0x68;   // F17
        case 0x4F: return 0x69;   // F18
        case 0x50: return 0x6A;   // F19
        case 0x5A: return 0x6B;   // F20

        // Keypad
        case 0x52: return 0x52;   // 0
        case 0x53: return 0x4F;   // 1
        case 0x54: return 0x50;   // 2
        case 0x55: return 0x51;   // 3
        case 0x56: return 0x4B;   // 4
        case 0x57: return 0x4C;   // 5
        case 0x58: return 0x4D;   // 6
        case 0x59: return 0x47;   // 7
        case 0x5B: return 0x48;   // 8
        case 0x5C: return 0x49;   // 9
        case 0x41: return 0x53;   // decimal
        case 0x43: return 0x37;   // multiply
        case 0x45: return 0x4E;   // plus
        case 0x4E: return 0x4A;   // minus
        case 0x4B: return 0xE035; // divide
        case 0x4C: return 0xE01C; // Enter
        case 0x47: return 0x45;   // Clear — sits where NumLock does on a PC

        // Media keys on the Apple function row
        case 0x48: return 0xE030; // VolumeUp
        case 0x49: return 0xE02E; // VolumeDown
        case 0x4A: return 0xE020; // Mute

        // Japanese layout
        case 0x5D: return 0x7D;   // kVK_JIS_Yen
        case 0x5E: return 0x73;   // kVK_JIS_Underscore -> KEY_RO
        case 0x5F: return 0x7E;   // kVK_JIS_KeypadComma
        case 0x66: return 0x7B;   // kVK_JIS_Eisu -> muhenkan, its PC analogue
        case 0x68: return 0x79;   // kVK_JIS_Kana -> henkan

        // kVK_Function (0x3F) is the fn key: no PC counterpart, and macOS
        // consumes it locally anyway.  Keypad equals (0x51) likewise — PCs
        // have no such key.
        default:   return 0;
    }
}

// Set 1 -> macOS virtual keycode.  Exact inverse of the table above; returns
// -1 for a key macOS has no equivalent for, so callers fall back instead of
// pressing something arbitrary.
inline int set1_to_mac_kc(uint16_t set1) {
    switch (set1) {
        case 0x1E: return 0x00;   // A
        case 0x1F: return 0x01;   // S
        case 0x20: return 0x02;   // D
        case 0x21: return 0x03;   // F
        case 0x23: return 0x04;   // H
        case 0x22: return 0x05;   // G
        case 0x2C: return 0x06;   // Z
        case 0x2D: return 0x07;   // X
        case 0x2E: return 0x08;   // C
        case 0x2F: return 0x09;   // V
        case 0x30: return 0x0B;   // B
        case 0x10: return 0x0C;   // Q
        case 0x11: return 0x0D;   // W
        case 0x12: return 0x0E;   // E
        case 0x13: return 0x0F;   // R
        case 0x15: return 0x10;   // Y
        case 0x14: return 0x11;   // T
        case 0x18: return 0x1F;   // O
        case 0x16: return 0x20;   // U
        case 0x17: return 0x22;   // I
        case 0x19: return 0x23;   // P
        case 0x26: return 0x25;   // L
        case 0x24: return 0x26;   // J
        case 0x25: return 0x28;   // K
        case 0x31: return 0x2D;   // N
        case 0x32: return 0x2E;   // M

        case 0x02: return 0x12;   // 1
        case 0x03: return 0x13;   // 2
        case 0x04: return 0x14;   // 3
        case 0x05: return 0x15;   // 4
        case 0x06: return 0x17;   // 5
        case 0x07: return 0x16;   // 6
        case 0x08: return 0x1A;   // 7
        case 0x09: return 0x1C;   // 8
        case 0x0A: return 0x19;   // 9
        case 0x0B: return 0x1D;   // 0

        case 0x0C: return 0x1B;   // minus
        case 0x0D: return 0x18;   // equals
        case 0x1A: return 0x21;   // left bracket
        case 0x1B: return 0x1E;   // right bracket
        case 0x2B: return 0x2A;   // backslash
        case 0x27: return 0x29;   // semicolon
        case 0x28: return 0x27;   // quote
        case 0x29: return 0x32;   // grave
        case 0x33: return 0x2B;   // comma
        case 0x34: return 0x2F;   // period
        case 0x35: return 0x2C;   // slash
        case 0x56: return 0x0A;   // 102nd key -> kVK_ISO_Section

        case 0x1C: return 0x24;   // Enter -> Return
        case 0x0F: return 0x30;   // Tab
        case 0x39: return 0x31;   // Space
        case 0x0E: return 0x33;   // Backspace -> Delete
        case 0x01: return 0x35;   // Escape

        case 0xE052: return 0x72; // Insert -> Help
        case 0xE047: return 0x73; // Home
        case 0xE049: return 0x74; // PageUp
        case 0xE053: return 0x75; // Delete -> ForwardDelete
        case 0xE04F: return 0x77; // End
        case 0xE051: return 0x79; // PageDown
        case 0xE04B: return 0x7B; // Left
        case 0xE04D: return 0x7C; // Right
        case 0xE050: return 0x7D; // Down
        case 0xE048: return 0x7E; // Up

        case 0x2A: return 0x38;   // LShift
        case 0x36: return 0x3C;   // RShift
        case 0x1D: return 0x3B;   // LCtrl
        case 0xE01D: return 0x3E; // RCtrl
        case 0x38: return 0x3A;   // LAlt -> Option
        case 0xE038: return 0x3D; // RAlt -> RightOption
        case 0xE05B: return 0x37; // LMeta -> Command
        case 0xE05C: return 0x36; // RMeta -> RightCommand
        case 0x3A: return 0x39;   // CapsLock

        case 0x3B: return 0x7A;   // F1
        case 0x3C: return 0x78;   // F2
        case 0x3D: return 0x63;   // F3
        case 0x3E: return 0x76;   // F4
        case 0x3F: return 0x60;   // F5
        case 0x40: return 0x61;   // F6
        case 0x41: return 0x62;   // F7
        case 0x42: return 0x64;   // F8
        case 0x43: return 0x65;   // F9
        case 0x44: return 0x6D;   // F10
        case 0x57: return 0x67;   // F11
        case 0x58: return 0x6F;   // F12
        case 0x64: return 0x69;   // F13
        case 0x65: return 0x6B;   // F14
        case 0x66: return 0x71;   // F15
        case 0x67: return 0x6A;   // F16
        case 0x68: return 0x40;   // F17
        case 0x69: return 0x4F;   // F18
        case 0x6A: return 0x50;   // F19
        case 0x6B: return 0x5A;   // F20

        case 0x52: return 0x52;   // Keypad 0
        case 0x4F: return 0x53;   // Keypad 1
        case 0x50: return 0x54;   // Keypad 2
        case 0x51: return 0x55;   // Keypad 3
        case 0x4B: return 0x56;   // Keypad 4
        case 0x4C: return 0x57;   // Keypad 5
        case 0x4D: return 0x58;   // Keypad 6
        case 0x47: return 0x59;   // Keypad 7
        case 0x48: return 0x5B;   // Keypad 8
        case 0x49: return 0x5C;   // Keypad 9
        case 0x53: return 0x41;   // Keypad decimal
        case 0x37: return 0x43;   // Keypad multiply
        case 0x4E: return 0x45;   // Keypad plus
        case 0x4A: return 0x4E;   // Keypad minus
        case 0xE035: return 0x4B; // Keypad divide
        case 0xE01C: return 0x4C; // Keypad Enter
        case 0x45: return 0x47;   // NumLock -> Clear

        case 0xE030: return 0x48; // VolumeUp
        case 0xE02E: return 0x49; // VolumeDown
        case 0xE020: return 0x4A; // Mute

        case 0x7D: return 0x5D;   // Yen
        case 0x73: return 0x5E;   // KEY_RO
        case 0x7E: return 0x5F;   // Keypad comma
        case 0x7B: return 0x66;   // muhenkan -> Eisu
        case 0x79: return 0x68;   // henkan -> Kana

        // macOS has no key for ScrollLock (0x46), PrintScreen (0xE037), the
        // menu key (0xE05D) or KEY_KATAKANAHIRAGANA (0x70).
        default:   return -1;
    }
}

} // namespace vivora::protocol
