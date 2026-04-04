#ifdef DESKBEAM_WINDOWS

#include "client/render/stream_window.h"
#include "common/utils/log.h"
#include <QResizeEvent>
#include <QMouseEvent>
#include <QKeyEvent>

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

bool StreamWindow::init_renderer(ID3D11Device* device, uint32_t width, uint32_t height) {
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) {
        log::error("RENDER", "No HWND available");
        return false;
    }

    resize(width, height);
    initialized_ = renderer_.init(device, hwnd, width, height);
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
