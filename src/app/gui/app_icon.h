// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QIcon>

namespace vivora::gui {

// Brand icons loaded from the embedded qrc (`/icons/...`).
// Multiple PNG sizes per icon; QIcon picks the best per render
// surface (taskbar, tray, About dialog, Explorer).
//
// Source PNGs live in `icons/win/` and `icons/tray/` at the repo root
// and are baked into the binary via `qml/qml.qrc`.  Replace those PNGs
// (or rebuild from the SVG sources in `icons/`) to change the brand.
QIcon make_app_icon();         // full-colour app icon (window, taskbar, Explorer)
QIcon make_tray_idle_icon();   // blue dot   — connected, no active session
QIcon make_tray_sharing_icon();// green dot  — actively sharing
QIcon make_tray_error_icon();  // red dot    — error state (rarely shown)

} // namespace vivora::gui
