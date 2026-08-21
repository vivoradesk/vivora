// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#ifdef VIVORA_LINUX

#include <cstdint>
#include <vector>

namespace vivora::host {

// Pulls the host's real mouse cursor — position *and* shape — straight from
// X11 via XFixesGetCursorImage, bypassing the xdg-desktop-portal cursor
// modes (EMBEDDED isn't composited on some compositor/GPU combos and leaves
// the glyph frozen; METADATA is empty on older portals — VIV-66).  libX11 /
// libXfixes are dlopen'd, so the host still links and runs where X isn't
// present (headless / Wayland) — there available() stays false and the
// caller falls back to the portal cursor.
class X11Cursor {
public:
    struct Shape {
        uint32_t id = 0;
        uint16_t width = 0, height = 0;
        uint16_t hotspot_x = 0, hotspot_y = 0;
        std::vector<uint8_t> bgra;   // width*height*4, premultiplied BGRA
    };

    X11Cursor() = default;
    ~X11Cursor();
    X11Cursor(const X11Cursor&) = delete;
    X11Cursor& operator=(const X11Cursor&) = delete;

    bool init();
    void shutdown();
    bool available() const;

    // Poll the current cursor once (cheap, once per frame).  Fills the
    // normalized position (0..1 of the X screen) + visibility.  When the
    // cursor shape changed since the last poll, also fills `shape` and sets
    // `shape_changed`.  Returns false only when X is unavailable.
    bool poll(float& x_norm, float& y_norm, bool& visible,
              bool& shape_changed, Shape& shape);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace vivora::host

#endif // VIVORA_LINUX
