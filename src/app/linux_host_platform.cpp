#ifdef VIVORA_LINUX

#include "app/linux_host_platform.h"
#include "host/capture/wayland_outputs.h"
#include "common/utils/log.h"
#include <chrono>

bool LinuxHostPlatform::init(uint32_t manual_bitrate_bps,
                             vivora::VideoCodec codec,
                             vivora::EncoderKind encoder_kind,
                             uint16_t stream_fps) {
    codec_        = codec;
    bitrate_bps_  = manual_bitrate_bps;
    encoder_kind_ = encoder_kind;
    stream_fps_   = stream_fps > 0 ? stream_fps : 60;   // VIV-67

    // VIV-66: prefer painting the cursor from X11 (reliable position + shape)
    // and ask the portal to keep the cursor out of the captured frame.  If X
    // isn't available, fall back to the portal's embedded cursor.
    x11_cursor_active_ = x11cursor_.init();
    cap_.set_cursor_embedded(!x11_cursor_active_);
    vivora::log::info("HOST", "Cursor source: %s",
                      x11_cursor_active_ ? "X11 (portal cursor hidden)"
                                         : "portal embedded");

    if (!cap_.init([this](const vivora::host::PipeWireCapture::Frame& f) {
            on_pw_frame(f);
        })) {
        vivora::log::error("HOST", "PipeWire capture init failed");
        return false;
    }
    if (!cap_.start()) {
        vivora::log::error("HOST", "PipeWire capture start failed");
        return false;
    }

    // Block here until the first frame lands so capture_width()/height()
    // return real values when host_loop reads them for bitrate sizing.
    // Without this, the bitrate controller initialises with 0×0 → ~50 kbps
    // ceiling and the stream falls apart on the first packet loss.
    // Note (VIV-12): only capture geometry is validated here — the encoder
    // is built lazily by start_encoder() when the first viewer attaches,
    // so an encoder failure now surfaces per-connect instead of at boot.
    vivora::log::info("HOST", "Linux host platform up — waiting for first capture frame");
    std::unique_lock<std::mutex> lk(first_frame_mu_);
    if (!first_frame_cv_.wait_for(lk, std::chrono::seconds(10),
                                  [this] { return first_frame_seen_; })) {
        vivora::log::error("HOST",
            "Timed out waiting for first PipeWire frame (10s) — capture didn't start");
        return false;
    }
    vivora::log::info("HOST", "Capture is %ux%u — host loop can start", cap_w_, cap_h_);
    return true;
}

std::vector<vivora::protocol::MonitorDesc> LinuxHostPlatform::list_monitors() {
    std::vector<vivora::protocol::MonitorDesc> out;
    auto outputs = vivora::host::enumerate_wayland_outputs();
    uint8_t idx = 0;
    for (const auto& o : outputs) {
        vivora::protocol::MonitorDesc md;
        md.index   = idx++;
        md.width   = static_cast<uint16_t>(o.width);
        md.height  = static_cast<uint16_t>(o.height);
        md.primary = o.primary;
        // Best-effort "currently viewing": the portal picked the source, so we
        // can't map node→output reliably; flag the output whose logical size
        // matches the active capture.  Ties just highlight the first match.
        md.viewing = (static_cast<uint32_t>(o.width) == cap_w_
                   && static_cast<uint32_t>(o.height) == cap_h_);
        out.push_back(md);
    }
    return out;
}

void LinuxHostPlatform::shutdown() {
    // Order matters: flag first so any on_pw_frame already past cap_.stop()
    // bails out before touching encoder state; then capture stop signals
    // PipeWire; then we re-acquire enc_mu_ to serialise against any in-
    // flight callback that already got past the flag check; then free.
    shutting_down_.store(true, std::memory_order_release);
    cap_.stop();
    std::lock_guard<std::mutex> lk(enc_mu_);
    if (enc_) enc_->shutdown();
    x11cursor_.shutdown();
}

uint32_t LinuxHostPlatform::capture_width()  const { return cap_w_; }
uint32_t LinuxHostPlatform::capture_height() const { return cap_h_; }

void LinuxHostPlatform::set_bitrate(uint32_t bps) {
    // enc_mu_ serialises against the PipeWire encode callback and against
    // stop_encoder() destroying enc_ on last-client-disconnect (VIV-12).
    std::lock_guard<std::mutex> lk(enc_mu_);
    bitrate_bps_ = bps;
    if (enc_) enc_->set_bitrate(static_cast<int>(bps));
}

