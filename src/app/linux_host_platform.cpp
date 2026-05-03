#ifdef DESKBEAM_LINUX

#include "app/linux_host_platform.h"
#include "common/utils/log.h"
#include <chrono>

bool LinuxHostPlatform::init(uint32_t manual_bitrate_bps,
                             deskbeam::VideoCodec codec) {
    codec_       = codec;
    bitrate_bps_ = manual_bitrate_bps;
    if (!cap_.init([this](const deskbeam::host::PipeWireCapture::Frame& f) {
            on_pw_frame(f);
        })) {
        deskbeam::log::error("HOST", "PipeWire capture init failed");
        return false;
    }
    if (!cap_.start()) {
        deskbeam::log::error("HOST", "PipeWire capture start failed");
        return false;
    }

    // Block here until the first frame lands so capture_width()/height()
    // return real values when host_loop reads them for bitrate sizing.
    // Without this, the bitrate controller initialises with 0×0 → ~50 kbps
    // ceiling and the stream falls apart on the first packet loss.
    deskbeam::log::info("HOST", "Linux host platform up — waiting for first capture frame");
    std::unique_lock<std::mutex> lk(first_frame_mu_);
    if (!first_frame_cv_.wait_for(lk, std::chrono::seconds(10),
                                  [this] { return first_frame_seen_; })) {
        deskbeam::log::error("HOST",
            "Timed out waiting for first PipeWire frame (10s) — capture didn't start");
        return false;
    }
    deskbeam::log::info("HOST", "Capture is %ux%u — host loop can start", cap_w_, cap_h_);
    return true;
}

void LinuxHostPlatform::shutdown() {
    cap_.stop();
    enc_.shutdown();
}

uint32_t LinuxHostPlatform::capture_width()  const { return cap_w_; }
uint32_t LinuxHostPlatform::capture_height() const { return cap_h_; }

void LinuxHostPlatform::set_bitrate(uint32_t bps) {
    bitrate_bps_ = bps;
    if (enc_ready_) enc_.set_bitrate(static_cast<int>(bps));
}

void LinuxHostPlatform::request_idr() {
    if (enc_ready_) enc_.request_idr();
}

void LinuxHostPlatform::on_pw_frame(const deskbeam::host::PipeWireCapture::Frame& f) {
    // Lazy encoder init on first frame — capture decides the size.
    if (!enc_ready_) {
        cap_w_ = f.width;
        cap_h_ = f.height;
        deskbeam::host::VaapiEncoder::Config ec;
        ec.width  = static_cast<int>(f.width);
        ec.height = static_cast<int>(f.height);
        ec.fps    = 60;
        // Default bitrate if caller didn't override: ~bpp 0.1 at 60fps.
        ec.bitrate_bps = bitrate_bps_ > 0
            ? static_cast<int>(bitrate_bps_)
            : static_cast<int>(static_cast<int64_t>(f.width) * f.height * 60 / 10);
        // HEVC via Intel iHD vaapi has a known assertion bug at some
        // resolutions; H.264 is the safer default until that's resolved.
        ec.codec = codec_;
        if (!enc_.init(ec)) {
            deskbeam::log::error("HOST", "VAAPI encoder init failed");
            return;
        }
        enc_ready_ = true;
        // Wake init() blocked on first frame.
        {
            std::lock_guard<std::mutex> lk(first_frame_mu_);
            first_frame_seen_ = true;
        }
        first_frame_cv_.notify_all();
    }

    if (f.dmabuf_fd >= 0 || !f.data || !f.size) return;  // SHM-only path right now.

    // Skip the BGRx snapshot for heartbeat: copying ~9 MB per frame on
    // the PipeWire thread starves encode (cuts 60fps → 38fps).  Linux
    // heartbeat re-encode is deferred to Stage 4 (DMA-BUF zero-copy).
    // Encoder isn't internally thread-safe so capture_and_encode /
    // get_encoded_packet on the host_loop thread share enc_mu_.
    std::lock_guard<std::mutex> lk(enc_mu_);
    if (!enc_.encode_bgrx(f.data, static_cast<int>(f.stride), f.pts_ns / 1000)) return;
    deskbeam::host::VaapiEncoder::Packet pkt;
    while (enc_.get_packet(pkt)) {
        queued_pkts_.push(std::move(pkt));
    }
}

bool LinuxHostPlatform::capture_and_encode(uint64_t& pts_us,
                                           bool& content_changed,
                                           bool /*force*/) {
    // The actual capture+encode happens asynchronously in the PipeWire
    // thread.  Here we only signal whether the host_loop has any new
    // packets to drain.  pts/content_changed are best-effort metadata.
    std::lock_guard<std::mutex> lk(enc_mu_);
    if (queued_pkts_.empty()) return false;
    pts_us = queued_pkts_.front().pts_us;
    content_changed = true;
    return true;
}

bool LinuxHostPlatform::re_encode_last(uint64_t /*pts_us*/) {
    // Heartbeat re-encode disabled on Linux until Stage 4 wires the
    // DMA-BUF zero-copy capture buffer through directly — the SHM path
    // would cost a 9 MB/frame snapshot on the PipeWire thread.
    return false;
}

bool LinuxHostPlatform::get_cursor_state(CursorState& out) {
    if (!cap_.has_cursor()) return false;
    auto s = cap_.cursor_state();
    out.x_norm   = s.x_norm;
    out.y_norm   = s.y_norm;
    out.visible  = s.visible;
    out.shape_id = s.shape_id;
    return true;
}

bool LinuxHostPlatform::take_cursor_shape(CursorShapeView& out) {
    deskbeam::host::PipeWireCapture::CursorShape s;
    if (!cap_.take_new_cursor_shape(s)) return false;
    out.id        = s.id;
    out.width     = s.width;
    out.height    = s.height;
    out.hotspot_x = s.hotspot_x;
    out.hotspot_y = s.hotspot_y;
    out.bgra      = std::move(s.bgra);
    return true;
}

bool LinuxHostPlatform::get_encoded_packet(EncodedPacketView& out) {
    std::lock_guard<std::mutex> lk(enc_mu_);
    if (queued_pkts_.empty()) return false;
    pkt_buf_ = std::move(queued_pkts_.front());
    queued_pkts_.pop();
    out.data      = pkt_buf_.data.data();
    out.len       = pkt_buf_.data.size();
    out.pts       = pkt_buf_.pts_us;
    out.keyframe  = pkt_buf_.keyframe;
    out.heartbeat = false;  // Stage 4 will plumb the heartbeat tag.
    return true;
}

#endif // DESKBEAM_LINUX
