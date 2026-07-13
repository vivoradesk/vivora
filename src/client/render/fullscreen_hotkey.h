#pragma once

#include <QKeyEvent>

namespace vivora {

// Borderless fullscreen toggle hotkey (VIV-20): F11 or Ctrl+Shift+F.
// Shared by the Qt stream views (Windows StreamWindow, Linux QtGlVideoView)
// so both press and release handlers agree on exactly which chords are
// client-local.  Matching events must be swallowed — never forwarded to
// the host input channel.  The macOS view is native Cocoa and mirrors this
// predicate on kVK codes in mac_video_view.mm.
inline bool is_fullscreen_hotkey(const QKeyEvent* e) {
    if (e->key() == Qt::Key_F11) return true;
    if (e->key() == Qt::Key_F
        && (e->modifiers() & Qt::ControlModifier)
        && (e->modifiers() & Qt::ShiftModifier)) return true;
    return false;
}

} // namespace vivora
