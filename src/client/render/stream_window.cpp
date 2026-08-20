#ifdef VIVORA_WINDOWS

#include "client/render/stream_window.h"

#include "common/protocol/scancode.h"
#include "client/render/fullscreen_hotkey.h"
#include "common/utils/log.h"
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QResizeEvent>
#include <QScreen>
#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace vivora {

namespace {
// QLabel subclass that paints a translucent rounded rect under the text.
// stylesheet's rgba background isn't honoured on top-level QLabel windows
// when WA_TranslucentBackground is set — we have to draw the background
// ourselves before the text pass.
class HudLabel : public QLabel {
public:
    using QLabel::QLabel;
protected:
    void paintEvent(QPaintEvent* e) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 110));
        p.drawRoundedRect(rect(), 6, 6);
        p.end();
        QLabel::paintEvent(e);
    }
};
} // namespace

StreamWindow::StreamWindow(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(640, 360);
    // Hide the local OS cursor over the stream — either the host's
    // cursor packets paint a custom one (Windows / macOS hosts that
    // sync cursors), or it's already baked into the frame by the
    // capture API (Linux PipeWire with cursor_mode=embedded). Either
    // way the user sees exactly one cursor. The view layer guards
    // update_cursor_position() with session.has_cursor_position(), so
    // this BlankCursor default no longer fights with the
    // default-constructed (visible=false) message.
    setCursor(Qt::BlankCursor);

    // Diagnostics overlay — top-level frameless tool window with translucent
    // background.  A child QLabel won't render here because the StreamWindow
    // uses WA_PaintOnScreen (the D3D11 swap chain owns the HWND and Qt's
    // compositor is bypassed).  An independent overlay window is the only
    // approach that draws reliably on top of the D3D surface.
    hud_label_ = new HudLabel(nullptr);
    hud_label_->setWindowFlags(Qt::FramelessWindowHint
                             | Qt::Tool
                             | Qt::WindowStaysOnTopHint
                             | Qt::WindowDoesNotAcceptFocus
                             | Qt::WindowTransparentForInput);
    hud_label_->setAttribute(Qt::WA_TranslucentBackground);
    hud_label_->setAttribute(Qt::WA_ShowWithoutActivating);
    hud_label_->setAttribute(Qt::WA_TransparentForMouseEvents);
    hud_label_->setStyleSheet(
        "QLabel {"
        "  color: rgb(230, 230, 230);"
        "  font-family: 'Consolas', 'DejaVu Sans Mono', monospace;"
        "  font-size: 12px;"
        "  padding: 8px;"
        "}");
    hud_label_->setText("HUD ready (F9)");
    hud_label_->adjustSize();

    // Status overlay (connecting / waiting-for-approval / close reason).
    // Same top-level-overlay trick as the HUD, centred, larger font.
    status_label_ = new HudLabel(nullptr);
    status_label_->setWindowFlags(Qt::FramelessWindowHint
                                | Qt::Tool
                                | Qt::WindowStaysOnTopHint
                                | Qt::WindowDoesNotAcceptFocus
                                | Qt::WindowTransparentForInput);
    status_label_->setAttribute(Qt::WA_TranslucentBackground);
    status_label_->setAttribute(Qt::WA_ShowWithoutActivating);
    status_label_->setAttribute(Qt::WA_TransparentForMouseEvents);
    status_label_->setAlignment(Qt::AlignCenter);
    status_label_->setStyleSheet(
        "QLabel {"
        "  color: rgb(240, 240, 240);"
        "  font-family: 'Segoe UI', 'DejaVu Sans', sans-serif;"
        "  font-size: 15px;"
        "  padding: 14px 22px;"
        "}");
    status_label_->hide();

    // In-stream control menu (VIV-74).  Created hidden; summoned by Ctrl+F1.
    // Top-level (parent nullptr) for the same WA_PaintOnScreen reason as the
    // overlays above.
    menu_ = new StreamMenu(nullptr);
    menu_->hide();
    connect(menu_, &StreamMenu::fullscreenToggled, this, &StreamWindow::toggle_fullscreen);
    connect(menu_, &StreamMenu::keepAspectToggled, this, [this](bool keep) {
        keep_aspect_ = keep;
        renderer_.set_keep_aspect(keep);
    });
    connect(menu_, &StreamMenu::closed, this, [this]() {
        // Skip when we're deliberately handing focus to the monitor panel —
        // otherwise the panel would lose activation and dismiss itself.
        if (suppress_menu_refocus_) return;
        // Return focus to the stream so input resumes (and relative-mouse
        // mode re-enters if the host has its cursor hidden).
        activateWindow();
        setFocus(Qt::OtherFocusReason);
    });

    // "Switch monitor…" panel (VIV-50).  Owned top-level overlay like the menu.
    monitor_panel_ = new MonitorPanel(nullptr);
    monitor_panel_->hide();
    connect(monitor_panel_, &MonitorPanel::closed, this, [this]() {
        activateWindow();
        setFocus(Qt::OtherFocusReason);
    });
    connect(menu_, &StreamMenu::monitorClicked, this, [this]() {
        // Ask the host for a fresh display list, then hand off from the menu
        // to the panel without bouncing focus back to the stream.
        if (menu_actions_.request_monitors) menu_actions_.request_monitors();
        suppress_menu_refocus_ = true;
        menu_->close_menu();
        suppress_menu_refocus_ = false;
        monitor_panel_->set_monitors(last_monitors_);
        monitor_panel_->open_over(this);
    });
}

