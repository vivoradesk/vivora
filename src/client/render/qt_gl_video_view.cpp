#include "client/render/qt_gl_video_view.h"
#include "common/utils/log.h"

#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace vivora::client {

namespace {

// Qt 6.2 ships GLSL ES 3.0 / OpenGL Core 3.3 by default depending on the
// system.  `#version 330 core` works on every desktop driver Linux ships
// (Mesa Gallium has supported it since ~2014); we don't target embedded
// GL ES devices yet.
const char* kVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_tex;
out vec2 v_uv;
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_tex;
}
)";

// Two colorspace paths in one shader, selected by `u_hdr`:
//   * SDR (u_hdr == 0): BT.709 limited-range Y'CbCr -> RGB, no extra
//     EOTF — output is already in display sRGB approximation.
//   * HDR (u_hdr == 1): BT.2020 NCL limited-range Y'CbCr -> RGB, then
//     PQ inverse EOTF to linear nits, Hable filmic tonemap to [0..1],
//     then sRGB OETF.  Gamut mapping is a simple clip — Rec.709-out-
//     of-gamut colors desaturate; full gamut mapping would need a
//     perceptual matrix beyond what we want in a hot-path shader.
//
// 8-bit input precision is lossy for PQ (lower mids quantize visibly),
// but the output is acceptable for a desktop streaming viewer.  L4 with
// 10-bit GL textures will fix this.
const char* kFragmentShader = R"(
#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D y_tex;
uniform sampler2D u_tex;
uniform sampler2D v_tex;
uniform int       u_hdr;

vec3 yuv_limited_to_rgb_bt709(float Y, float U, float V) {
    Y = (Y - 16.0/255.0) * (255.0/219.0);
    U = (U - 0.5) * (255.0/224.0);
    V = (V - 0.5) * (255.0/224.0);
    float r = Y + 1.5748 * V;
    float g = Y - 0.1873 * U - 0.4681 * V;
    float b = Y + 1.8556 * U;
    return vec3(r, g, b);
}

vec3 yuv_limited_to_rgb_bt2020(float Y, float U, float V) {
    Y = (Y - 16.0/255.0) * (255.0/219.0);
    U = (U - 0.5) * (255.0/224.0);
    V = (V - 0.5) * (255.0/224.0);
    // BT.2020 non-constant-luminance matrix.
    float r = Y               + 1.4746 * V;
    float g = Y - 0.16455 * U - 0.57135 * V;
    float b = Y + 1.8814 * U;
    return vec3(r, g, b);
}

// SMPTE 2084 PQ inverse EOTF: encoded [0..1] -> linear [0..1] where 1.0
// represents 10000 cd/m².  Clamp before pow to avoid NaN on negatives.
vec3 pq_to_linear(vec3 v) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    vec3 p   = pow(max(v, vec3(0.0)), vec3(1.0 / m2));
    vec3 num = max(p - c1, vec3(0.0));
    vec3 den = c2 - c3 * p;
    return pow(num / max(den, vec3(1e-6)), vec3(1.0 / m1));
}

vec3 srgb_oetf(vec3 v) {
    bvec3 small = lessThan(v, vec3(0.0031308));
    vec3  lo    = v * 12.92;
    vec3  hi    = 1.055 * pow(max(v, vec3(0.0)), vec3(1.0/2.4)) - 0.055;
    return mix(hi, lo, vec3(small));
}

uniform float u_hdr_exposure;  // multiplier on linear PQ output

