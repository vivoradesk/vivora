#pragma once
#ifdef VIVORA_LINUX

#include "app/view_platform.h"
#include "client/decode/ffmpeg_decoder.h"
#include "client/render/qt_gl_video_view.h"

#include <QApplication>
#include <QMainWindow>

#include <memory>

class LinuxViewPlatform : public vivora::ViewPlatform {
public:
    bool init(int argc, char* argv[], const char* host_ip, uint16_t port);

    void set_input_callback(InputCallback cb) override;
    bool pump_events() override;
    bool init_decoder(vivora::VideoCodec codec) override;
    bool decode(const uint8_t* data, size_t len,
                uint32_t timestamp, bool keyframe,
                uint16_t seq_no) override;
    int  render() override;
    void flush_decoder() override;
    void set_stream_size(uint32_t width, uint32_t height) override;
    void update_stats(const vivora::StatsView& stats) override;
    void set_status(const char* text) override;
    void set_menu_actions(const vivora::MenuActions& actions) override;
    void upload_cursor_shape(const vivora::protocol::CursorShapeMessage& shape) override;
    void update_cursor_position(const vivora::protocol::CursorPositionMessage& pos) override;
    void shutdown() override;

private:
    std::unique_ptr<QApplication>           app_;
    std::unique_ptr<QMainWindow>            window_;
    vivora::client::QtGlVideoView*        view_ = nullptr;  // owned by window_
    std::unique_ptr<vivora::client::FfmpegDecoder> decoder_;

    // StreamInfo can arrive before the first decoded frame or before the
    // window is fully initialized — stash it and apply when ready.
    uint32_t pending_stream_w_ = 0;
    uint32_t pending_stream_h_ = 0;
};

#endif // VIVORA_LINUX
