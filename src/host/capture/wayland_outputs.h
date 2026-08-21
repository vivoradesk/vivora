// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#ifdef VIVORA_LINUX

#include <cstdint>
#include <string>
#include <vector>

namespace vivora::host {

// One Wayland output (physical display), from the wl_output/xdg_output
// globals.  Used to populate the VIV-50 monitor list on Linux hosts, where
// xdg-desktop-portal does NOT expose the display list programmatically — the
// portal only offers its own picker.  Enumerating wl_output gives us the list
// to advertise; the actual capture source is still chosen via the portal
// picker (a monitor switch re-invokes it).
struct WaylandOutput {
    int32_t     x = 0, y = 0;          // logical position in the compositor space
    int32_t     width = 0, height = 0; // logical size (post-scale)
    std::string name;                  // connector name, e.g. "DP-1" (may be empty)
    bool        primary = false;       // first/origin output — best-effort
};

// Connect to the Wayland display named by $WAYLAND_DISPLAY, round-trip the
// registry, and return the outputs.  Empty on non-Wayland sessions or any
// failure (the caller then advertises a single display).  Self-contained:
// opens and closes its own wl_display so it never disturbs Qt/portal state.
std::vector<WaylandOutput> enumerate_wayland_outputs();

} // namespace vivora::host

#endif // VIVORA_LINUX
