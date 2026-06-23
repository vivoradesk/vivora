#ifdef VIVORA_MACOS

#include "app/mac_view_platform.h"
#include "common/utils/log.h"
#include <utility>

bool MacViewPlatform::init(const char* host_ip, uint16_t port) {
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
    // Ctrl+F1 over the stream → toggle the menu (called on the main thread
    // from the Cocoa view's keyDown).
    view_.set_menu_hotkey_callback([this]() {
        if (!menu_) return;
        if (menu_->isVisible()) {
            menu_->close_menu();
        } else {
            feed_menu_info();
            menu_->open_over(nullptr);
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
    if (!menu_) return;
    menu_->set_actions(actions);
    menu_->set_initial_state(1.0f, false, false, /*keep_aspect=*/true);
}

void MacViewPlatform::shutdown() { menu_.reset(); }

#endif // VIVORA_MACOS