StreamWindow::~StreamWindow() {
    if (hud_label_) { hud_label_->hide(); hud_label_->deleteLater(); hud_label_ = nullptr; }
    if (status_label_) { status_label_->hide(); status_label_->deleteLater(); status_label_ = nullptr; }
    if (menu_) { menu_->hide(); menu_->deleteLater(); menu_ = nullptr; }
    if (monitor_panel_) { monitor_panel_->hide(); monitor_panel_->deleteLater(); monitor_panel_ = nullptr; }
}

void StreamWindow::set_menu_actions(const MenuActions& actions) {
    menu_actions_ = actions;
    if (menu_) {
        menu_->set_actions(actions);
        // Seed controls to the session defaults (unity volume, not muted, input
        // forwarding on) without echoing them back through the callbacks.
        menu_->set_initial_state(1.0f, false, false, keep_aspect_);
        menu_->set_initial_caps(actions.initial_fps_cap,
                                actions.initial_bitrate_cap_kbps);
    }
    // Wire the panel's switch/refresh to the same session callbacks (VIV-50).
    if (monitor_panel_) {
        monitor_panel_->set_select_callback([this](uint32_t idx) {
            if (menu_actions_.select_monitor) menu_actions_.select_monitor(idx);
        });
        monitor_panel_->set_refresh_callback([this]() {
            if (menu_actions_.request_monitors) menu_actions_.request_monitors();
        });
    }
}

void StreamWindow::set_monitor_list(const std::vector<protocol::MonitorDesc>& monitors) {
    last_monitors_ = monitors;
    if (monitor_panel_) monitor_panel_->set_monitors(monitors);
}

void StreamWindow::set_peer_label(const QString& peer) {
    if (menu_) menu_->set_header("Vivora", peer);
}

void StreamWindow::toggle_menu() {
    if (!menu_) return;
    if (menu_->isVisible()) {
        menu_->close_menu();
    } else {
        feed_menu_info();
        menu_->open_over(this);
    }
}

void StreamWindow::toggle_fullscreen() {
    if (isFullScreen()) {
        // Restore the exact pre-fullscreen state.  showNormal() alone is
        // usually enough, but explicitly re-applying the saved geometry
        // guarantees position+size come back on every WM (VIV-20).
        if (was_maximized_before_fullscreen_) {
            showMaximized();
        } else {
            showNormal();
            if (saved_normal_geometry_.isValid())
                setGeometry(saved_normal_geometry_);
        }
    } else {
        was_maximized_before_fullscreen_ = isMaximized();
        // normalGeometry() is the non-maximized geometry even while
        // maximized, so a maximized→fullscreen→maximized→restore chain
        // still lands on the original floating rect.
        saved_normal_geometry_ = normalGeometry();
        showFullScreen();
    }
}

