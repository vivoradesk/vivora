#ifdef VIVORA_MACOS

#include "app/mac_view_platform.h"
#include "common/utils/log.h"
#include <utility>

bool MacViewPlatform::init(const char* host_ip, uint16_t port) {
    // CLI --view lands here with no QApplication; the StreamMenu below is a
    // QWidget and aborts without one.  Construct it BEFORE the Cocoa window
    // so NSApp initialization order matches the (working) GUI connect path.
    if (!QApplication::instance()) {
        static int qt_argc = 1;
        static char app_name[] = "vivora";
        static char* qt_argv[] = { app_name, nullptr };
        app_ = std::make_unique<QApplication>(qt_argc, qt_argv);
    }
    if (!view_.create_window("Vivora", 1280, 720)) {
        vivora::log::error("VIEW", "Failed to create window");
        return false;
    }

    // In-stream control menu (VIV-74) — Qt overlay over the Cocoa window.
    menu_ = std::make_unique<vivora::StreamMenu>(nullptr);
    menu_->hide();
    menu_->set_header("Vivora",
        QString("%1:%2").arg(host_ip ? host_ip : "").arg(port));
    QObject::connect(menu_.get(), &vivora::StreamMenu::fullscreenToggled,
                     menu_.get(), [this]() { view_.toggle_fullscreen(); });
    QObject::connect(menu_.get(), &vivora::StreamMenu::keepAspectToggled,
                     menu_.get(), [this](bool keep) { view_.set_keep_aspect(keep); });
    // When the menu closes (Esc / click-away / Disconnect), let the stream
    // view resume input and re-take the cursor — unless the monitor panel
    // took over (menu → panel hand-off keeps input suppressed, VIV-50).
    QObject::connect(menu_.get(), &vivora::StreamMenu::closed,
                     menu_.get(), [this]() {
        if (!(monitor_panel_ && monitor_panel_->isVisible()))
            view_.set_menu_open(false);
    });

    // "Switch monitor…" panel (VIV-50) — same top-level overlay pattern as
    // the menu.  The original VIV-50 commit wired it only into the Windows
    // StreamWindow; on the Mac the menu button silently did nothing.
    monitor_panel_ = std::make_unique<vivora::MonitorPanel>(nullptr);
    monitor_panel_->hide();
    QObject::connect(menu_.get(), &vivora::StreamMenu::monitorClicked,
                     menu_.get(), [this]() {
        if (actions_.request_monitors) actions_.request_monitors();
        menu_->close_menu();
        monitor_panel_->set_monitors(last_monitors_);
        monitor_panel_->open_over(nullptr);
        view_.set_menu_open(true);   // keep input suppressed under the panel
    });
    QObject::connect(monitor_panel_.get(), &vivora::MonitorPanel::closed,
                     monitor_panel_.get(), [this]() {
        view_.set_menu_open(false);
    });
    // Click on the stream (outside the menu) dismisses it — the Qt overlay
    // doesn't get a reliable deactivation under Cocoa, so the view tells us.
    view_.set_menu_dismiss_callback([this]() {
        if (menu_ && menu_->isVisible()) menu_->close_menu();
    });
    // Cmd/Ctrl+F1 over the stream → toggle the menu (called on the main
    // thread from the Cocoa view's keyDown).
    view_.set_menu_hotkey_callback([this]() {
        if (!menu_) return;
        // Hotkey also dismisses the monitor panel (VIV-50), like Esc does.
        if (monitor_panel_ && monitor_panel_->isVisible()) {
            monitor_panel_->close_panel();   // closed() restores input
            return;
        }
        if (menu_->isVisible()) {
            menu_->close_menu();
            view_.set_menu_open(false);
        } else {
            feed_menu_info();
            menu_->open_over(nullptr);
            view_.set_menu_open(true);
        }
    });
    return true;
}

void MacViewPlatform::feed_menu_info() {
    if (!menu_) return;
    vivora::MenuInfo mi;
    mi.rtt_ms          = last_stats_.rtt_ms;
    mi.transport       = QString::fromUtf8(last_stats_.transport);
    mi.width           = last_stats_.width;
    mi.height          = last_stats_.height;
    mi.hz              = last_stats_.target_fps;
    mi.codec           = QString::fromUtf8(last_stats_.codec);
    mi.decoder         = QString::fromUtf8(last_stats_.decoder);
    mi.session_seconds = last_stats_.session_seconds;
    mi.connected       = last_stats_.width > 0;
    menu_->set_info(mi);
}

void MacViewPlatform::set_input_callback(InputCallback cb) {
    view_.set_input_callback(std::move(cb));
}

bool MacViewPlatform::pump_events() {
    view_.pump_events();
    // CLI path: the Qt overlay (StreamMenu) gets no events from the Cocoa
    // pump — drive its queue here, like the Windows platform does.
    if (app_) app_->processEvents();
    return !view_.should_close();
}

bool MacViewPlatform::init_decoder(vivora::VideoCodec codec) {
    view_.set_codec(codec);
    return true;
}

bool MacViewPlatform::decode(const uint8_t* data, size_t len,
                              uint32_t timestamp, bool keyframe,
                              uint16_t /*seq_no*/) {
    return view_.submit_frame(data, len, timestamp, keyframe);
}

int MacViewPlatform::render() {
    // Mac view decodes + renders inside submit_frame, so each
    // successful decode() call is also a render.  Return 0 here
    // to avoid double-counting — the common loop counts decode().
    return 0;
}

void MacViewPlatform::flush_decoder() {
    view_.flush_decoder();
}

void MacViewPlatform::set_stream_size(uint32_t width, uint32_t height) {
    view_.set_stream_size(width, height);
}

void MacViewPlatform::upload_cursor_shape(const vivora::protocol::CursorShapeMessage& shape) {
    view_.upload_cursor_shape(shape);
}

void MacViewPlatform::update_cursor_position(const vivora::protocol::CursorPositionMessage& pos) {
    view_.update_cursor_position(pos);
}

void MacViewPlatform::update_stats(const vivora::StatsView& stats) {
    view_.update_stats(stats);
    last_stats_ = stats;
    if (menu_ && menu_->isVisible()) feed_menu_info();
}

void MacViewPlatform::set_status(const char* text) {
    view_.set_status(text);
}

void MacViewPlatform::set_menu_actions(const vivora::MenuActions& actions) {
    actions_ = actions;
    if (!menu_) return;
    menu_->set_actions(actions);
    menu_->set_initial_state(1.0f, false, false, /*keep_aspect=*/true);
    // Wire the panel's switch/refresh to the same session callbacks (VIV-50),
    // mirroring the Windows StreamWindow wiring.
    if (monitor_panel_) {
        monitor_panel_->set_select_callback([this](uint32_t idx) {
            if (actions_.select_monitor) actions_.select_monitor(idx);
        });
        monitor_panel_->set_refresh_callback([this]() {
            if (actions_.request_monitors) actions_.request_monitors();
        });
    }
}

void MacViewPlatform::set_monitor_list(
        const std::vector<vivora::protocol::MonitorDesc>& monitors) {
    last_monitors_ = monitors;
    if (monitor_panel_) monitor_panel_->set_monitors(monitors);
}

vivora::IVideoPipeline* MacViewPlatform::video_pipeline() {
    if (!pipeline_) pipeline_ = std::make_unique<vivora::MacVideoPipeline>(&view_);
    return pipeline_.get();
}

void MacViewPlatform::shutdown() {
    pipeline_.reset();   // decode thread is already joined (ViewLoop teardown)
    monitor_panel_.reset();
    menu_.reset();
}

#endif // VIVORA_MACOS
