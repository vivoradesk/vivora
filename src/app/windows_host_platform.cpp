#ifdef DESKBEAM_WINDOWS

#include "app/windows_host_platform.h"
#include <windows.h>
#include "common/codec/bitrate_controller.h"
#include "common/utils/log.h"
#include <chrono>
#include <utility>

bool WindowsHostPlatform::init(uint32_t manual_bitrate_bps) {
    capture_ = deskbeam::IScreenCapture::create();
    dxgi_ = dynamic_cast<deskbeam::DxgiCapture*>(capture_.get());
    if (!capture_ || !capture_->init(0)) {
        deskbeam::log::error("HOST", "Failed to init capture");
        return false;
    }
    auto res = capture_->get_resolution();
    deskbeam::log::info("HOST", "Capture: %ux%u", res.width, res.height);

    uint32_t bitrate = manual_bitrate_bps;
    if (bitrate == 0)
        bitrate = deskbeam::codec::default_bitrate_for(res.width, res.height, 60);

    encoder_ = deskbeam::IVideoEncoder::create();
    deskbeam::EncoderConfig cfg;
    cfg.width = res.width;
    cfg.height = res.height;
    cfg.fps = 60;
    cfg.bitrate_bps = bitrate;
    cfg.idr_period = 60;
    if (dxgi_) cfg.input_format = dxgi_->get_capture_format();

    if (!encoder_->init(cfg, dxgi_ ? dxgi_->get_device() : nullptr)) {
        deskbeam::log::error("HOST", "Failed to init encoder");
        return false;
    }

    // Force initial mouse movement for DXGI.
    INPUT mi = {};
    mi.type = INPUT_MOUSE;
    mi.mi.dwFlags = MOUSEEVENTF_MOVE;
    mi.mi.dx = 1;
    SendInput(1, &mi, sizeof(INPUT));

    return true;
}

uint32_t WindowsHostPlatform::capture_width()  const { return capture_->get_resolution().width; }
uint32_t WindowsHostPlatform::capture_height() const { return capture_->get_resolution().height; }

void WindowsHostPlatform::set_bitrate(uint32_t bps) { encoder_->set_bitrate(bps); }
void WindowsHostPlatform::request_idr() { encoder_->request_idr(); }

bool WindowsHostPlatform::capture_and_encode(uint64_t& pts_us,
                                              bool& content_changed,
                                              bool force) {
    deskbeam::CapturedFrame frame;
    if (!capture_->capture_frame(frame, 16))
        return false;

    pts_us = std::chrono::duration_cast<std::chrono::microseconds>(
        frame.capture_time.time_since_epoch()).count();
    content_changed = frame.content_changed;

    if (!content_changed && !force) {
        capture_->release_frame(frame);
        return false;
    }

    if (!encoder_->encode(frame.texture, pts_us)) {
        capture_->release_frame(frame);
        return false;
    }
    capture_->release_frame(frame);
    return true;
}

bool WindowsHostPlatform::get_encoded_packet(EncodedPacketView& out) {
    deskbeam::EncodedPacket pkt;
    if (!encoder_->get_packet(pkt))
        return false;
    pkt_buf_ = std::move(pkt.data);
    out.data     = pkt_buf_.data();
    out.len      = pkt_buf_.size();
    out.pts      = pkt.pts;
    out.keyframe = pkt.keyframe;
    return true;
}

#endif // DESKBEAM_WINDOWS