void LinuxHostPlatform::request_idr() {
    std::lock_guard<std::mutex> lk(enc_mu_);
    if (enc_) enc_->request_idr();
}

bool LinuxHostPlatform::start_encoder() {
    // First viewer attached (Phase B+, VIV-12).  Capture geometry is
    // guaranteed valid here: init() blocked until the first PipeWire frame
    // recorded cap_w_/cap_h_.  Building synchronously (instead of arming a
    // build-on-next-frame flag) lets host_loop see a real failure and drop
    // the connecting client instead of leaving it on a silent black stream.
    std::lock_guard<std::mutex> lk(enc_mu_);
    if (enc_) return true;   // already running
    vivora::host::ILinuxEncoder::Config ec;
    ec.width  = static_cast<int>(cap_w_);
    ec.height = static_cast<int>(cap_h_);
    ec.fps    = static_cast<int>(stream_fps_);   // VIV-67 user framerate cap
    // Default bitrate if caller didn't override: ~bpp 0.1 at the
    // configured framerate.
    ec.bitrate_bps = bitrate_bps_ > 0
        ? static_cast<int>(bitrate_bps_)
        : static_cast<int>(static_cast<int64_t>(cap_w_) * cap_h_ * stream_fps_ / 10);
    ec.codec = codec_;
    // Factory probes NVENC (NVIDIA) first when allowed, else VAAPI (VIV-8).
    enc_ = vivora::host::create_linux_encoder(encoder_kind_, ec);
    if (!enc_) {
        vivora::log::error("HOST", "no usable Linux encoder (NVENC/VAAPI both failed)");
        return false;
    }
    vivora::log::info("HOST", "Encoder started (backend: %s, %u kbps)",
                      enc_->backend_name(), static_cast<uint32_t>(ec.bitrate_bps) / 1000);
    return true;
}

void LinuxHostPlatform::stop_encoder() {
    // Last viewer dropped — release the GPU encode session.  Holding
    // enc_mu_ makes this safe against an in-flight on_pw_frame: the
    // callback either finishes its encode before we take the lock, or
    // observes enc_ == nullptr afterwards and drops the frame.  PipeWire
    // capture itself keeps running (see header note re portal re-pick).
    std::lock_guard<std::mutex> lk(enc_mu_);
    if (!enc_) return;
    enc_.reset();   // ~encoder runs shutdown(): frees NVENC/CUDA or VAAPI state
    // Drop packets queued from the ended session — the next viewer starts
    // from the fresh encoder's IDR; stale pre-gap NALs would only confuse
    // the decoder.
    std::queue<QueuedPacket>().swap(queued_pkts_);
    vivora::log::info("HOST", "Encoder stopped (no clients attached)");
}

void LinuxHostPlatform::on_pw_frame(const vivora::host::PipeWireCapture::Frame& f) {
    // First frame: latch capture geometry and unblock init().  Encoder
    // creation is decoupled from this (VIV-12 lazy encoder): it happens in
    // start_encoder() when the first viewer attaches.
    if (!geometry_seen_) {
        geometry_seen_ = true;
        cap_w_ = f.width;
        cap_h_ = f.height;
        // Wake init() blocked on first frame.
        {
            std::lock_guard<std::mutex> lk(first_frame_mu_);
            first_frame_seen_ = true;
        }
        first_frame_cv_.notify_all();
    }

    if (f.dmabuf_fd >= 0 || !f.data || !f.size) return;  // SHM-only path right now.

    // Encoder isn't internally thread-safe so capture_and_encode /
    // get_encoded_packet on the host_loop thread share enc_mu_.  Heartbeat
    // re-encode reuses the encoder's cached NV12 staging frame (~1.5 bpp,
    // already converted from BGRx) — no need to snapshot the 4-bpp BGRx
    // buffer here, which used to cost 9 MB/frame and starve capture.
    std::lock_guard<std::mutex> lk(enc_mu_);
    // Re-check the shutdown flag now that we hold the lock; without this
    // a callback already past the early bail-out at the top could race
    // through to encode_bgrx after shutdown() set the flag but before
    // it acquired enc_mu_ for the encoder teardown.
    if (shutting_down_.load(std::memory_order_acquire)) return;
    // enc_ == nullptr: no viewers attached (lazy encoder down) — drop the
    // frame; this null check is the entire per-frame cost of idling.
    if (!enc_) return;
    // Framerate pacing (VIV-67): PipeWire pushes at the compositor rate
    // (a 165 Hz panel delivers ~146 fps) and these frames never pass
    // host_loop's pull-side gate, so the cap must be enforced here.
    // The 1/8 tolerance keeps a source running at exactly the cap from
    // halving: without it, alternate frames land a hair short of the
    // interval and get dropped.  A backwards pts (capture restart /
    // monitor switch) re-latches instead of dropping.
    const int64_t interval = min_frame_interval_us_.load(std::memory_order_relaxed);
    const int64_t pts_us   = static_cast<int64_t>(f.pts_ns / 1000);
    const int64_t elapsed  = pts_us - last_accepted_pts_us_;
    if (last_accepted_pts_us_ != 0 && interval > 0 &&
        elapsed >= 0 && elapsed < interval - interval / 8) {
        return;
    }
    if (!enc_->encode_bgrx(f.data, static_cast<int>(f.stride), f.pts_ns / 1000)) return;
    last_accepted_pts_us_ = pts_us;
    vivora::host::ILinuxEncoder::Packet pkt;
    while (enc_->get_packet(pkt)) {
        queued_pkts_.push({std::move(pkt), /*heartbeat=*/false});
    }
}