void StreamWindow::feed_menu_info() {
    if (!menu_) return;
    MenuInfo mi;
    mi.rtt_ms          = last_stats_.rtt_ms;
    mi.transport       = QString::fromUtf8(last_stats_.transport);
    mi.width           = last_stats_.width;
    mi.height          = last_stats_.height;
    mi.hz              = last_stats_.target_fps;
    mi.codec           = QString::fromUtf8(last_stats_.codec);
    mi.decoder         = QString::fromUtf8(last_stats_.decoder);
    mi.session_seconds = last_stats_.session_seconds;
    mi.connected       = last_stats_.width > 0;
    menu_->set_info(mi);
}

void StreamWindow::set_status(const QString& text) {
    if (!status_label_) return;
    if (text.isEmpty()) {
        // Clear the text too, not just hide: the activation / show / move
        // handlers use !text().isEmpty() as the "should the overlay be
        // visible?" proxy, so leaving stale text here made the overlay
        // pop back up on the next alt-tab / window activation even though
        // frames were flowing (VIV-62 follow-up).
        status_label_->clear();
        status_label_->hide();
        return;
    }
    if (status_label_->text() != text) {
        status_label_->setText(text);
        status_label_->adjustSize();
    }
    position_status();
    // Don't float the overlay over other apps when we're not focused.
    if (isActiveWindow() && !(windowState() & Qt::WindowMinimized)) {
        status_label_->show();
        status_label_->raise();
    }
}

void StreamWindow::position_status() {
    if (!status_label_) return;
    const QPoint c = mapToGlobal(QPoint(width() / 2, height() / 2));
    status_label_->move(c.x() - status_label_->width() / 2,
                        c.y() - status_label_->height() / 2);
}

void StreamWindow::upload_cursor_shape(const protocol::CursorShapeMessage& shape) {
    // BGRA → QImage(Format_ARGB32) — DXGI gives us B,G,R,A in memory, which
    // matches Qt's Format_ARGB32 byte layout on little-endian. Copying via
    // a temporary is simpler than trying to wrap raw memory (Qt may require
    // the buffer to stay alive and 32-bit aligned per scanline).
    const uint8_t* src = shape.bgra.data();
    const size_t expected = static_cast<size_t>(shape.width) * shape.height * 4u;
    if (shape.width == 0 || shape.height == 0 || shape.bgra.size() < expected) return;

    QImage img(src, shape.width, shape.height,
               static_cast<int>(shape.width) * 4,
               QImage::Format_ARGB32);
    // Deep copy so the QImage owns its bytes after `shape` goes out of scope.
    QImage owned = img.copy();
    QPixmap pix = QPixmap::fromImage(std::move(owned));
    QCursor cur(pix, static_cast<int>(shape.hotspot_x), static_cast<int>(shape.hotspot_y));
    cursor_cache_[shape.shape_id] = std::move(cur);

    // If this is the shape the host is currently displaying, apply it now.
    if (have_active_shape_ && shape.shape_id == active_shape_id_) {
        refresh_cursor();
    }
}

void StreamWindow::update_cursor_position(const protocol::CursorPositionMessage& pos) {
    // Position (pos.x_norm / pos.y_norm) is intentionally unused: the local
    // OS already places the cursor at the user's mouse position in absolute
    // mode, and in relative mode the host tracks its own cursor from our
    // deltas — it does not need to feed position back.
    const bool shape_changed = !have_active_shape_ || pos.shape_id != active_shape_id_;
    active_shape_id_   = pos.shape_id;
    have_active_shape_ = true;

    if (pos.visible != host_cursor_visible_) {
        // Visibility flicker debounce — some games toggle cursor visibility
        // on HUD interactions / frame-by-frame animations.  Wait for a stable
        // signal before flipping modes, otherwise we keep entering/exiting
        // relative mode and the saved_pos restore produces visible jumps.
        //
        // Important: only (re)start the timer when the *pending target*
        // actually changes.  The host sends cursor messages many times per
        // second — restarting the timer on every message would reset it
        // forever and the flip would never fire.
        if (!visibility_debounce_) {
            visibility_debounce_ = new QTimer(this);
            visibility_debounce_->setSingleShot(true);
            connect(visibility_debounce_, &QTimer::timeout,
                    this, &StreamWindow::apply_pending_visibility);
        }
        if (!pending_visibility_valid_ || pending_visibility_ != pos.visible) {
            pending_visibility_ = pos.visible;
            pending_visibility_valid_ = true;
            visibility_debounce_->start(50);
        }
    } else if (pending_visibility_valid_ && pending_visibility_ != pos.visible) {
        // Visibility reverted before debounce fired — cancel pending flip.
        pending_visibility_valid_ = false;
    }

    if (shape_changed) refresh_cursor();
}

