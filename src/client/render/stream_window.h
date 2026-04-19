#pragma once

#ifdef DESKBEAM_WINDOWS

#include "client/render/d3d_renderer.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/input_event.h"
#include <QCursor>
#include <QTimer>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace deskbeam {

class StreamWindow : public QWidget {
    Q_OBJECT
public:
    explicit StreamWindow(QWidget* parent = nullptr);

    bool init_renderer(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format);
    bool render_frame(ID3D11Texture2D* texture, uint32_t subresource);

    // Cursor sync hooks — proxied to D3dRenderer.
    void upload_cursor_shape(const protocol::CursorShapeMessage& shape);
    void update_cursor_position(const protocol::CursorPositionMessage& pos);

    // Set host screen resolution for coordinate mapping
    void set_host_resolution(uint32_t w, uint32_t h) { host_w_ = w; host_h_ = h; }

    // Real (pre-encoder-padding) stream dimensions from the host.  Updates
    // both the mouse-mapping size and the renderer's crop so padding rows
    // from codec alignment don't show up in the output.
    void set_stream_size(uint32_t w, uint32_t h);

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
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    QPaintEngine* paintEngine() const override { return nullptr; }

private:
    void send_event(const protocol::InputEvent& ev);
    protocol::MouseButton qt_to_button(Qt::MouseButton btn);

    // Apply shape+visibility → setCursor().  Called from the two hooks above
    // whenever either half changes.
    void refresh_cursor();

    // Relative mode (cursor hidden on host, e.g. game/3D app).  The local
    // cursor is clipped to the window, warped back to centre after every
    // movement, and only raw deltas are sent to the host.
    void enter_relative_mode();
    void exit_relative_mode();
    void update_clip_rect();
    void apply_pending_visibility();

    D3dRenderer renderer_;
    bool initialized_ = false;
    uint32_t host_w_ = 1920;
    uint32_t host_h_ = 1080;
    InputCallback input_cb_;

    // Cursor state — position is implicit (local OS mouse), only shape +
    // visibility come from the host.
    std::unordered_map<uint32_t, QCursor> cursor_cache_;
    uint32_t active_shape_id_ = 0;
    bool host_cursor_visible_ = true;
    bool have_active_shape_ = false;

    bool  relative_mode_ = false;
    bool  raw_input_registered_ = false;
    QPoint saved_global_pos_;

    // Visibility flicker debounce — some games toggle cursor visibility on
    // HUD/menu interactions.  Require stable signal for DEBOUNCE_MS before
    // actually flipping the mode.
    QTimer* visibility_debounce_ = nullptr;
    bool    pending_visibility_ = true;
    bool    pending_visibility_valid_ = false;
};

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
