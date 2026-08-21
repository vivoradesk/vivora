// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QtGlobal>

namespace vivora::gui {

// Start-at-login backend (VIV-18).  Registers/unregisters the running
// executable so the OS launches Vivora when the user signs in.
//
// Windows: a "Vivora" value under HKCU\Software\Microsoft\Windows\
// CurrentVersion\Run holding the quoted absolute path of vivora.exe.
// The registry — not our ini — is the source of truth: the user can
// remove the entry externally (Task Manager, regedit) and the Settings
// checkbox must follow reality on next launch.
//
// Linux: an XDG autostart entry at $XDG_CONFIG_HOME/autostart/
// dev.vivora.app.desktop.  Same rule as Windows -- the file on disk is the
// source of truth, so removing it externally is reflected in the UI.
//
// macOS (LaunchAgent) is still a stub; there supported() is false and the
// UI keeps the toggle disabled.
class Autostart {
public:
    // True when this build has a real backend for the current OS.
    static bool supported();
    // True when the OS is currently set to launch Vivora at login.
    static bool enabled();
    // Register (on=true) or unregister (on=false) the running executable.
    // Registering overwrites any previous entry, which also self-heals a
    // stale path left behind by a moved/updated install.
    static void setEnabled(bool on);
};

#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
// Stub for platforms with no backend yet (macOS).  autostart_win.cpp and
// autostart_linux.cpp are added per-platform in src/app/CMakeLists.txt.
inline bool Autostart::supported()   { return false; }
inline bool Autostart::enabled()     { return false; }
inline void Autostart::setEnabled(bool) {}
#endif

} // namespace vivora::gui
