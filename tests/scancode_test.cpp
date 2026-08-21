// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// scancode_test.cpp — the VIV-6 canonical scancode space.
//
// The whole point of sending a physical key instead of a character is that
// both ends agree on what code means which key. That agreement is two lookup
// tables, so this pins them down: round-trip, injectivity (two different keys
// must never collapse onto one code), the extended-key encoding, and the
// handful of keys whose codes are worth naming outright.

#include "common/protocol/scancode.h"

#include <cstdio>
#include <map>

using namespace vivora::protocol;

namespace {

int g_tests_run = 0;
int g_tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++g_tests_run;                                                         \
        if (!(expr)) {                                                         \
            std::fprintf(stderr,                                               \
                         "FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);       \
            ++g_tests_failed;                                                  \
        }                                                                      \
    } while (0)

// Every evdev code that maps must map back to itself, and no two may share a
// set-1 code — a collision would mean two physical keys are indistinguishable
// on the wire.
void test_roundtrip_and_injectivity() {
    printf("  evdev <-> set1 round-trip...\n");
    std::map<uint16_t, uint16_t> seen;   // set1 -> evdev
    int mapped = 0;
    for (uint16_t evdev = 0; evdev < 256; ++evdev) {
        const uint16_t s1 = evdev_to_set1(evdev);
        if (s1 == 0) continue;
        ++mapped;
        CHECK(set1_to_evdev(s1) == evdev);
        const auto it = seen.find(s1);
        if (it != seen.end()) {
            std::fprintf(stderr, "FAIL collision: evdev %u and %u both -> 0x%X\n",
                         it->second, evdev, s1);
            ++g_tests_failed;
        }
        seen[s1] = evdev;
        ++g_tests_run;
    }
    // The PC/AT block plus the extended tail: a full keyboard, not a subset.
    CHECK(mapped >= 100);
}

// evdev 1..83 were assigned to equal the AT set-1 make codes; the code leans
// on that identity instead of carrying 83 redundant table rows, so verify the
// premise rather than trusting it.
void test_identity_block() {
    printf("  identity block 1..83...\n");
    for (uint16_t e = 1; e <= 83; ++e) CHECK(evdev_to_set1(e) == e);
    CHECK(evdev_to_set1(84) == 0);      // gap in the evdev table
    CHECK(evdev_to_set1(85) == 0);      // KEY_ZENKAKUHANKAKU: no set-1 code
    CHECK(evdev_to_set1(86) == 0x56);   // KEY_102ND
    CHECK(evdev_to_set1(87) == 0x57);   // KEY_F11
    CHECK(evdev_to_set1(88) == 0x58);   // KEY_F12
}

// A few codes worth naming: if any of these drift, something that used to type
// stops typing.
void test_known_keys() {
    printf("  known keys...\n");
    CHECK(evdev_to_set1(1)  == 0x01);   // Esc
    CHECK(evdev_to_set1(16) == 0x10);   // Q — the key Cyrillic input used to lose
    CHECK(evdev_to_set1(30) == 0x1E);   // A
    CHECK(evdev_to_set1(28) == 0x1C);   // Enter
    CHECK(evdev_to_set1(57) == 0x39);   // Space
    CHECK(evdev_to_set1(42) == 0x2A);   // Left Shift
    CHECK(evdev_to_set1(54) == 0x36);   // Right Shift
    CHECK(evdev_to_set1(29) == 0x1D);   // Left Ctrl
    CHECK(evdev_to_set1(97) == 0xE01D); // Right Ctrl — extended
    CHECK(evdev_to_set1(56) == 0x38);   // Left Alt
    CHECK(evdev_to_set1(100) == 0xE038);// Right Alt — extended
}

// Left Arrow and Keypad-4 share make code 0x4B; only the 0xE0 prefix tells
// them apart. That is the reason the space is 16-bit at all.
void test_extended_encoding() {
    printf("  extended encoding...\n");
    const uint16_t left  = evdev_to_set1(105);  // KEY_LEFT
    const uint16_t kp4   = evdev_to_set1(75);   // KEY_KP4
    CHECK(left == 0xE04B);
    CHECK(kp4  == 0x4B);
    CHECK(left != kp4);
    CHECK(set1_is_extended(left));
    CHECK(!set1_is_extended(kp4));
    CHECK(set1_make_code(left) == 0x4B);
    CHECK(set1_make_code(kp4)  == 0x4B);
    // Keypad Enter vs Enter, same shape.
    CHECK(evdev_to_set1(96) == 0xE01C);
    CHECK(evdev_to_set1(28) == 0x1C);
    CHECK(set1_is_extended(evdev_to_set1(96)));
    CHECK(!set1_is_extended(evdev_to_set1(28)));
}

// Qt reports keys in the X11 convention on both xcb and Wayland: evdev + 8.
void test_qt_native_offset() {
    printf("  Qt native scan offset...\n");
    CHECK(qt_native_scan_to_set1(9)  == 0x01);   // X11 9  = evdev 1  = Esc
    CHECK(qt_native_scan_to_set1(24) == 0x10);   // X11 24 = evdev 16 = Q
    CHECK(qt_native_scan_to_set1(38) == 0x1E);   // X11 38 = evdev 30 = A
    CHECK(qt_native_scan_to_set1(108) == 0xE038);// X11 108 = evdev 100 = Right Alt
    CHECK(qt_native_scan_to_set1(113) == 0xE04B);// X11 113 = evdev 105 = Left Arrow
    // Below 9 is not a key in the X11 numbering; must not wrap into the table.
    for (uint32_t n = 0; n < 9; ++n) CHECK(qt_native_scan_to_set1(n) == 0);
}

// Windows and macOS viewers hand out bare make codes; they must reconstruct
// the 0xE0 prefix from the virtual key before the code goes on the wire, or a
// Linux host presses the wrong physical key.
void test_win_scan_canonicalisation() {
    printf("  Windows make code -> canonical set1...\n");
    // Left Arrow (VK_LEFT) and Keypad-4 both arrive as make code 0x4B.
    CHECK(win_scan_to_set1(0x4B, 0x25) == 0xE04B);   // VK_LEFT   -> extended
    CHECK(win_scan_to_set1(0x4B, 0x64) == 0x4B);     // VK_NUMPAD4 -> plain
    CHECK(set1_to_evdev(win_scan_to_set1(0x4B, 0x25)) == 105);  // KEY_LEFT
    CHECK(set1_to_evdev(win_scan_to_set1(0x4B, 0x64)) == 75);   // KEY_KP4

    CHECK(win_scan_to_set1(0x1D, 0xA3) == 0xE01D);   // right Ctrl
    CHECK(win_scan_to_set1(0x1D, 0x11) == 0x1D);     // VK_CONTROL -> left Ctrl
    CHECK(win_scan_to_set1(0x38, 0xA5) == 0xE038);   // right Alt
    CHECK(win_scan_to_set1(0x38, 0x12) == 0x38);     // VK_MENU    -> left Alt
    CHECK(win_scan_to_set1(0x1E, 0x41) == 0x1E);     // A: never extended

    // Idempotent on something already canonical, and honest about nothing.
    CHECK(win_scan_to_set1(0xE04B, 0x25) == 0xE04B);
    CHECK(win_scan_to_set1(0, 0x25) == 0);
}

// Anything unmapped has to say so, so callers fall back to vk_code rather than
// pressing an arbitrary key.
void test_unmapped_is_zero() {
    printf("  unmapped codes...\n");
    CHECK(evdev_to_set1(0) == 0);
    CHECK(evdev_to_set1(119) == 0);     // KEY_PAUSE: 0xE1 1D 45, no single code
    CHECK(evdev_to_set1(200) == 0);
    CHECK(set1_to_evdev(0) == 0);
    CHECK(set1_to_evdev(0x59) == 0);
    CHECK(set1_to_evdev(0xE0FF) == 0);
}

// Every mac keycode that maps must map back to itself, and no two may share a
// set-1 code. This is the only test macOS input gets — the platform has no CI
// and the tables are exact inverses by hand, which is exactly the kind of
// thing that rots silently.
void test_mac_roundtrip_and_injectivity() {
    printf("  mac kVK <-> set1 round-trip...\n");
    std::map<uint16_t, uint16_t> seen;   // set1 -> mac kc
    int mapped = 0;
    for (uint16_t kc = 0; kc < 256; ++kc) {
        const uint16_t s1 = mac_kc_to_set1(kc);
        if (s1 == 0) continue;
        ++mapped;
        CHECK(set1_to_mac_kc(s1) == static_cast<int>(kc));
        const auto it = seen.find(s1);
        if (it != seen.end()) {
            std::fprintf(stderr, "FAIL collision: mac kc 0x%X and 0x%X both -> 0x%X\n",
                         it->second, kc, s1);
            ++g_tests_failed;
        }
        seen[s1] = kc;
        ++g_tests_run;
    }
    // A full Apple keyboard: alphanumerics, both modifier pairs, the arrow
    // cluster, F1..F20, the whole keypad and the JIS extras.
    CHECK(mapped >= 110);

    // And the other direction: anything set1_to_mac_kc claims must come back.
    for (uint32_t s1 = 1; s1 < 0x10000; ++s1) {
        const int kc = set1_to_mac_kc(static_cast<uint16_t>(s1));
        if (kc < 0) continue;
        CHECK(mac_kc_to_set1(static_cast<uint16_t>(kc)) == s1);
    }
}

// A mac viewer and a Linux host have no table in common — they meet in set 1.
// These are the keys where getting it wrong is silent: the letter still types,
// just the wrong one.
void test_mac_to_evdev_bridge() {
    printf("  mac kVK -> evdev, end to end...\n");
    CHECK(set1_to_evdev(mac_kc_to_set1(0x00)) == 30);   // A
    CHECK(set1_to_evdev(mac_kc_to_set1(0x0C)) == 16);   // Q
    CHECK(set1_to_evdev(mac_kc_to_set1(0x24)) == 28);   // Return -> KEY_ENTER
    CHECK(set1_to_evdev(mac_kc_to_set1(0x31)) == 57);   // Space
    CHECK(set1_to_evdev(mac_kc_to_set1(0x33)) == 14);   // Delete -> KEY_BACKSPACE
    CHECK(set1_to_evdev(mac_kc_to_set1(0x75)) == 111);  // ForwardDelete -> KEY_DELETE
    CHECK(set1_to_evdev(mac_kc_to_set1(0x7B)) == 105);  // Left arrow
    CHECK(set1_to_evdev(mac_kc_to_set1(0x37)) == 125);  // Command -> KEY_LEFTMETA
    CHECK(set1_to_evdev(mac_kc_to_set1(0x3A)) == 56);   // Option  -> KEY_LEFTALT
    CHECK(set1_to_evdev(mac_kc_to_set1(0x3D)) == 100);  // RightOption -> KEY_RIGHTALT
    CHECK(set1_to_evdev(mac_kc_to_set1(0x3E)) == 97);   // RightControl

    // ...and back, which is the direction VIV-123 was actually about.
    CHECK(set1_to_mac_kc(evdev_to_set1(30))  == 0x00);  // A
    CHECK(set1_to_mac_kc(evdev_to_set1(16))  == 0x0C);  // Q — the Cyrillic case
    CHECK(set1_to_mac_kc(evdev_to_set1(105)) == 0x7B);  // Left arrow
    CHECK(set1_to_mac_kc(evdev_to_set1(100)) == 0x3D);  // Right Alt
    CHECK(set1_to_mac_kc(evdev_to_set1(96))  == 0x4C);  // Keypad Enter
}

// The arrow-versus-keypad split has to survive the mac tables too, in both
// directions — mac numbers the two clusters separately, set 1 does not.
void test_mac_extended_split() {
    printf("  mac extended split...\n");
    CHECK(mac_kc_to_set1(0x7B) == 0xE04B);   // Left arrow
    CHECK(mac_kc_to_set1(0x56) == 0x4B);     // Keypad 4
    CHECK(mac_kc_to_set1(0x4C) == 0xE01C);   // Keypad Enter
    CHECK(mac_kc_to_set1(0x24) == 0x1C);     // Return
    CHECK(mac_kc_to_set1(0x4B) == 0xE035);   // Keypad /
    CHECK(set1_to_mac_kc(0xE04B) == 0x7B);
    CHECK(set1_to_mac_kc(0x4B)   == 0x56);
}

// A Windows viewer's bare make code has to survive canonicalisation and land
// on the right mac key — this is the full Windows -> macOS host path.
void test_win_to_mac_bridge() {
    printf("  Windows make code -> mac kVK...\n");
    CHECK(set1_to_mac_kc(win_scan_to_set1(0x4B, 0x25)) == 0x7B);  // VK_LEFT -> Left
    CHECK(set1_to_mac_kc(win_scan_to_set1(0x4B, 0x64)) == 0x56);  // VK_NUMPAD4 -> Keypad 4
    CHECK(set1_to_mac_kc(win_scan_to_set1(0x1D, 0xA3)) == 0x3E);  // right Ctrl
    CHECK(set1_to_mac_kc(win_scan_to_set1(0x1D, 0x11)) == 0x3B);  // left Ctrl
    CHECK(set1_to_mac_kc(win_scan_to_set1(0x5B, 0x5B)) == 0x37);  // LWin -> Command
    CHECK(set1_to_mac_kc(win_scan_to_set1(0x1E, 0x41)) == 0x00);  // A
}

// Unmappable must say -1, not 0 — 0 is a real mac keycode (the A key), so a
// caller that treats "unknown" as 0 would type an A for every key macOS has
// no equivalent for.
void test_mac_unmapped() {
    printf("  mac unmapped codes...\n");
    CHECK(set1_to_mac_kc(0) == -1);
    CHECK(set1_to_mac_kc(0x46) == -1);      // ScrollLock
    CHECK(set1_to_mac_kc(0xE037) == -1);    // PrintScreen
    CHECK(set1_to_mac_kc(0xE05D) == -1);    // menu key
    CHECK(set1_to_mac_kc(0x70) == -1);      // KEY_KATAKANAHIRAGANA
    CHECK(set1_to_mac_kc(0xE0FF) == -1);
    CHECK(mac_kc_to_set1(0x3F) == 0);       // fn — handled locally by macOS
    CHECK(mac_kc_to_set1(0x51) == 0);       // keypad equals — no PC key
    CHECK(mac_kc_to_set1(0xFF) == 0);
    // 0x00 is A, and must not be confused with "nothing".
    CHECK(mac_kc_to_set1(0x00) == 0x1E);
}

} // namespace

int main() {
    printf("scancode_test:\n");
    test_roundtrip_and_injectivity();
    test_identity_block();
    test_known_keys();
    test_extended_encoding();
    test_qt_native_offset();
    test_win_scan_canonicalisation();
    test_unmapped_is_zero();
    test_mac_roundtrip_and_injectivity();
    test_mac_to_evdev_bridge();
    test_mac_extended_split();
    test_win_to_mac_bridge();
    test_mac_unmapped();
    printf("scancode_test: %d checks, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
