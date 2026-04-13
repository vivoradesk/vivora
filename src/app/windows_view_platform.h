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
    bool decode(const uint8_t* data, size_t len,
                uint32_t timestamp, bool keyframe,
                uint16_t seq_no) override;
    int  render() override;
    void flush_decoder() override;
    void shutdown() override;

private:
    std::unique_ptr<QApplication> app_;
    std::unique_ptr<deskbeam::StreamWindow> window_;
    std::unique_ptr<deskbeam::IVideoDecoder> decoder_;
    bool renderer_ready_ = false;
};

#endif // DESKBEAM_WINDOWS
