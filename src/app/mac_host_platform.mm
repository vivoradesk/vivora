#ifdef DESKBEAM_MACOS

#include "app/mac_host_platform.h"
#include "common/codec/bitrate_controller.h"
#include "common/utils/log.h"
#include <thread>
#include <chrono>
#include <utility>

bool MacHostPlatform::init(uint32_t display_index, bool prefer_hdr,
                            uint32_t manual_bitrate_bps) {
    auto displays = deskbeam::host::MacScreenCapture::enumerate_displays();
    if (displays.empty()) {
        deskbeam::log::error("HOST", "No displays found (check Screen Recording permission)");
        return false;
    }
    deskbeam::log::info("HOST", "Available displays:");
    for (const auto& d : displays) {
        deskbeam::log::info("HOST", "  [%u] %s %s", d.index, d.name.c_str(),
                            d.hdr_capable ? "(HDR capable)" : "");
    }
    if (display_index >= displays.size()) {
        deskbeam::log::error("HOST", "Display index %u out of range", display_index);
        return false;
    }

    deskbeam::host::MacCaptureConfig ccfg;
    ccfg.display_index = display_index;
    ccfg.fps = 60;
    ccfg.show_cursor = false;
    ccfg.prefer_hdr = prefer_hdr;
    if (!capture_.init(ccfg)) {
        deskbeam::log::error("HOST", "Failed to init capture");
        return false;
    }
    if (!capture_.start()) {
        deskbeam::log::error("HOST", "Failed to start capture");
        return false;
    }

    uint32_t bitrate = manual_bitrate_bps;
    if (bitrate == 0)
        bitrate = deskbeam::codec::default_bitrate_for(capture_.width(), capture_.height(), 60);

    deskbeam::host::MacEncoderConfig ecfg;
    ecfg.width = capture_.width();
    ecfg.height = capture_.height();
    ecfg.fps = 60;
    ecfg.bitrate_bps = bitrate;
    ecfg.idr_period = 120;
    ecfg.hdr = capture_.hdr_active();
    if (!encoder_.init(ecfg)) {
        deskbeam::log::error("HOST", "Failed to init encoder");
        capture_.stop();
        return false;
    }

    return true;
}

uint32_t MacHostPlatform::capture_width()  const { return capture_.width(); }
uint32_t MacHostPlatform::capture_height() const { return capture_.height(); }
uint32_t MacHostPlatform::input_width()    const { return capture_.points_width(); }
uint32_t MacHostPlatform::input_height()   const { return capture_.points_height(); }

void MacHostPlatform::set_bitrate(uint32_t bps) { encoder_.set_bitrate(bps); }
void MacHostPlatform::request_idr() { encoder_.request_idr(); }

bool MacHostPlatform::capture_and_encode(uint64_t& pts_us,
                                          bool& content_changed,
                                          bool /*force*/) {
    CVPixelBufferRef pb = capture_.try_get_frame(&pts_us);
    if (!pb)
        return false;
    content_changed = true;  // Mac capture always delivers changed frames.
    encoder_.encode(pb, pts_us);
    return true;
}

bool MacHostPlatform::get_encoded_packet(EncodedPacketView& out) {
    deskbeam::host::MacEncodedPacket pkt;
    if (!encoder_.get_packet(pkt))
        return false;
    pkt_buf_ = std::move(pkt.data);
    out.data     = pkt_buf_.data();
    out.len      = pkt_buf_.size();
    out.pts      = pkt.pts;
    out.keyframe = pkt.keyframe;
    return true;
}

void MacHostPlatform::on_idle() {
    std::this_thread::sleep_for(std::chrono::microseconds(500));
}

void MacHostPlatform::shutdown() {
    encoder_.shutdown();
    capture_.stop();
}

#endif // DESKBEAM_MACOS