void StreamWindow::apply_pending_visibility() {
    if (!pending_visibility_valid_) return;
    pending_visibility_valid_ = false;
    if (pending_visibility_ == host_cursor_visible_) return;
    host_cursor_visible_ = pending_visibility_;
    if (!host_cursor_visible_) enter_relative_mode();
    else                       exit_relative_mode();
    refresh_cursor();
}

void StreamWindow::enter_relative_mode() {
    if (relative_mode_) return;
    // Loopback session: the "host cursor" IS the user's physical mouse.
    // Hiding + clipping it here trapped the real pointer invisibly inside
    // the window whenever the host cursor was elsewhere (e.g. on another
    // display after a monitor switch, VIV-50).  Relative mode exists for
    // remote games that hide the pointer — meaningless against yourself.
    if (loopback_) return;
    relative_mode_ = true;
    saved_global_pos_ = QCursor::pos();

    // Register for raw-input mouse (WM_INPUT) — this gives us hardware
    // deltas regardless of whether the OS cursor is clamped at the
    // ClipCursor edge.  Avoids the warp-back race entirely: we never
    // compute deltas from window positions, we just forward the raw HID
    // motion to the host.
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;            // Generic desktop
    rid.usUsage     = 0x02;            // Mouse
    rid.dwFlags     = 0;               // Foreground only
    rid.hwndTarget  = reinterpret_cast<HWND>(winId());
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        raw_input_registered_ = true;
    } else {
        log::error("CURSOR", "RegisterRawInputDevices failed (%lu)", GetLastError());
    }

    grabMouse();
    update_clip_rect();
    log::info("CURSOR", "enter relative mode (saved pos %d,%d)",
              saved_global_pos_.x(), saved_global_pos_.y());
}

void StreamWindow::exit_relative_mode() {
    if (!relative_mode_) return;
    relative_mode_ = false;

    if (raw_input_registered_) {
        RAWINPUTDEVICE rid = {};
        rid.usUsagePage = 0x01;
        rid.usUsage     = 0x02;
        rid.dwFlags     = RIDEV_REMOVE;
        rid.hwndTarget  = nullptr;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        raw_input_registered_ = false;
    }

    ClipCursor(nullptr);
    releaseMouse();
    QCursor::setPos(saved_global_pos_);
    log::info("CURSOR", "exit relative mode (restored pos %d,%d)",
              saved_global_pos_.x(), saved_global_pos_.y());
}

void StreamWindow::update_clip_rect() {
    if (!relative_mode_) return;
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) return;
    RECT cr;
    if (!GetClientRect(hwnd, &cr)) return;
    POINT tl = { cr.left,  cr.top    };
    POINT br = { cr.right, cr.bottom };
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &br);
    RECT clip = { tl.x, tl.y, br.x, br.y };
    ClipCursor(&clip);
}

void StreamWindow::refresh_cursor() {
    if (!host_cursor_visible_) {
        setCursor(Qt::BlankCursor);
        return;
    }
    auto it = cursor_cache_.find(active_shape_id_);
    if (it != cursor_cache_.end()) {
        log::debug("CURSOR", "apply host shape %u (%zu cached)",
                   active_shape_id_, cursor_cache_.size());
        setCursor(it->second);
        have_applied_shape_ = true;
        return;
    }
    // The shape has not arrived yet.  That is normal for a moment: the host
    // announces a new id in the position message it sends every frame, and
    // the bitmap follows in the next one.
    //
    // Hold whatever we are already showing rather than snapping back to the
    // local arrow.  Falling back on every miss made the cursor flicker
    // between the host's shape and ours, and when the host churned ids
    // faster than a bitmap could cross the wire it meant the host's cursor
    // was never seen at all.
    if (have_applied_shape_) {
        log::debug("CURSOR", "shape %u not here yet - holding previous",
                   active_shape_id_);
        return;
    }
    // Nothing has ever arrived — a plain arrow beats an invisible cursor.
    log::debug("CURSOR", "no host shape yet - local arrow");
    setCursor(Qt::ArrowCursor);
}