void main() {
    float Y = texture(y_tex, v_uv).r;
    float U = texture(u_tex, v_uv).r;
    float V = texture(v_tex, v_uv).r;

    vec3 rgb;
    if (u_hdr != 0) {
        rgb = yuv_limited_to_rgb_bt2020(Y, U, V);
        // PQ -> linear (1.0 == 10000 nits) then exposure-scale.  Exposure
        // is set per-frame on the CPU from a 99th-percentile Y sample so
        // dim SDR-in-HDR content maps to sRGB white and bright HDR videos
        // don't blow out — replaces the old hardcoded 50x gain.  Highlight
        // clip still happens (no filmic tonemap), but the auto-exposure
        // keeps that to actual specular highlights instead of midtones.
        vec3 lin = pq_to_linear(clamp(rgb, 0.0, 1.0));
        lin *= u_hdr_exposure;
        rgb = srgb_oetf(clamp(lin, 0.0, 1.0));
    } else {
        rgb = yuv_limited_to_rgb_bt709(Y, U, V);
    }
    frag = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

// Map a Qt key code to a (rough) Windows VK code, for the host-side
// SendInput path.  This is intentionally a small subset for L2 — full
// XKB->VK mapping comes with the host-side Linux input injector.
uint16_t qt_key_to_vk(int qt_key) {
    if (qt_key >= Qt::Key_A && qt_key <= Qt::Key_Z) return 0x41 + (qt_key - Qt::Key_A);
    if (qt_key >= Qt::Key_0 && qt_key <= Qt::Key_9) return 0x30 + (qt_key - Qt::Key_0);
    if (qt_key >= Qt::Key_F1 && qt_key <= Qt::Key_F12) return 0x70 + (qt_key - Qt::Key_F1);
    switch (qt_key) {
        case Qt::Key_Backspace: return 0x08;
        case Qt::Key_Tab:       return 0x09;
        case Qt::Key_Return:    return 0x0D;
        case Qt::Key_Enter:     return 0x0D;
        case Qt::Key_Shift:     return 0x10;
        case Qt::Key_Control:   return 0x11;
        case Qt::Key_Alt:       return 0x12;
        case Qt::Key_Pause:     return 0x13;
        case Qt::Key_CapsLock:  return 0x14;
        case Qt::Key_Escape:    return 0x1B;
        case Qt::Key_Space:     return 0x20;
        case Qt::Key_PageUp:    return 0x21;
        case Qt::Key_PageDown:  return 0x22;
        case Qt::Key_End:       return 0x23;
        case Qt::Key_Home:      return 0x24;
        case Qt::Key_Left:      return 0x25;
        case Qt::Key_Up:        return 0x26;
        case Qt::Key_Right:     return 0x27;
        case Qt::Key_Down:      return 0x28;
        case Qt::Key_Insert:    return 0x2D;
        case Qt::Key_Delete:    return 0x2E;
        case Qt::Key_Meta:      return 0x5B;  // Windows / Super
        case Qt::Key_Comma:     return 0xBC;
        case Qt::Key_Period:    return 0xBE;
        case Qt::Key_Slash:     return 0xBF;
        case Qt::Key_Semicolon: return 0xBA;
        case Qt::Key_Apostrophe:return 0xDE;
        case Qt::Key_BracketLeft:  return 0xDB;
        case Qt::Key_BracketRight: return 0xDD;
        case Qt::Key_Backslash: return 0xDC;
        case Qt::Key_Minus:     return 0xBD;
        case Qt::Key_Equal:     return 0xBB;
        case Qt::Key_QuoteLeft: return 0xC0;  // Backtick
    }
    return 0;
}

} // namespace

QtGlVideoView::QtGlVideoView(QWidget* parent) : QOpenGLWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    // Diagnostics overlay.  Child QLabel renders on top of the GL widget;
    // semi-transparent dark background, monospace, top-right corner.
    // Default hidden — F9 toggles.
    hud_label_ = new QLabel(this);
    hud_label_->setAttribute(Qt::WA_TransparentForMouseEvents);
    hud_label_->setStyleSheet(
        "QLabel {"
        "  background: rgba(0, 0, 0, 160);"
        "  color: rgb(220, 220, 220);"
        "  font-family: 'DejaVu Sans Mono', 'Consolas', monospace;"
        "  font-size: 12px;"
        "  padding: 8px;"
        "  border-radius: 4px;"
        "}");
    hud_label_->setText("HUD ready (F9)");
    hud_label_->adjustSize();
    hud_label_->hide();
}

QtGlVideoView::~QtGlVideoView() {
    if (context()) {
        makeCurrent();
        if (y_tex_) glDeleteTextures(1, &y_tex_);
        if (u_tex_) glDeleteTextures(1, &u_tex_);
        if (v_tex_) glDeleteTextures(1, &v_tex_);
        vbo_.destroy();
        vao_.destroy();
        doneCurrent();
    }
}