void LinuxHostPlatform::set_min_frame_interval_us(int64_t us) {
    min_frame_interval_us_.store(us, std::memory_order_relaxed);
}

bool LinuxHostPlatform::capture_and_encode(uint64_t& pts_us,
                                           bool& content_changed,
                                           bool /*force*/) {
    // The actual capture+encode happens asynchronously in the PipeWire
    // thread.  Here we only signal whether the host_loop has any new
    // packets to drain.  pts/content_changed are best-effort metadata.
    std::lock_guard<std::mutex> lk(enc_mu_);
    if (queued_pkts_.empty()) return false;
    pts_us = queued_pkts_.front().pkt.pts_us;
    content_changed = true;
    return true;
}

bool LinuxHostPlatform::re_encode_last(uint64_t pts_us) {
    std::lock_guard<std::mutex> lk(enc_mu_);
    if (!enc_) return false;
    if (!enc_->reencode_last(pts_us)) return false;
    bool produced = false;
    vivora::host::ILinuxEncoder::Packet pkt;
    while (enc_->get_packet(pkt)) {
        queued_pkts_.push({std::move(pkt), /*heartbeat=*/true});
        produced = true;
    }
    return produced;
}

bool LinuxHostPlatform::get_cursor_state(CursorState& out) {
    if (x11_cursor_active_) {
        float xn = 0, yn = 0; bool vis = false, changed = false;
        vivora::host::X11Cursor::Shape sh;
        if (!x11cursor_.poll(xn, yn, vis, changed, sh)) return false;
        if (changed) {
            pending_x11_shape_      = std::move(sh);
            have_pending_x11_shape_ = true;
            x11_shape_id_           = pending_x11_shape_.id;
        }
        out.x_norm   = xn;
        out.y_norm   = yn;
        out.visible  = vis;
        out.shape_id = x11_shape_id_;
        return true;
    }
    if (!cap_.has_cursor()) return false;
    auto s = cap_.cursor_state();
    out.x_norm   = s.x_norm;
    out.y_norm   = s.y_norm;
    out.visible  = s.visible;
    out.shape_id = s.shape_id;
    return true;
}

bool LinuxHostPlatform::take_cursor_shape(CursorShapeView& out) {
    if (x11_cursor_active_) {
        if (!have_pending_x11_shape_) return false;
        out.id        = pending_x11_shape_.id;
        out.width     = pending_x11_shape_.width;
        out.height    = pending_x11_shape_.height;
        out.hotspot_x = pending_x11_shape_.hotspot_x;
        out.hotspot_y = pending_x11_shape_.hotspot_y;
        out.bgra      = std::move(pending_x11_shape_.bgra);
        have_pending_x11_shape_ = false;
        return true;
    }
    vivora::host::PipeWireCapture::CursorShape s;
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
    out.data      = pkt_buf_.pkt.data.data();
    out.len       = pkt_buf_.pkt.data.size();
    out.pts       = pkt_buf_.pkt.pts_us;
    out.keyframe  = pkt_buf_.pkt.keyframe;
    out.heartbeat = pkt_buf_.heartbeat;
    return true;
}

#endif // VIVORA_LINUX