bool StreamWindow::init_renderer(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) {
        log::error("RENDER", "No HWND available");
        return false;
    }

    // Size the window to match host resolution if it fits on screen,
    // otherwise keep current window size. The swap chain uses the actual
    // client area size, and the Video Processor scales frame -> window.
    QSize screen_size = screen() ? screen()->availableSize() : QSize(1920, 1080);
    uint32_t target_w = std::min<uint32_t>(width, screen_size.width() * 9 / 10);
    uint32_t target_h = std::min<uint32_t>(height, screen_size.height() * 9 / 10);
    // Preserve aspect ratio
    double frame_aspect = static_cast<double>(width) / height;
    if (target_w / frame_aspect <= target_h) {
        target_h = static_cast<uint32_t>(target_w / frame_aspect);
    } else {
        target_w = static_cast<uint32_t>(target_h * frame_aspect);
    }
    resize(target_w, target_h);

    // Re-centre on the current screen after the resize.  The window
    // was created at the small placeholder size and Qt's default
    // position (~screen origin); after we grow to fit the host
    // resolution, half the window often hangs off the right/bottom
    // edge.  Place it so the resized rect fits within availableGeometry.
    if (screen()) {
        const QRect avail = screen()->availableGeometry();
        const int x = avail.x() + (avail.width()  - static_cast<int>(target_w)) / 2;
        const int y = avail.y() + (avail.height() - static_cast<int>(target_h)) / 2;
        move(qMax(avail.x(), x), qMax(avail.y(), y));
    }

    // Swap chain needs PHYSICAL pixels, not Qt logical pixels.
    // On high-DPI monitors devicePixelRatio() > 1.
    qreal dpr = devicePixelRatio();
    uint32_t client_w = static_cast<uint32_t>(this->width() * dpr);
    uint32_t client_h = static_cast<uint32_t>(this->height() * dpr);
    initialized_ = renderer_.init(device, hwnd, width, height, client_w, client_h, format);
    return initialized_;
}

bool StreamWindow::render_frame(ID3D11Texture2D* texture, uint32_t subresource) {
    if (!initialized_) return false;
    return renderer_.render(texture, subresource);
}

void StreamWindow::set_stream_size(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return;
    host_w_ = w;
    host_h_ = h;
    if (initialized_) {
        renderer_.set_crop(w, h);
    }
}

void StreamWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (initialized_) {
        // Convert Qt logical pixels to physical pixels for the swap chain.
        qreal dpr = devicePixelRatio();
        uint32_t phys_w = static_cast<uint32_t>(event->size().width() * dpr);
        uint32_t phys_h = static_cast<uint32_t>(event->size().height() * dpr);
        renderer_.resize(phys_w, phys_h);
        // Re-render last frame at new size so the window isn't blank/stale
        // until the next stream frame arrives.
        renderer_.re_present();
    }
    update_clip_rect();
    if (hud_visible_) position_hud();
    if (status_label_ && !status_label_->text().isEmpty()) position_status();
}

bool StreamWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (relative_mode_ && eventType == "windows_generic_MSG") {
        MSG* msg = static_cast<MSG*>(message);
        if (msg->message == WM_INPUT) {
            UINT size = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam),
                            RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
            if (size > 0 && size <= sizeof(RAWINPUT) + 64) {
                alignas(RAWINPUT) BYTE buf[sizeof(RAWINPUT) + 64];
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam),
                                    RID_INPUT, buf, &size,
                                    sizeof(RAWINPUTHEADER)) == size) {
                    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buf);
                    if (raw->header.dwType == RIM_TYPEMOUSE) {
                        const auto& m = raw->data.mouse;
                        // MOUSE_MOVE_ABSOLUTE is set by remote-desktop / some
                        // virtualisation drivers.  Skip those — we only want
                        // raw hardware deltas.
                        if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 &&
                            (m.lLastX != 0 || m.lLastY != 0)) {
                            protocol::InputEvent ev;
                            ev.type = protocol::InputEventType::MouseMoveRelative;
                            ev.dx = m.lLastX;
                            ev.dy = m.lLastY;
                            send_event(ev);
                        }
                    }
                }
            }
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}