void QtGlVideoView::set_stream_size(uint32_t w, uint32_t h) {
    stream_w_ = w;
    stream_h_ = h;
    recompute_viewport();
    update();
}

void QtGlVideoView::recompute_viewport() {
    const int vw = width();
    const int vh = height();
    uint32_t sw = stream_w_, sh = stream_h_;
    if (sw == 0 || sh == 0) { sw = frame_w_; sh = frame_h_; }
    if (sw == 0 || sh == 0 || vw <= 0 || vh <= 0) {
        viewport_x_ = 0; viewport_y_ = 0;
        viewport_w_ = vw; viewport_h_ = vh;
        return;
    }
    // Fit `sw x sh` into `vw x vh` preserving aspect ratio (letterbox /
    // pillarbox).  No upscaling caps — GL bilinear filtering handles it.
    const double ar_widget = static_cast<double>(vw) / vh;
    const double ar_stream = static_cast<double>(sw) / sh;
    if (ar_widget > ar_stream) {
        // Widget is wider than stream → pillarbox (black bars left/right).
        viewport_h_ = vh;
        viewport_w_ = static_cast<int>(vh * ar_stream + 0.5);
        viewport_x_ = (vw - viewport_w_) / 2;
        viewport_y_ = 0;
    } else {
        // Widget is taller → letterbox (black bars top/bottom).
        viewport_w_ = vw;
        viewport_h_ = static_cast<int>(vw / ar_stream + 0.5);
        viewport_x_ = 0;
        viewport_y_ = (vh - viewport_h_) / 2;
    }
}

void QtGlVideoView::update_yuv(const uint8_t* y, int y_stride,
                               const uint8_t* u, int u_stride,
                               const uint8_t* v, int v_stride,
                               uint32_t w, uint32_t h) {
    // Copy planes — paintGL runs later, decoder buffer may be reused.
    y_buf_.resize(static_cast<size_t>(y_stride) * h);
    u_buf_.resize(static_cast<size_t>(u_stride) * (h / 2));
    v_buf_.resize(static_cast<size_t>(v_stride) * (h / 2));
    std::memcpy(y_buf_.data(), y, y_buf_.size());
    std::memcpy(u_buf_.data(), u, u_buf_.size());
    std::memcpy(v_buf_.data(), v, v_buf_.size());
    y_stride_ = y_stride;
    u_stride_ = u_stride;
    v_stride_ = v_stride;
    const bool size_changed = (frame_w_ != w || frame_h_ != h);
    frame_w_ = w;
    frame_h_ = h;
    has_frame_ = true;
    dirty_ = true;
    // First decoded frame (or resolution change) — refresh viewport so
    // the aspect rect matches the actual decoded dims when no StreamInfo
    // arrived yet.
    if (size_changed) recompute_viewport();

    // HDR auto-exposure — sample once per ~30 frames (≈1 Hz at 30 fps,
    // 0.5 Hz at 60).  Cheap: ~4k Y samples on a 1080p frame at 16-px
    // stride.  Cycle is short enough to react to scene changes within
    // ~2 s after EWMA but slow enough to not pump on small flickers.
    if (is_hdr_ && ++hdr_sample_counter_ >= 30) {
        hdr_sample_counter_ = 0;
        recompute_hdr_exposure();
    }

    update();  // schedule paintGL
}

