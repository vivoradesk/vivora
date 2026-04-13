#ifdef DESKBEAM_WINDOWS

#include "client/render/stream_window.h"
#include "common/utils/log.h"
#include <QResizeEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QScreen>
#include <algorithm>

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
    protocol::InputEvent ev;
    ev.type = protocol::InputEventType::KeyDown;
    ev.scan_code = static_cast<uint16_t>(event->nativeScanCode());
    ev.vk_code = static_cast<uint16_t>(event->nativeVirtualKey());
    send_event(ev);
}

void StreamWindow::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) return;
    protocol::InputEvent ev;
    ev.type = protocol::InputEventType::KeyUp;
    ev.scan_code = static_cast<uint16_t>(event->nativeScanCode());
    ev.vk_code = static_cast<uint16_t>(event->nativeVirtualKey());
    send_event(ev);
}

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
