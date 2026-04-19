#pragma once
#ifdef DESKBEAM_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "app/view_platform.h"
#include "client/decode/video_decoder.h"
#include "client/render/stream_window.h"
#include <QApplication>
#include <memory>

class WindowsViewPlatform : public deskbeam::ViewPlatform {
public:
    bool init(int argc, char* argv[], const char* host_ip, uint16_t port);

    void set_input_callback(InputCallback cb) override;
    bool pump_events() override;
    bool init_decoder(deskbeam::VideoCodec codec) override;
    bool decode(const uint8_t* data, size_t len,
                uint32_t timestamp, bool keyframe,
                uint16_t seq_no) override;
    int  render() override;
    void flush_decoder() override;
    void upload_cursor_shape(const deskbeam::protocol::CursorShapeMessage& shape) override;
    void update_cursor_position(const deskbeam::protocol::CursorPositionMessage& pos) override;
    void set_stream_size(uint32_t width, uint32_t height) override;
    void shutdown() override;

private:
    std::unique_ptr<QApplication> app_;
    std::unique_ptr<deskbeam::StreamWindow> window_;
    std::unique_ptr<deskbeam::IVideoDecoder> decoder_;
    bool renderer_ready_ = false;
    // Latest crop dims from the host — applied to the window whenever the
    // renderer becomes ready (StreamInfo may arrive before the first frame).
    uint32_t pending_stream_w_ = 0;
    uint32_t pending_stream_h_ = 0;
};

#endif // DESKBEAM_WINDOWS