void StreamWindow::send_event(const protocol::InputEvent& ev, bool force) {
    // While an overlay (in-stream menu / monitor panel) is up, the user is
    // interacting with the UI, not the host — forwarding input would drive
    // the remote cursor underneath the overlay (and on a loopback session it
    // teleports the LOCAL cursor away, making the overlay unclickable).  The
    // Mac view has suppressed input while its menu is open since VIV-74; the
    // Windows window never did (VIV-50).
    //
    // `force` exists for exactly one caller: releasing keys the host already
    // has down.  That is not user input reaching the host, it is cleanup of
    // input that already reached it, and suppressing it is what left Ctrl
    // stuck.  Ctrl+F1 opens the menu, so the menu is visible by the time the
    // focus-out release runs and every KeyUp was dropped here.
    if (!force && ((menu_ && menu_->isVisible())
                   || (monitor_panel_ && monitor_panel_->isVisible())))
        return;
    if (input_cb_) input_cb_(ev);
}

protocol::MouseButton StreamWindow::qt_to_button(Qt::MouseButton btn) {
    switch (btn) {
    case Qt::LeftButton:   return protocol::MouseButton::Left;
    case Qt::RightButton:  return protocol::MouseButton::Right;
    case Qt::MiddleButton: return protocol::MouseButton::Middle;
    case Qt::XButton1:     return protocol::MouseButton::X1;
    case Qt::XButton2:     return protocol::MouseButton::X2;
    default:               return protocol::MouseButton::Left;
    }
}

void StreamWindow::mouseMoveEvent(QMouseEvent* event) {
    if (relative_mode_) {
        // In relative mode, motion comes from WM_INPUT (nativeEvent).
        // Ignore legacy WM_MOUSEMOVE so we don't double-count, and so a
        // cursor pinned at the ClipCursor edge doesn't silently block
        // camera motion.
        return;
    }

    protocol::InputEvent ev;
    ev.type = protocol::InputEventType::MouseMove;

    // The video is aspect-fitted inside the widget (the VideoProcessor
    // letterboxes to match frame aspect). We must map the widget mouse
    // coords to the video rect, not the full widget, otherwise the cursor
    // drifts through the letterbox bars.
    const double win_w = std::max(width(), 1);
    const double win_h = std::max(height(), 1);
    const double frame_aspect = (host_h_ > 0)
        ? static_cast<double>(host_w_) / static_cast<double>(host_h_)
        : win_w / win_h;
    const double window_aspect = win_w / win_h;

    double vid_w, vid_h;
    if (!keep_aspect_) {
        // Stretch-to-fill: the image covers the whole window, so map mouse
        // coords against the full client area (no letterbox bars).
        vid_w = win_w;
        vid_h = win_h;
    } else if (frame_aspect > window_aspect) {
        vid_w = win_w;
        vid_h = win_w / frame_aspect;
    } else {
        vid_h = win_h;
        vid_w = win_h * frame_aspect;
    }
    const double vid_x = (win_w - vid_w) * 0.5;
    const double vid_y = (win_h - vid_h) * 0.5;

    const double px = (static_cast<double>(event->pos().x()) - vid_x) / vid_w;
    const double py = (static_cast<double>(event->pos().y()) - vid_y) / vid_h;
    ev.x_norm = static_cast<float>(std::max(0.0, std::min(1.0, px)));
    ev.y_norm = static_cast<float>(std::max(0.0, std::min(1.0, py)));
    send_event(ev);
}

void StreamWindow::mousePressEvent(QMouseEvent* event) {
    protocol::InputEvent ev;
    ev.type = protocol::InputEventType::MouseButton;
    ev.button = qt_to_button(event->button());
    ev.pressed = true;
    send_event(ev);
}

void StreamWindow::mouseReleaseEvent(QMouseEvent* event) {
    protocol::InputEvent ev;
    ev.type = protocol::InputEventType::MouseButton;
    ev.button = qt_to_button(event->button());
    ev.pressed = false;
    send_event(ev);
}

void StreamWindow::wheelEvent(QWheelEvent* event) {
    protocol::InputEvent ev;
    ev.type = protocol::InputEventType::MouseScroll;
    QPoint delta = event->angleDelta();
    ev.scroll_dx = static_cast<int16_t>(delta.x());
    ev.scroll_dy = static_cast<int16_t>(delta.y());
    send_event(ev);
}

