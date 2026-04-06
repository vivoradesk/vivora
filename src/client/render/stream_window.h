#pragma once

#ifdef DESKBEAM_WINDOWS

#include "client/render/d3d_renderer.h"
#include "common/protocol/input_event.h"
#include <QWidget>
#include <functional>

namespace deskbeam {

class StreamWindow : public QWidget {
    Q_OBJECT
public:
    explicit StreamWindow(QWidget* parent = nullptr);

    bool init_renderer(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format);
    bool render_frame(ID3D11Texture2D* texture, uint32_t subresource);

    // Set host screen resolution for coordinate mapping
    void set_host_resolution(uint32_t w, uint32_t h) { host_w_ = w; host_h_ = h; }

    // Callback for input events
    using InputCallback = std::function<void(const protocol::InputEvent&)>;
    void set_input_callback(InputCallback cb) { input_cb_ = std::move(cb); }

protected:
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    QPaintEngine* paintEngine() const override { return nullptr; }

private:
    void send_event(const protocol::InputEvent& ev);
    protocol::MouseButton qt_to_button(Qt::MouseButton btn);

    D3dRenderer renderer_;
    bool initialized_ = false;
    uint32_t host_w_ = 1920;
    uint32_t host_h_ = 1080;
    InputCallback input_cb_;
};

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