void QtGlVideoView::recompute_hdr_exposure() {
    if (frame_w_ == 0 || frame_h_ == 0 || y_buf_.empty()) return;
    // Subsample at 16-px stride in both dimensions — for 1920x1080 that's
    // ~120x67 = ~8000 samples, plenty for a percentile estimate while
    // staying ~10 µs on CPU.  Stride is in pixels; row pitch is y_stride_.
    constexpr int STRIDE = 16;
    const int W = static_cast<int>(frame_w_);
    const int H = static_cast<int>(frame_h_);
    std::vector<uint8_t> samples;
    samples.reserve((W / STRIDE + 1) * (H / STRIDE + 1));
    for (int y = 0; y < H; y += STRIDE) {
        const uint8_t* row = y_buf_.data() + static_cast<size_t>(y) * y_stride_;
        for (int x = 0; x < W; x += STRIDE) {
            samples.push_back(row[x]);
        }
    }
    if (samples.empty()) return;

    // 99th percentile via nth_element — O(n).
    const size_t pct_idx = samples.size() - samples.size() / 100;
    std::nth_element(samples.begin(), samples.begin() + pct_idx - 1, samples.end());
    const uint8_t y8 = samples[pct_idx - 1];

    // BT.2020 limited-range Y' to full-range PQ-encoded value [0..1].
    float y_full = (static_cast<float>(y8) - 16.0f) * (255.0f / 219.0f) / 255.0f;
    y_full = std::clamp(y_full, 0.0f, 1.0f);

    // PQ EOTF (inverse): PQ-encoded → linear nits / 10000.
    constexpr float m1 = 2610.0f / 4096.0f / 4.0f;
    constexpr float m2 = 2523.0f / 4096.0f * 128.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 4096.0f * 32.0f;
    constexpr float c3 = 2392.0f / 4096.0f * 32.0f;
    const float p   = std::pow(std::max(y_full, 0.0f), 1.0f / m2);
    const float num = std::max(p - c1, 0.0f);
    const float den = std::max(c2 - c3 * p, 1e-6f);
    const float lin = std::pow(num / den, 1.0f / m1);  // 0..1, 1 == 10000 nits

    // Target exposure: scale 99th-percentile linear to ~0.85 of sRGB
    // headroom so highlights have ~15% room before hard clipping.
    constexpr float HEADROOM = 0.85f;
    constexpr float MIN_EXPOSURE = 5.0f;     // very bright HDR (1700+ nits peak)
    constexpr float MAX_EXPOSURE = 200.0f;   // very dim SDR-in-HDR (~50 nits)
    const float target = std::clamp(HEADROOM / std::max(lin, 1e-4f),
                                    MIN_EXPOSURE, MAX_EXPOSURE);

    // EWMA: fast darken (avoid blowout flicker), slow brighten (avoid
    // pumping on transient dim frames).
    const float alpha = (target < hdr_exposure_) ? 0.5f : 0.1f;
    hdr_exposure_ = hdr_exposure_ * (1.0f - alpha) + target * alpha;
}

void QtGlVideoView::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    program_.addShaderFromSourceCode(QOpenGLShader::Vertex,   kVertexShader);
    program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    if (!program_.link()) {
        log::error("QtGL", "Shader link failed: %s",
                   program_.log().toUtf8().constData());
        return;
    }

    // Fullscreen quad: pos.xy + tex.uv.  Y axis flipped — Qt's GL viewport
    // has origin at bottom-left, but our YUV buffers have origin at
    // top-left.  Flipping the V coordinate avoids an upside-down image.
    static const float quad[] = {
        // pos        tex
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
    };
    vao_.create();
    vao_.bind();
    vbo_.create();
    vbo_.bind();
    vbo_.allocate(quad, sizeof(quad));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    vbo_.release();
    vao_.release();

    glGenTextures(1, &y_tex_);
    glGenTextures(1, &u_tex_);
    glGenTextures(1, &v_tex_);
    for (auto t : {y_tex_, u_tex_, v_tex_}) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void QtGlVideoView::resizeGL(int /*w*/, int /*h*/) {
    recompute_viewport();
    position_hud();
}

void QtGlVideoView::paintGL() {
    // Clear the entire widget first so the letter/pillarbox bars stay
    // black even when the viewport shrinks to the stream's aspect.
    glViewport(0, 0, width(), height());
    glClear(GL_COLOR_BUFFER_BIT);
    if (!has_frame_) return;
    glViewport(viewport_x_, viewport_y_, viewport_w_, viewport_h_);

    if (dirty_) {
        // GL_RED single-channel upload at the decoder's plane stride.
        // Using GL_UNPACK_ROW_LENGTH lets us upload directly without
        // pre-tightening rows on the CPU.
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, y_tex_);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, y_stride_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame_w_, frame_h_,
                     0, GL_RED, GL_UNSIGNED_BYTE, y_buf_.data());

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, u_tex_);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, u_stride_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame_w_ / 2, frame_h_ / 2,
                     0, GL_RED, GL_UNSIGNED_BYTE, u_buf_.data());

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, v_tex_);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, v_stride_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame_w_ / 2, frame_h_ / 2,
                     0, GL_RED, GL_UNSIGNED_BYTE, v_buf_.data());

        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        dirty_ = false;
    }

    program_.bind();
    program_.setUniformValue("y_tex", 0);
    program_.setUniformValue("u_tex", 1);
    program_.setUniformValue("v_tex", 2);
    program_.setUniformValue("u_hdr", is_hdr_ ? 1 : 0);
    program_.setUniformValue("u_hdr_exposure", hdr_exposure_);
    vao_.bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    vao_.release();
    program_.release();
}