void StreamWindow::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) return; // skip auto-repeat, host handles it
    // Ctrl+F1: toggle the in-stream control menu locally — never forward.
    if (event->key() == Qt::Key_F1 && (event->modifiers() & Qt::ControlModifier)) {
        toggle_menu();
        return;
    }
    // F11 / Ctrl+Shift+F: borderless fullscreen toggle (VIV-20).  Client-
    // local — swallow it so the host never sees the keypress.
    if (is_fullscreen_hotkey(event)) {
        toggle_fullscreen();
        return;
    }
    // F9: toggle diagnostics HUD locally — never forward to the host.
    if (event->key() == Qt::Key_F9) {
        hud_visible_ = !hud_visible_;
        if (hud_visible_) {
            rebuild_hud_text();
            position_hud();
            hud_label_->show();
            hud_label_->raise();
        } else {
            hud_label_->hide();
        }
        return;
    }
    // Qt hands out the bare 8-bit make code; the canonical wire space is set 1
    // WITH the 0xE0 prefix, so a Linux host can tell Left Arrow from Keypad-4
    // without having to interpret a Windows virtual key (VIV-6).
    const uint16_t scan = protocol::win_scan_to_set1(
        static_cast<uint16_t>(event->nativeScanCode()),
        static_cast<uint16_t>(event->nativeVirtualKey()));
    const uint16_t vk   = static_cast<uint16_t>(event->nativeVirtualKey());
    pressed_keys_[vk] = scan;
    protocol::InputEvent ev;
    ev.type = protocol::InputEventType::KeyDown;
    ev.scan_code = scan;
    ev.vk_code = vk;
    send_event(ev);
}

void StreamWindow::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) return;
    if (event->key() == Qt::Key_F9) return;  // local toggle, don't forward
    if (event->key() == Qt::Key_F1 && (event->modifiers() & Qt::ControlModifier))
        return;                              // Ctrl+F1 menu toggle, don't forward
    if (is_fullscreen_hotkey(event))
        return;                              // F11 / Ctrl+Shift+F — local (VIV-20)
    const uint16_t scan = protocol::win_scan_to_set1(
        static_cast<uint16_t>(event->nativeScanCode()),
        static_cast<uint16_t>(event->nativeVirtualKey()));
    const uint16_t vk   = static_cast<uint16_t>(event->nativeVirtualKey());
    pressed_keys_.erase(vk);
    protocol::InputEvent ev;
    ev.type = protocol::InputEventType::KeyUp;
    ev.scan_code = scan;
    ev.vk_code = vk;
    send_event(ev);
}

void StreamWindow::update_stats(const StatsView& stats) {
    last_stats_ = stats;
    if (hud_visible_) {
        rebuild_hud_text();
        position_hud();
    }
    if (menu_ && menu_->isVisible()) feed_menu_info();
}

void StreamWindow::rebuild_hud_text() {
    if (!hud_label_) return;
    QString txt;
    txt += QString::asprintf("FPS:    %5.1f decoded / %5.1f arrived / target %u\n",
                             last_stats_.fps, last_stats_.arrived_fps,
                             last_stats_.target_fps);
    txt += QString::asprintf("RTT:    %5.1f ms   Bitrate: %u (%u) kbps\n",
                             last_stats_.rtt_ms, last_stats_.encoding_kbps,
                             last_stats_.bitrate_kbps);
    txt += QString::asprintf("Reject: %5.2f%% (%llu)   Drop: %5.2f%% (%llu)\n",
                             last_stats_.reject_pct,
                             (unsigned long long)last_stats_.total_rejected,
                             last_stats_.drop_pct,
                             (unsigned long long)last_stats_.total_dropped);
    txt += QString::asprintf("Audio:  %u pps   PLC %u%%\n",
                             last_stats_.audio_pps, last_stats_.plc_pct);
    txt += QString::asprintf("FEC:    %llu recovered / %llu failed\n",
                             (unsigned long long)last_stats_.fec_recovered,
                             (unsigned long long)last_stats_.fec_groups_failed);
    txt += QString::asprintf("Stream: %ux%u%s\n",
                             last_stats_.width, last_stats_.height,
                             last_stats_.hdr ? " HDR" : "");
    txt += QString::asprintf("Codec:   %s\n", last_stats_.codec);
    txt += QString::asprintf("Decoder: %s", last_stats_.decoder);
    hud_label_->setText(txt);
    hud_label_->adjustSize();
}

