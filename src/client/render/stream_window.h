#pragma once

#ifdef VIVORA_WINDOWS

#include "app/view_platform.h"
#include "client/render/d3d_renderer.h"
#include "client/render/stream_menu.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/input_event.h"
#include <QCursor>
#include <QLabel>
#include <QTimer>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace vivora {

class StreamWindow : public QWidget {
    Q_OBJECT
public:
    explicit StreamWindow(QWidget* parent = nullptr);
    ~StreamWindow() override;

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

    // Push diagnostics snapshot to the HUD overlay (drawn only when visible).
    void update_stats(const StatsView& stats);

    // Centred status overlay shown before the first frame (e.g. "Connecting…",
    // "Waiting for host to accept…").  Empty string hides it.
    void set_status(const QString& text);

    // Supply the in-stream menu callbacks (VIV-74).  Forwarded to the menu
    // widget; volume/view-only/disconnect run through the view loop.
    void set_menu_actions(const MenuActions& actions);

    // Peer/device subtitle for the menu header (VIV-74).
    void set_peer_label(const QString& peer);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
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

    // In-stream menu (VIV-74).
    void toggle_menu();
    void toggle_fullscreen();
    void feed_menu_info();

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

    // Tracks keys currently considered pressed on the host side (we sent a
    // KeyDown but no matching KeyUp yet).  On focus loss we synthesise KeyUp
    // for every entry so a Win-key-triggered focus change can't leave a
    // modifier stuck.  Keyed by nativeVirtualKey; value is the last scancode
    // we reported so the KeyUp we send matches the original KeyDown.
    std::unordered_map<uint32_t, uint16_t> pressed_keys_;

    // Whether relative mode was active when we lost focus — restored on
    // re-focus so alt-tabbing out and back doesn't strand the user with a
    // visible cursor in a game that expects it hidden.
    bool was_relative_on_focus_loss_ = false;

    // Visibility flicker debounce — some games toggle cursor visibility on
    // HUD/menu interactions.  Require stable signal for DEBOUNCE_MS before
    // actually flipping the mode.
    QTimer* visibility_debounce_ = nullptr;
    bool    pending_visibility_ = true;
    bool    pending_visibility_valid_ = false;

    // Diagnostics HUD — toggled by F9.  QLabel as native child so it sits
    // on top of the WA_PaintOnScreen D3D surface (otherwise Qt's compositor
    // is bypassed and a non-native child wouldn't be visible).
    QLabel*   hud_label_   = nullptr;
    bool      hud_visible_ = false;
    StatsView last_stats_{};
    void rebuild_hud_text();
    void position_hud();

    // Centred status overlay (connecting / waiting-for-approval / close
    // reason).  Native child like the HUD so it sits over the D3D surface.
    QLabel*   status_label_ = nullptr;
    void position_status();

    // In-stream control menu (VIV-74).  Top-level overlay like the HUD, but
    // interactive; toggled by Ctrl+F1.
    StreamMenu* menu_ = nullptr;
    // Keep stream aspect ratio (letterbox) vs stretch-to-fill.  Mirrors the
    // renderer's flag so mouse-coordinate mapping matches what's on screen.
    bool keep_aspect_ = true;
};

} // namespace vivora

#endif // VIVORA_WINDOWS
