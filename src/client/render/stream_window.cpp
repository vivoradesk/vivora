#ifdef DESKBEAM_WINDOWS

#include "client/render/stream_window.h"
#include "common/utils/log.h"
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QResizeEvent>
#include <QScreen>
#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace deskbeam {

StreamWindow::StreamWindow(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(640, 360);
    // Until the host sends us its cursor shape, hide the local system
    // cursor over the stream — otherwise we'd flicker between the default
    // Qt arrow and the host shape on first shape packet.
    setCursor(Qt::BlankCursor);
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
        setCursor(it->second);
    } else {
        // Host says visible but we haven't received that shape yet (first
        // packet may have been dropped) — fall back to a plain arrow so
        // the user isn't left with an invisible cursor.
        setCursor(Qt::ArrowCursor);
    }
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

void StreamWindow::send_event(const protocol::InputEvent& ev) {
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
    if (frame_aspect > window_aspect) {
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
    const uint16_t scan = static_cast<uint16_t>(event->nativeScanCode());
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
    const uint16_t scan = static_cast<uint16_t>(event->nativeScanCode());
    const uint16_t vk   = static_cast<uint16_t>(event->nativeVirtualKey());
    pressed_keys_.erase(vk);
    protocol::InputEvent ev;
    ev.type = protocol::InputEventType::KeyUp;
    ev.scan_code = scan;
    ev.vk_code = vk;
    send_event(ev);
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
            send_event(ev);
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

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
