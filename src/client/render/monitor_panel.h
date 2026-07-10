#pragma once

#include "common/protocol/monitor_info.h"

#include <QWidget>
#include <cstdint>
#include <functional>
#include <vector>

class QHBoxLayout;
class QLabel;
class QPushButton;

namespace vivora {

// "Remote monitors" panel (VIV-50).  Opened from the in-stream menu's
// "Switch monitor…" action over the live video.  Shows the host's capturable
// displays as proportional thumbnails — the one currently streamed is
// highlighted "· viewing" — and lets the user switch by click or number key
// (1..9).  A Refresh button re-requests the list.
//
// Like StreamMenu it's a top-level frameless Tool window (not a child of the
// stream view) so it draws over the Windows WA_PaintOnScreen D3D surface, and
// closes on click-away (deactivation), Esc, or the menu hotkey.  Colours are
// adapted from the supplied light mock to the dark menu theme for visual
// consistency with StreamMenu.
class MonitorPanel : public QWidget {
    Q_OBJECT
public:
    explicit MonitorPanel(QWidget* parent = nullptr);

    // Invoked with the host display index when the user picks one.
    void set_select_callback(std::function<void(uint32_t)> cb) { on_select_ = std::move(cb); }
    // Invoked when the user hits Refresh (re-request the list from the host).
    void set_refresh_callback(std::function<void()> cb) { on_refresh_ = std::move(cb); }

    // Replace the displayed list (host → client).  Safe to call while open.
    void set_monitors(const std::vector<protocol::MonitorDesc>& monitors);

    void open_over(QWidget* anchor);
    void close_panel();

signals:
    void closed();

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void changeEvent(QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private:
    void rebuild_thumbs();
    // User picked a display (click or number key): fire the callback and
    // optimistically mark it "viewing" so the highlight updates immediately;
    // the host's fresh MonitorList reconciles a moment later.
    void choose(uint32_t index);

    std::vector<protocol::MonitorDesc> monitors_;
    std::function<void(uint32_t)>      on_select_;
    std::function<void()>              on_refresh_;

    // Rebuild the "N · to switch" number-key hint for the current display
    // count (dynamic — 1, 2, 3… displays), hidden when there's nothing to
    // switch between.
    void rebuild_key_hint();

    QLabel*       title_label_  = nullptr;
    QLabel*       count_label_  = nullptr;
    QWidget*      strip_        = nullptr;   // container for thumbnails
    QHBoxLayout*  strip_layout_ = nullptr;
    QPushButton*  refresh_btn_  = nullptr;
    QWidget*      keycap_row_    = nullptr;  // holds the number keycaps + label
    QHBoxLayout*  keycap_layout_ = nullptr;
    QLabel*       switch_hint_   = nullptr;  // "to switch" (hidden for 1 display)
};

} // namespace vivora