void QtGlVideoView::emit_mouse_pos() {
    if (!input_cb_ || stream_w_ == 0 || stream_h_ == 0) return;
    if (viewport_w_ <= 0 || viewport_h_ <= 0) return;
    auto p = mapFromGlobal(QCursor::pos());
    // Translate widget-local coords into the aspect-preserved viewport
    // rect, then normalize.  Mouse outside the visible image clamps to
    // the nearest edge — the host's input injector will saturate at the
    // screen border, so a click on a pillarbox bar still reaches the
    // closest column of pixels rather than landing somewhere arbitrary.
    float xn = static_cast<float>(p.x() - viewport_x_) / static_cast<float>(viewport_w_);
    float yn = static_cast<float>(p.y() - viewport_y_) / static_cast<float>(viewport_h_);
    if (xn < 0.0f) xn = 0.0f; else if (xn > 1.0f) xn = 1.0f;
    if (yn < 0.0f) yn = 0.0f; else if (yn > 1.0f) yn = 1.0f;
    protocol::InputEvent ev{};
    ev.type   = protocol::InputEventType::MouseMove;
    ev.x_norm = xn;
    ev.y_norm = yn;
    input_cb_(ev);
}

void QtGlVideoView::emit_mouse_button(int qt_button, bool down) {
    if (!input_cb_) return;
    protocol::InputEvent ev{};
    ev.type    = protocol::InputEventType::MouseButton;
    ev.pressed = down;
    switch (qt_button) {
        case Qt::LeftButton:    ev.button = protocol::MouseButton::Left;   break;
        case Qt::RightButton:   ev.button = protocol::MouseButton::Right;  break;
        case Qt::MiddleButton:  ev.button = protocol::MouseButton::Middle; break;
        case Qt::XButton1:      ev.button = protocol::MouseButton::X1;     break;
        case Qt::XButton2:      ev.button = protocol::MouseButton::X2;     break;
        default: return;
    }
    input_cb_(ev);
}

void QtGlVideoView::emit_key(int qt_key, bool down) {
    if (!input_cb_) return;
    uint16_t vk = qt_key_to_vk(qt_key);
    if (vk == 0) return;  // unmapped — drop rather than confuse host
    protocol::InputEvent ev{};
    ev.type      = down ? protocol::InputEventType::KeyDown
                        : protocol::InputEventType::KeyUp;
    ev.vk_code   = vk;
    ev.scan_code = 0;  // host derives scancode from VK on Windows
    input_cb_(ev);
}

void QtGlVideoView::mouseMoveEvent(QMouseEvent* e) {
    if (!relative_mode_) {
        emit_mouse_pos();
        return;
    }
    // Relative mode: compute delta from the global anchor, send as a raw
    // motion delta, then warp back to the anchor.  The post-warp event
    // delivers (0,0) which we ignore — otherwise we'd halve every motion.
    const QPoint cur = e->globalPosition().toPoint();
    const int dx = cur.x() - last_warp_global_.x();
    const int dy = cur.y() - last_warp_global_.y();
    if (dx == 0 && dy == 0) return;
    if (input_cb_) {
        protocol::InputEvent ev{};
        ev.type = protocol::InputEventType::MouseMoveRelative;
        ev.dx   = dx;
        ev.dy   = dy;
        input_cb_(ev);
    }
    // Re-anchor to widget centre — also handles the case where the widget
    // was moved/resized between events.
    last_warp_global_ = mapToGlobal(QPoint(width() / 2, height() / 2));
    QCursor::setPos(last_warp_global_);
}
void QtGlVideoView::mousePressEvent(QMouseEvent* e)  { emit_mouse_button(e->button(), true); }
void QtGlVideoView::mouseReleaseEvent(QMouseEvent* e){ emit_mouse_button(e->button(), false); }

