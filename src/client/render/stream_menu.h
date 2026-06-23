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

// Snapshot fed to the menu header/info block each second (VIV-74).
struct MenuInfo {
    float    rtt_ms          = 0.0f;
    QString  transport;            // "P2P" / "Relay"
    uint32_t width           = 0;
    uint32_t height          = 0;
    uint32_t hz              = 0;   // stream framerate target
    QString  codec;                // "HEVC" / "H.264"
    QString  decoder;              // backend, e.g. "MF HW"
    uint32_t session_seconds = 0;   // uptime for the header pill
    bool     connected       = false;
};

// In-stream control panel (VIV-74).  Summoned over the live video with a
// hotkey (Ctrl+F1) so the user can adjust audio, toggle view-only, go
// fullscreen, or disconnect without leaving the stream.
//
// Top-level frameless window (not a child of the stream view) for the same
// reason the diagnostics HUD is: the Windows StreamWindow uses
// WA_PaintOnScreen, so the D3D11 swap chain owns the HWND and Qt's
// compositor is bypassed — ordinary child widgets don't draw on top of it.
// Unlike the HUD this panel is interactive (accepts focus + mouse) and
// closes on click-away (deactivation), Esc, or the hotkey again.
class StreamMenu : public QWidget {
    Q_OBJECT
public:
    explicit StreamMenu(QWidget* parent = nullptr);

    void set_actions(const MenuActions& actions) { actions_ = actions; }

    // App name + peer/device subtitle in the header.  Set once per session.
    void set_header(const QString& app, const QString& peer);

    // Seed the controls without firing the action callbacks.
    void set_initial_state(float volume, bool muted, bool view_only);

    // Live connection info — fed ~once per second.
    void set_info(const MenuInfo& info);

    void open_over(QWidget* anchor);
    void close_menu();

signals:
    void fullscreenToggled();
    void closed();

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void changeEvent(QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private:
    MenuActions  actions_;

    QLabel*      logo_label_     = nullptr;
    QLabel*      app_label_      = nullptr;
    QLabel*      peer_label_     = nullptr;
    QLabel*      timer_label_    = nullptr;

    QLabel*      latency_value_  = nullptr;
    QLabel*      display_value_  = nullptr;
    QLabel*      decoder_value_  = nullptr;

    QSlider*     volume_slider_  = nullptr;
    QLabel*      volume_value_   = nullptr;
    QCheckBox*   mute_check_     = nullptr;
    QCheckBox*   viewonly_check_ = nullptr;

    QPushButton* monitor_btn_    = nullptr;
    QPushButton* fullscreen_btn_ = nullptr;
    QPushButton* disconnect_btn_ = nullptr;

    // Guards the valueChanged/toggled handlers while we seed control state.
    bool suppress_signals_ = false;
};

} // namespace vivora
