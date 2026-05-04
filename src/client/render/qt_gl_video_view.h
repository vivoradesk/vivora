#pragma once

#include "common/protocol/input_event.h"
#include "common/protocol/cursor_message.h"

#include "app/view_platform.h"

#include <QCursor>
#include <QLabel>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPoint>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace deskbeam::client {

// QOpenGLWidget that renders a single YUV420P frame to a fullscreen quad
// via a fragment shader doing BT.709 YUV->RGB.  Mouse / keyboard / wheel
// events are translated to `protocol::InputEvent` and forwarded through
// the registered callback (which the session sends to the host).
//
// The widget is "pull"-driven: callers push raw YUV planes via
// `update_yuv()` from the view loop thread; the next paint event uploads
// them to GL textures and renders.  We don't try to do GL work outside
// paintGL — Qt's GL context is only current there.
class QtGlVideoView : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    using InputCallback = std::function<void(const protocol::InputEvent&)>;

    explicit QtGlVideoView(QWidget* parent = nullptr);
    ~QtGlVideoView() override;

    void set_input_callback(InputCallback cb) { input_cb_ = std::move(cb); }

    // Pre-encoder stream dimensions — used to map widget-local mouse
    // coords to the host's input coord space.  Updated when the host's
    // StreamInfo arrives or as a fallback from the first decoded frame.
    void set_stream_size(uint32_t w, uint32_t h);

    // Hand a freshly-decoded YUV420P frame to the widget.  Pointers must
    // remain valid until this call returns (we copy into internal buffers
    // because GL upload happens later in paintGL).
    void update_yuv(const uint8_t* y, int y_stride,
                    const uint8_t* u, int u_stride,
                    const uint8_t* v, int v_stride,
                    uint32_t w, uint32_t h);

    // True → BT.2020 + PQ + tonemap; false → BT.709 limited-range linear.
    // Set after the decoder identifies the stream's colorspace.
    void set_hdr(bool hdr) { is_hdr_ = hdr; }

    // Update (or hide) the diagnostics overlay.  Hidden by default —
    // toggled by the F9 keypress handled inside this widget.
    void update_stats(const StatsView& stats);

    // Cursor sync: cache shape bitmap by id; switch widget cursor when
    // host signals an active shape change.  Position is intentionally
    // ignored — the local OS already places the cursor where the user
    // is pointing.
    void upload_cursor_shape(const protocol::CursorShapeMessage& shape);
    void update_cursor_position(const protocol::CursorPositionMessage& pos);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;

private:
    void emit_mouse_button(int qt_button, bool down);
    void emit_mouse_pos();
    void emit_key(int qt_key, bool down);

    QOpenGLShaderProgram      program_;
    QOpenGLVertexArrayObject  vao_;
    QOpenGLBuffer             vbo_{QOpenGLBuffer::VertexBuffer};

    // Three luminance textures (one per plane).  Keep the GLuint handles
    // raw — Qt 6.2 doesn't have QOpenGLTexture overloads we want for
    // R8 single-channel uploads at arbitrary stride.
    unsigned int y_tex_ = 0;
    unsigned int u_tex_ = 0;
    unsigned int v_tex_ = 0;

    // Last YUV frame contents — copied here on update_yuv, uploaded in
    // paintGL.  Strides are stored separately so we can pass them as the
    // PIXEL_UNPACK row length (decoder's stride may differ from width).
    std::vector<uint8_t> y_buf_, u_buf_, v_buf_;
    int  y_stride_ = 0, u_stride_ = 0, v_stride_ = 0;
    uint32_t frame_w_ = 0, frame_h_ = 0;
    bool dirty_ = false;     // true → re-upload textures next paint
    bool has_frame_ = false;
    bool is_hdr_   = false;  // true → BT.2020 + PQ + tonemap path

    // Diagnostics overlay — child QLabel positioned top-right, repositioned
    // on resize.  Shown / hidden by F9, last-pushed stats kept so that a
    // toggle while no fresh stats arrive still shows something reasonable.
    QLabel* hud_label_ = nullptr;
    bool    hud_visible_ = false;
    StatsView last_stats_{};
    void rebuild_hud_text();
    void position_hud();

    // Pre-padding stream size from the host.  Mouse mapping uses these.
    uint32_t stream_w_ = 0;
    uint32_t stream_h_ = 0;

    // Aspect-preserving viewport inside the widget (pillarbox / letterbox).
    // Recomputed in resizeGL and whenever stream_size changes.  Mouse coord
    // mapping uses these so clicks outside the image clamp cleanly.
    int viewport_x_ = 0, viewport_y_ = 0;
    int viewport_w_ = 0, viewport_h_ = 0;
    void recompute_viewport();

    InputCallback input_cb_;

    // Cursor shape cache keyed by host's shape_id; the widget's QCursor
    // is updated when active_shape_id_ changes.
    std::unordered_map<uint32_t, QCursor> cursor_cache_;
    uint32_t active_shape_id_ = 0;
    bool     have_active_shape_ = false;

    // Relative-mouse mode (host hid the cursor — game / 3D app).  Only
    // active on X11 because Wayland refuses programmatic pointer warps,
    // making the warp-to-centre trick unworkable there. Wayland will need
    // zwp_relative_pointer_v1 + pointer-constraints — separate work.
    bool   relative_mode_ = false;
    bool   host_cursor_visible_ = true;
    bool   was_relative_on_focus_loss_ = false;
    QPoint saved_global_pos_;
    QPoint last_warp_global_;     // global-space anchor we warp back to
    // Visibility flicker debounce — same shape as StreamWindow's: games
    // toggle cursor visibility on HUD interactions, wait for stable signal
    // before flipping to avoid jumpy entry/exit cycles.
    QTimer* visibility_debounce_ = nullptr;
    bool    pending_visibility_ = true;
    bool    pending_visibility_valid_ = false;
    void    apply_pending_visibility();
    void    enter_relative_mode();
    void    exit_relative_mode();
    void    refresh_cursor();
    bool    is_x11_session() const;
};

} // namespace deskbeam::client