void QtGlVideoView::focusOutEvent(QFocusEvent* e) {
    // Same shape as StreamWindow: drop relative grab while we're back-
    // grounded so the user can interact with whatever stole focus, but
    // remember the prior state so re-focus restores it.
    was_relative_on_focus_loss_ = relative_mode_;
    if (relative_mode_) exit_relative_mode();
    QOpenGLWidget::focusOutEvent(e);
}

void QtGlVideoView::focusInEvent(QFocusEvent* e) {
    if (was_relative_on_focus_loss_ && !host_cursor_visible_) {
        enter_relative_mode();
    }
    was_relative_on_focus_loss_ = false;
    QOpenGLWidget::focusInEvent(e);
}

void QtGlVideoView::wheelEvent(QWheelEvent* e) {
    if (!input_cb_) return;
    // Send raw angleDelta in 1/8-degree units (= Windows WHEEL_DELTA scale,
    // 120 per notch). Earlier we pre-divided by 120 to "notches" but that
    // truncated touchpad two-finger scrolls (deltas of 16-40 units) to 0
    // — wheel mice still sent enough events to feel OK, but trackpads
    // ended up scrolling glacially. Host injectors expect WHEEL_DELTA
    // units (Windows uses the value as-is, Mac divides by 40 → lines).
    QPoint d = e->angleDelta();
    protocol::InputEvent ev{};
    ev.type      = protocol::InputEventType::MouseScroll;
    ev.scroll_dx = static_cast<int16_t>(d.x());
    ev.scroll_dy = static_cast<int16_t>(d.y());
    input_cb_(ev);
}

void QtGlVideoView::keyPressEvent(QKeyEvent* e)   {
    if (e->isAutoRepeat()) return;  // host handles repeat itself
    // F9: toggle diagnostics HUD.  Don't forward the key to the host —
    // it's a client-local debug control.
    if (e->key() == Qt::Key_F9) {
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
    emit_key(e->key(), true);
}

void QtGlVideoView::update_stats(const StatsView& stats) {
    last_stats_ = stats;
    if (hud_visible_) {
        rebuild_hud_text();
        position_hud();
    }
}

void QtGlVideoView::rebuild_hud_text() {
    if (!hud_label_) return;
    QString txt;
    txt += QString::asprintf("FPS:    %5.1f decoded / %5.1f arrived / target %u\n",
                             last_stats_.fps, last_stats_.arrived_fps,
                             last_stats_.target_fps);
    txt += QString::asprintf("RTT:    %5.1f ms   Bitrate: %u kbps\n",
                             last_stats_.rtt_ms, last_stats_.bitrate_kbps);
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
    txt += QString::asprintf("Decoder: %s", last_stats_.decoder);
    hud_label_->setText(txt);
    hud_label_->adjustSize();
}

void QtGlVideoView::position_hud() {
    if (!hud_label_) return;
    const int pad = 12;
    hud_label_->move(width() - hud_label_->width() - pad, pad);
}

void QtGlVideoView::keyReleaseEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) return;
    emit_key(e->key(), false);
}

void QtGlVideoView::upload_cursor_shape(const protocol::CursorShapeMessage& shape) {
    // Host BGRA matches Qt Format_ARGB32 byte order on little-endian.
    const size_t expected = static_cast<size_t>(shape.width) * shape.height * 4u;
    if (shape.width == 0 || shape.height == 0 || shape.bgra.size() < expected) return;

    QImage img(shape.bgra.data(), shape.width, shape.height,
               static_cast<int>(shape.width) * 4,
               QImage::Format_ARGB32);
    QPixmap pix = QPixmap::fromImage(img.copy());
    QCursor cur(pix, static_cast<int>(shape.hotspot_x), static_cast<int>(shape.hotspot_y));
    cursor_cache_[shape.shape_id] = std::move(cur);

    if (have_active_shape_ && shape.shape_id == active_shape_id_) {
        auto it = cursor_cache_.find(active_shape_id_);
        if (it != cursor_cache_.end()) setCursor(it->second);
    }
}

