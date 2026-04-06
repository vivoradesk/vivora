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

    // Use actual widget client size for swap chain, frame size for VP input
    uint32_t client_w = static_cast<uint32_t>(this->width());
    uint32_t client_h = static_cast<uint32_t>(this->height());
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
        renderer_.resize(event->size().width(), event->size().height());
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
    // Map widget coords to 0..1 normalized coords
    ev.x_norm = static_cast<float>(event->pos().x()) / static_cast<float>(std::max(width(), 1));
    ev.y_norm = static_cast<float>(event->pos().y()) / static_cast<float>(std::max(height(), 1));
    // Clamp
    ev.x_norm = std::max(0.0f, std::min(1.0f, ev.x_norm));
    ev.y_norm = std::max(0.0f, std::min(1.0f, ev.y_norm));
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
