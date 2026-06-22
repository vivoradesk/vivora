#pragma once

#include "app/view_platform.h"

#include <QString>
#include <QWidget>
#include <cstdint>

class QCheckBox;
class QLabel;
class QPushButton;
class QSlider;

namespace vivora {

// In-stream control panel (VIV-74).  Summoned over the live video with a
// hotkey (Ctrl+F1) so the user can adjust audio, toggle view-only, go
// fullscreen, or disconnect without leaving the stream.
//
// It is a top-level frameless window (not a child of the stream view) for
// the same reason the diagnostics HUD is: the Windows StreamWindow uses
// WA_PaintOnScreen, so the D3D11 swap chain owns the HWND and Qt's
// compositor is bypassed — ordinary child widgets don't draw on top of it.
// Unlike the HUD this panel is interactive, so it accepts focus and mouse
// events (no WA_TransparentForMouseEvents) and closes on click-away
// (deactivation), Esc, or the hotkey again.
//
// The widget itself is plain Qt and platform-agnostic; only its hosting
// (StreamWindow) is Windows-specific today.  Linux/macOS can reuse it once
// their stream views grow a menu hook.
class StreamMenu : public QWidget {
    Q_OBJECT
public:
    explicit StreamMenu(QWidget* parent = nullptr);

    // Callbacks for the actual work (audio/view-only/disconnect) — supplied
    // by the view loop via the platform.  Fullscreen is a pure window
    // concern, surfaced as a signal instead (see below).
    void set_actions(const MenuActions& actions) { actions_ = actions; }

    // Seed the controls without firing the action callbacks (so opening the
    // menu doesn't echo the current state back to the session).
    void set_initial_state(float volume, bool muted, bool view_only);

    // Live connection info for the header — fed from the same StatsView the
    // HUD uses, ~once per second.
    void set_info(float rtt_ms, uint32_t width, uint32_t height,
                  const QString& decoder);

    // Show centred over `anchor`, raised and activated so it receives
    // keyboard/mouse.  Re-seeds the info line from the last snapshot.
    void open_over(QWidget* anchor);
    void close_menu();

signals:
    // The host window toggles fullscreen — the menu doesn't own the window.
    void fullscreenToggled();
    // Emitted whenever the menu hides (Esc / click-away / hotkey / button)
    // so the host can re-focus the stream and resume relative-mouse mode.
    void closed();

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void changeEvent(QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private:
    MenuActions  actions_;
    QLabel*      info_label_     = nullptr;
    QSlider*     volume_slider_  = nullptr;
    QLabel*      volume_value_   = nullptr;
    QCheckBox*   mute_check_     = nullptr;
    QCheckBox*   viewonly_check_ = nullptr;
    QPushButton* fullscreen_btn_ = nullptr;
    QPushButton* disconnect_btn_ = nullptr;

    // Guards the valueChanged/toggled handlers while we programmatically set
    // control state in set_initial_state(), so seeding doesn't fire actions.
    bool suppress_signals_ = false;
};

} // namespace vivora