void QtGlVideoView::update_cursor_position(const protocol::CursorPositionMessage& pos) {
    // Position itself is ignored — local OS already follows the user's
    // mouse.  We use this message for two things: the active shape id, and
    // the visibility flag (host hides the cursor in games / 3D apps —
    // that's the trigger to enter relative-input mode).
    const bool shape_changed = !have_active_shape_ || pos.shape_id != active_shape_id_;
    active_shape_id_   = pos.shape_id;
    have_active_shape_ = true;

    if (pos.visible != host_cursor_visible_) {
        // Debounce visibility flips — games toggle visible/hidden on HUD
        // / menu interactions and we don't want to ping-pong relative mode
        // every couple of frames.  Same shape as StreamWindow's logic.
        if (!visibility_debounce_) {
            visibility_debounce_ = new QTimer(this);
            visibility_debounce_->setSingleShot(true);
            connect(visibility_debounce_, &QTimer::timeout,
                    this, &QtGlVideoView::apply_pending_visibility);
        }
        if (!pending_visibility_valid_ || pending_visibility_ != pos.visible) {
            pending_visibility_ = pos.visible;
            pending_visibility_valid_ = true;
            visibility_debounce_->start(50);
        }
    } else if (pending_visibility_valid_ && pending_visibility_ != pos.visible) {
        // Visibility reverted before debounce fired — cancel the pending flip.
        pending_visibility_valid_ = false;
    }

    if (shape_changed) refresh_cursor();
}

void QtGlVideoView::apply_pending_visibility() {
    if (!pending_visibility_valid_) return;
    pending_visibility_valid_ = false;
    if (pending_visibility_ == host_cursor_visible_) return;
    host_cursor_visible_ = pending_visibility_;
    if (!host_cursor_visible_) enter_relative_mode();
    else                       exit_relative_mode();
    refresh_cursor();
}

bool QtGlVideoView::is_x11_session() const {
    // QGuiApplication::platformName() returns "xcb" on X11 and "wayland"
    // on Wayland.  Pointer warp + grab is functional on X11; Wayland
    // refuses warp for security so we'd need zwp_relative_pointer_v1 /
    // pointer-constraints to make this work there.
    return QGuiApplication::platformName() == QStringLiteral("xcb");
}

void QtGlVideoView::enter_relative_mode() {
    if (relative_mode_) return;
    if (!is_x11_session()) {
        log::warn("CURSOR", "relative mode requested but session is not X11 (%s) — ignoring",
                  QGuiApplication::platformName().toUtf8().constData());
        return;
    }
    relative_mode_ = true;
    saved_global_pos_ = QCursor::pos();
    grabMouse();
    // Anchor at widget centre so deltas are computed against a fixed point
    // and we have maximum room before the cursor approaches the screen
    // edge between events.
    last_warp_global_ = mapToGlobal(QPoint(width() / 2, height() / 2));
    QCursor::setPos(last_warp_global_);
    log::info("CURSOR", "enter relative mode (saved pos %d,%d, anchor %d,%d)",
              saved_global_pos_.x(), saved_global_pos_.y(),
              last_warp_global_.x(), last_warp_global_.y());
}

void QtGlVideoView::exit_relative_mode() {
    if (!relative_mode_) return;
    relative_mode_ = false;
    releaseMouse();
    if (is_x11_session()) QCursor::setPos(saved_global_pos_);
    log::info("CURSOR", "exit relative mode (restored pos %d,%d)",
              saved_global_pos_.x(), saved_global_pos_.y());
}

void QtGlVideoView::refresh_cursor() {
    if (!host_cursor_visible_) {
        setCursor(Qt::BlankCursor);
        return;
    }
    auto it = cursor_cache_.find(active_shape_id_);
    if (it != cursor_cache_.end()) {
        setCursor(it->second);
    } else {
        // Host says visible but we haven't received that shape yet — fall
        // back to a plain arrow so the user isn't left invisible.
        setCursor(Qt::ArrowCursor);
    }
}

} // namespace vivora::client
