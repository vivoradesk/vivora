#ifdef DESKBEAM_WINDOWS

#include "client/render/stream_window.h"
#include "common/utils/log.h"
#include <QResizeEvent>

namespace deskbeam {

StreamWindow::StreamWindow(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
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

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