void StreamWindow::position_hud() {
    if (!hud_label_) return;
    const int pad = 12;
    // Top-level overlay → translate the in-window pad to global screen coords
    // and anchor to the right edge of the StreamWindow client area.
    const QPoint origin = mapToGlobal(QPoint(width() - hud_label_->width() - pad, pad));
    hud_label_->move(origin);
}

void StreamWindow::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    if (hud_visible_) position_hud();
    if (status_label_ && !status_label_->text().isEmpty()) position_status();
}

void StreamWindow::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (hud_label_) hud_label_->hide();
    if (status_label_) status_label_->hide();
    if (menu_) menu_->close_menu();
}

void StreamWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Restore HUD only when the user previously toggled it on (F9) AND
    // we're currently the active window.  Coming back from
    // minimized/hidden with HUD-on shouldn't surprise the user.
    if (hud_label_ && hud_visible_ && isActiveWindow()) {
        position_hud();
        hud_label_->show();
        hud_label_->raise();
    }
    if (status_label_ && !status_label_->text().isEmpty() && isActiveWindow()) {
        position_status();
        status_label_->show();
        status_label_->raise();
    }
}

void StreamWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    // The HUD is a separate top-level Qt::Tool window with
    // WindowStaysOnTopHint — without help it floats above OTHER apps
    // even when StreamWindow loses activation.  Tie its visibility to
    // ours: hide on ActivationChange-to-inactive, show on return.
    if (event->type() == QEvent::ActivationChange && hud_label_ && hud_visible_) {
        if (isActiveWindow()) {
            position_hud();
            hud_label_->show();
            hud_label_->raise();
        } else {
            hud_label_->hide();
        }
    }
    // Also handle window state changes: minimized → hide HUD.
    if (event->type() == QEvent::WindowStateChange && hud_label_) {
        if (windowState() & Qt::WindowMinimized) {
            hud_label_->hide();
        } else if (hud_visible_ && isActiveWindow()) {
            position_hud();
            hud_label_->show();
            hud_label_->raise();
        }
    }
    // Same activation/minimise discipline for the status overlay.
    const bool status_on = status_label_ && !status_label_->text().isEmpty();
    if (status_on && (event->type() == QEvent::ActivationChange
                      || event->type() == QEvent::WindowStateChange)) {
        if (isActiveWindow() && !(windowState() & Qt::WindowMinimized)) {
            position_status();
            status_label_->show();
            status_label_->raise();
        } else {
            status_label_->hide();
        }
    }
}

void StreamWindow::focusOutEvent(QFocusEvent* event) {
    // Release every key we believe is still down on the host — otherwise
    // pressing Win (or any shortcut that steals focus) strands the modifier
    // pressed on the host, because the matching KeyUp is delivered to
    // whichever window took focus, not to us.
    if (!pressed_keys_.empty()) {
        for (const auto& [vk, scan] : pressed_keys_) {
            protocol::InputEvent ev;
            ev.type = protocol::InputEventType::KeyUp;
            ev.scan_code = scan;
            ev.vk_code = static_cast<uint16_t>(vk);
            send_event(ev, /*force=*/true);
        }
        log::info("INPUT", "focus lost, released %zu stuck key(s)", pressed_keys_.size());
        pressed_keys_.clear();
    }

    // Drop relative mode while we're backgrounded so the cursor is visible
    // and the user can actually interact with whatever took focus.  Remember
    // the prior state and re-enter on focusIn.
    was_relative_on_focus_loss_ = relative_mode_;
    if (relative_mode_) exit_relative_mode();

    QWidget::focusOutEvent(event);
}

void StreamWindow::focusInEvent(QFocusEvent* event) {
    if (was_relative_on_focus_loss_ && !host_cursor_visible_) {
        enter_relative_mode();
    }
    was_relative_on_focus_loss_ = false;
    QWidget::focusInEvent(event);
}

} // namespace vivora

#endif // VIVORA_WINDOWS
