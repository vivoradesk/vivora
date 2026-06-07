#ifdef VIVORA_MACOS

#include "app/mac_view_platform.h"
#include "common/utils/log.h"
#include <utility>

bool MacViewPlatform::init(const char* /*host_ip*/, uint16_t /*port*/) {
    if (!view_.create_window("Vivora", 1280, 720)) {
        vivora::log::error("VIEW", "Failed to create window");
        return false;
    }
    return true;
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
}

void MacViewPlatform::set_status(const char* text) {
    view_.set_status(text);
}

void MacViewPlatform::shutdown() {}

#endif // VIVORA_MACOS
