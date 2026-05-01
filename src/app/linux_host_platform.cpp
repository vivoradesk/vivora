#ifdef DESKBEAM_LINUX

#include "app/linux_host_platform.h"
#include "common/utils/log.h"

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
    deskbeam::log::info("HOST", "Linux host platform up — waiting for first capture frame");
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
    }

    if (f.dmabuf_fd >= 0 || !f.data || !f.size) return;  // SHM-only path right now.

    // Snapshot the BGRx buffer for heartbeat re-encode under the same
    // lock so re_encode_last sees a consistent copy.
    {
        std::lock_guard<std::mutex> lk(frame_mu_);
        last_bgrx_.assign(f.data, f.data + f.size);
        last_stride_ = static_cast<int>(f.stride);
    }

    // Encode + drain.  Encoder isn't internally thread-safe, so the
    // PipeWire thread holds enc_mu_ for the whole submit/drain window;
    // capture_and_encode / get_encoded_packet take the same lock from
    // the host_loop thread.
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

bool LinuxHostPlatform::re_encode_last(uint64_t pts_us) {
    if (!enc_ready_) return false;

    // Snapshot the latest BGRx buffer.
    std::vector<uint8_t> snapshot;
    int stride = 0;
    {
        std::lock_guard<std::mutex> lk(frame_mu_);
        if (last_bgrx_.empty()) return false;
        snapshot = last_bgrx_;
        stride = last_stride_;
    }

    std::lock_guard<std::mutex> lk(enc_mu_);
    if (!enc_.encode_bgrx(snapshot.data(), stride, pts_us)) return false;
    deskbeam::host::VaapiEncoder::Packet pkt;
    while (enc_.get_packet(pkt)) {
        queued_pkts_.push(std::move(pkt));
    }
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
