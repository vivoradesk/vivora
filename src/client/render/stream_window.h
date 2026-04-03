#pragma once

#ifdef DESKBEAM_WINDOWS

#include "client/render/d3d_renderer.h"
#include <QWidget>

namespace deskbeam {

class StreamWindow : public QWidget {
    Q_OBJECT
public:
    explicit StreamWindow(QWidget* parent = nullptr);

    bool init_renderer(ID3D11Device* device, uint32_t width, uint32_t height);

    // Render a decoded frame. Call from main thread.
    bool render_frame(ID3D11Texture2D* texture, uint32_t subresource);

protected:
    void resizeEvent(QResizeEvent* event) override;
    QPaintEngine* paintEngine() const override { return nullptr; } // we render via D3D11

private:
    D3dRenderer renderer_;
    bool initialized_ = false;
};

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
