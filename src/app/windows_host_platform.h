#pragma once
#ifdef DESKBEAM_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "app/host_platform.h"
#include "host/capture/screen_capture.h"
#include "host/capture/dxgi_capture.h"
#include "host/encode/video_encoder.h"
#include <memory>
#include <vector>

class WindowsHostPlatform : public deskbeam::HostPlatform {
public:
    bool init(uint32_t manual_bitrate_bps,
              deskbeam::EncoderKind kind,
              deskbeam::VideoCodec codec);

    uint32_t capture_width()  const override;
    uint32_t capture_height() const override;

    void set_bitrate(uint32_t bps) override;
    void request_idr() override;

    bool capture_and_encode(uint64_t& pts_us,
                            bool& content_changed,
                            bool force) override;
    bool get_encoded_packet(EncodedPacketView& out) override;

private:
    std::unique_ptr<deskbeam::IScreenCapture> capture_;
    deskbeam::DxgiCapture* dxgi_ = nullptr;
    std::unique_ptr<deskbeam::IVideoEncoder> encoder_;
    std::vector<uint8_t> pkt_buf_;
};

#endif // DESKBEAM_WINDOWS
