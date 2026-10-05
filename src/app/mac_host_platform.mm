// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef VIVORA_MACOS

#include "app/mac_host_platform.h"
#include "common/codec/bitrate_controller.h"
#include "common/utils/log.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <utility>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

namespace {

// VIV-116: process-global keepalive for the ScreenCaptureKit session.
//
// Pause tears the host worker (thread + MacHostPlatform + encoder + network
// session) all the way down — the only thing we deliberately keep alive is the
// SCStream.  A brand-new SCStream on Resume is a fresh capture session that
// macOS 15+/26 re-confirms with the native Screen Recording prompt even when
// the permission is already granted; keeping the exact same SCStream running
// avoids that.
//
// The live MacScreenCapture is moved (unique_ptr, so the pointee never
// relocates — see the header note) into this slot by shutdown() and moved back
// out by start_pipeline() on the next start.  While stowed the SCStream keeps
// running on its own dispatch queue: frames land in its drop-oldest slot and
// are simply never pulled (no encoder, no worker), which costs a little GPU but
// no CPU on our side.  Its internal restart/wake watchdog keeps `this` valid
// because the object stays alive here.
//
// Access is serialised by the GUI: HostWorker::stop() blocks on thread join
// (so shutdown()'s stow completes) before AppController::startSharing() spins a
// new worker whose init() adopts — the two never overlap.  The mutex is belt-
// and-braces against any future caller that breaks that ordering.
struct MacCaptureKeepalive {
    std::mutex mtx;
    std::unique_ptr<vivora::host::MacScreenCapture> capture;
    uint32_t display_index = 0;
    uint16_t fps = 0;
};

MacCaptureKeepalive& keepalive() {
    static MacCaptureKeepalive g_keepalive;
    return g_keepalive;
}

// VIV-147: set by the CoreGraphics display-reconfiguration callback (which
// fires on the main run loop, not the host loop) and cleared by
// poll_display_change() on the host loop.  Process-global rather than a member
// because CGDisplayRegisterReconfigurationCallback has no un-register that we
// can reliably pair with a worker restart — a stale `this` in the callback
// would be a use-after-free, and a flag never can be.
std::atomic<bool> g_display_config_dirty{false};

void display_reconfigure_cb(CGDirectDisplayID /*display*/,
                            CGDisplayChangeSummaryFlags flags,
                            void* /*userInfo*/) {
    // BeginConfiguration is the "about to change" notice; act on the settled
    // state only, or we re-enumerate mid-flight and read the old geometry.
    if (flags & kCGDisplayBeginConfigurationFlag) return;
    g_display_config_dirty.store(true, std::memory_order_release);
}

} // namespace

bool MacHostPlatform::init(uint32_t display_index,
                            uint32_t manual_bitrate_bps,
                            vivora::VideoCodec codec,
                            uint16_t stream_fps) {
    stream_fps_ = stream_fps > 0 ? stream_fps : 60;   // VIV-67
    // NSCursor / NSScreen need the AppKit shared app initialized. Host mode
    // doesn't create a UI, so make sure the singleton exists.
    [NSApplication sharedApplication];

    // Remember the negotiated codec; start_encoder() builds the VideoToolbox
    // session for it lazily when a viewer attaches (VIV-7).
    codec_ = codec;
    if (codec == vivora::VideoCodec::H264) {
        vivora::log::info("HOST", "Negotiated H.264 (SDR fallback) for the macOS host encoder");
    }
    init_error_.clear();
    auto displays = vivora::host::MacScreenCapture::enumerate_displays();
    if (displays.empty()) {
        vivora::log::error("HOST", "No displays found (check Screen Recording permission)");
        // VIV-111: almost always a missing/denied Screen Recording grant (or a
        // pending prompt that timed out) — surface that, not a generic failure.
        init_error_ = "Screen Recording permission required — macOS returned no "
                      "capturable displays.";
        return false;
    }
    vivora::log::info("HOST", "Available displays:");
    for (const auto& d : displays) {
        vivora::log::info("HOST", "  [%u] %s %s", d.index, d.name.c_str(),
                            d.hdr_capable ? "(HDR capable)" : "");
    }
    if (display_index >= displays.size()) {
        vivora::log::error("HOST", "Display index %u out of range", display_index);
        init_error_ = "Selected display is no longer available.";
        return false;
    }

    manual_bitrate_bps_ = manual_bitrate_bps;
    install_display_reconfigure_hook();
    if (!start_pipeline(display_index)) {
        init_error_ = "Failed to start the screen-capture pipeline.";
        return false;
    }
    return true;
}

// (Re)build capture for the given display.  Shared by init() and the VIV-50
// monitor switch.  Caller has already validated display_index.  The encoder
// is NOT built here — start_encoder() creates it when a viewer attaches
// (Phase B+ lazy encoder, VIV-12), so a host that's "Listening" all day
// holds no VideoToolbox session until somebody connects.
bool MacHostPlatform::start_pipeline(uint32_t display_index) {
    // VIV-116: adopt a live SCK session kept alive across a previous pause if
    // it targets the same display + fps.  Adoption reuses the running
    // SCStream, so macOS does not re-raise the Screen Recording prompt.
    if (adopt_keepalive_capture(display_index)) {
        current_display_index_ = display_index;
        vivora::log::info("HOST", "Adopted kept-alive SCK capture (display %u) — no re-prompt",
                          display_index);
        return true;
    }

    auto cap = std::make_unique<vivora::host::MacScreenCapture>();
    vivora::host::MacCaptureConfig ccfg;
    ccfg.display_index = display_index;
    ccfg.fps = stream_fps_;   // VIV-67 user framerate cap
    ccfg.show_cursor = false;
    // Auto-detect: capture HDR if the display reports HDR, otherwise SDR.
    // MacScreenCapture honours prefer_hdr only when display_is_hdr() agrees.
    ccfg.prefer_hdr = true;
    if (!cap->init(ccfg)) {
        vivora::log::error("HOST", "Failed to init capture");
        return false;
    }
    if (!cap->start()) {
        vivora::log::error("HOST", "Failed to start capture");
        return false;
    }

    capture_ = std::move(cap);
    current_display_index_ = display_index;
    return true;
}

bool MacHostPlatform::adopt_keepalive_capture(uint32_t display_index) {
    auto& ka = keepalive();
    std::lock_guard<std::mutex> lock(ka.mtx);
    if (!ka.capture) return false;
    if (ka.display_index == display_index && ka.fps == stream_fps_) {
        // Compatible: take ownership of the still-running SCStream.
        capture_ = std::move(ka.capture);
        ka.fps = 0;
        return true;
    }
    // Stowed capture targets a different display or fps (a Settings change
    // between pause and resume).  We can't reuse it — tear it down (this stops
    // the SCStream and removes its wake observer) so it doesn't linger, then
    // fall back to a fresh capture.  This rare path may re-prompt, which is
    // acceptable for an intentional capture-config change.
    vivora::log::info("HOST",
                      "Stowed SCK capture incompatible (display %u/fps %u vs %u/%u) — rebuilding",
                      ka.display_index, ka.fps, display_index, stream_fps_);
    ka.capture.reset();
    ka.fps = 0;
    return false;
}

void MacHostPlatform::stow_keepalive_capture() {
    if (!capture_) return;
    auto& ka = keepalive();
    std::lock_guard<std::mutex> lock(ka.mtx);
    // Normally the slot is empty here (the matching resume already adopted the
    // previous one).  If something is still stowed, drop it before overwriting
    // so we never leak a running SCStream.
    if (ka.capture) ka.capture.reset();
    ka.display_index = current_display_index_;
    ka.fps           = stream_fps_;
    ka.capture       = std::move(capture_);
    vivora::log::info("HOST",
                      "Stowed live SCK capture across pause (display %u, %u fps) — kept alive",
                      ka.display_index, ka.fps);
}

bool MacHostPlatform::start_encoder() {
    if (encoder_live_) return true;   // already running

    // Bitrate: last adaptive value if we've streamed before, else the
    // user's pinned rate, else auto from the live capture resolution.
    uint32_t bitrate = live_bitrate_bps_;
    if (bitrate == 0) {
        bitrate = manual_bitrate_bps_ != 0
            ? manual_bitrate_bps_
            : vivora::codec::default_bitrate_for(capture_->width(), capture_->height(),
                                                 stream_fps_);
    }

    vivora::host::MacEncoderConfig ecfg;
    ecfg.width = capture_->width();
    ecfg.height = capture_->height();
    ecfg.fps = stream_fps_;   // VIV-67 user framerate cap
    ecfg.bitrate_bps = bitrate;
    // Keep the keyframe interval at ~2s worth of frames (120 @ 60fps).
    ecfg.idr_period = 2u * stream_fps_;
    ecfg.codec = codec_;   // VIV-7: honour the negotiated H.264/HEVC choice
    // H.264 is SDR-only; the encoder ignores hdr for H.264 but keep the config
    // honest so the log reflects reality.
    ecfg.hdr = (codec_ != vivora::VideoCodec::H264) && capture_->hdr_active();
    if (!encoder_.init(ecfg)) {
        vivora::log::error("HOST", "Failed to init encoder");
        return false;
    }
    encoder_live_     = true;
    live_bitrate_bps_ = bitrate;
    vivora::log::info("HOST", "Encoder started (%s, %u kbps)",
                      codec_ == vivora::VideoCodec::H264 ? "h264" : "hevc",
                      bitrate / 1000);
    return true;
}

void MacHostPlatform::stop_encoder() {
    if (!encoder_live_) return;
    // Invalidate the VT session (flushes + releases GPU state and clears
    // the output queue).  Capture keeps running — see header note (VIV-95).
    encoder_.shutdown();
    encoder_live_ = false;
    vivora::log::info("HOST", "Encoder stopped (no clients attached)");
}

bool MacHostPlatform::set_codec(vivora::VideoCodec codec) {
    // VIV-112: client-driven codec switch.  start_encoder() reads codec_ when
    // it builds the VideoToolbox session, so update it and rebuild if live.
    // Runs on the host_loop thread alongside start/stop_encoder — no lock.
    if (codec == codec_) return true;   // already there
    // H.264 is 8-bit SDR only; downgrading it on an HDR capture yields wrong
    // colours (mirrors the Windows FP16 guard).  Refuse and stay on HEVC.
    if (codec == vivora::VideoCodec::H264 && capture_ && capture_->hdr_active()) {
        vivora::log::warn("HOST",
            "set_codec: refusing H.264 downgrade on HDR capture — keeping HEVC");
        return false;
    }
    codec_ = codec;
    if (!encoder_live_) return true;    // applied at next start_encoder()
    stop_encoder();
    if (!start_encoder()) {
        vivora::log::error("HOST", "set_codec: encoder rebuild failed for %s",
                           codec == vivora::VideoCodec::H264 ? "h264" : "hevc");
        return false;
    }
    vivora::log::info("HOST", "Encoder switched to %s (codec negotiation)",
                      codec == vivora::VideoCodec::H264 ? "h264" : "hevc");
    return true;
}

std::vector<vivora::protocol::MonitorDesc> MacHostPlatform::list_monitors() {
    std::vector<vivora::protocol::MonitorDesc> out;
    for (const auto& d : vivora::host::MacScreenCapture::enumerate_displays()) {
        vivora::protocol::MonitorDesc md;
        md.index   = static_cast<uint8_t>(d.index);
        md.width   = static_cast<uint16_t>(d.width_px);
        md.height  = static_cast<uint16_t>(d.height_px);
        md.primary = (d.index == 0);  // SCShareableContent lists the main display first
        md.viewing = (d.index == current_display_index_);
        out.push_back(md);
    }
    return out;
}

void MacHostPlatform::install_display_reconfigure_hook() {
    if (display_hook_installed_) return;
    // Registered once per process; the callback only flips a global flag, so
    // re-registration across worker restarts would just double-set it.
    static bool s_registered = false;
    if (!s_registered) {
        CGError err = CGDisplayRegisterReconfigurationCallback(
            display_reconfigure_cb, nullptr);
        if (err != kCGErrorSuccess) {
            vivora::log::warn("HOST",
                "CGDisplayRegisterReconfigurationCallback failed (%d) — "
                "display hot-plug will not refresh the monitor list", (int)err);
            return;
        }
        s_registered = true;
    }
    display_hook_installed_ = true;
    // A worker starting up should not inherit a change flagged before it
    // existed; the list it advertises on connect is already current.
    g_display_config_dirty.store(false, std::memory_order_release);
}

bool MacHostPlatform::poll_display_change() {
    return g_display_config_dirty.exchange(false, std::memory_order_acq_rel);
}

bool MacHostPlatform::refresh_capture() {
    // The captured display may have changed resolution (or vanished) under the
    // running SCStream.  SCK keeps delivering at the size the stream was
    // configured with, so compare against the display list and rebuild when
    // they disagree — the encoder is bound to the old size (VIV-147).
    auto displays = vivora::host::MacScreenCapture::enumerate_displays();
    if (displays.empty()) {
        vivora::log::warn("HOST", "refresh_capture: no displays enumerated");
        return false;
    }
    uint32_t index = current_display_index_;
    if (index >= displays.size()) {
        vivora::log::warn("HOST", "Captured display %u is gone — falling back to display 0",
                          index);
        index = 0;
    }
    const uint32_t want_w = displays[index].width_px;
    const uint32_t want_h = displays[index].height_px;
    if (index == current_display_index_
        && want_w == capture_width() && want_h == capture_height())
        return false;   // topology moved, but not under us

    const bool had_encoder = encoder_live_;
    stop_encoder();
    // Full teardown: the SCStream is configured for the old geometry, and a
    // stow/adopt would hand the stale one straight back.
    capture_.reset();
    if (!start_pipeline(index) || (had_encoder && !start_encoder())) {
        vivora::log::error("HOST", "refresh_capture: rebuild on display %u failed", index);
        return true;    // geometry did change; the caller still re-syncs
    }
    vivora::log::info("HOST", "Capture rebuilt after display change: %ux%u",
                      capture_width(), capture_height());
    return true;
}

bool MacHostPlatform::select_monitor(uint32_t index, bool /*seed_cursor*/) {
    if (index == current_display_index_) return true;
    auto displays = vivora::host::MacScreenCapture::enumerate_displays();
    if (index >= displays.size()) {
        vivora::log::warn("HOST", "select_monitor: display %u out of range", index);
        return false;
    }
    const uint32_t prev = current_display_index_;
    // Tear the pipeline down and rebuild on the new display.  VideoToolbox is
    // bound to the old resolution, so a full encoder shutdown/init is needed.
    // Rebuild the encoder only if it was live (a viewer is attached) —
    // otherwise the lazy-encoder gap stays encoder-free (VIV-12).
    const bool had_encoder = encoder_live_;
    stop_encoder();
    // Full teardown (not a stow): we're switching displays, so the old
    // display's SCStream must not survive.  reset() stops it + removes its
    // wake observer before start_pipeline() builds one for the new display.
    capture_.reset();
    if (start_pipeline(index) && (!had_encoder || start_encoder()))
        return true;
    // Roll back to the previous display so the session keeps streaming.
    vivora::log::error("HOST", "select_monitor: rebuild on display %u failed, restoring %u",
                       index, prev);
    stop_encoder();
    capture_.reset();
    if (!start_pipeline(prev)) return false;
    return had_encoder ? start_encoder() : true;
}

uint32_t MacHostPlatform::capture_width()  const { return capture_ ? capture_->width() : 0; }
uint32_t MacHostPlatform::capture_height() const { return capture_ ? capture_->height() : 0; }
uint32_t MacHostPlatform::input_width()    const { return capture_ ? capture_->points_width() : 0; }
uint32_t MacHostPlatform::input_height()   const { return capture_ ? capture_->points_height() : 0; }

void MacHostPlatform::set_bitrate(uint32_t bps) {
    // Remember the value even when the encoder is torn down so the next
    // start_encoder() resumes at the latest adaptive rate (VIV-12).
    live_bitrate_bps_ = bps;
    if (encoder_live_) encoder_.set_bitrate(bps);
}
void MacHostPlatform::request_idr() {
    // Don't latch idr_pending_ into a torn-down encoder: the next
    // session's first frame is an IDR anyway.
    if (encoder_live_) encoder_.request_idr();
}

bool MacHostPlatform::capture_and_encode(uint64_t& pts_us,
                                          bool& content_changed,
                                          bool force) {
    // Phase B+ lazy encoder (VIV-12): host_loop's zero-client gate normally
    // keeps us out of here while the encoder is down, but guard anyway so a
    // disconnect racing this tick can't feed a dead VT session.  Not pulling
    // the frame leaves it in the capture's latest-frame slot — free.
    if (!encoder_live_ || !capture_) return false;

    CVPixelBufferRef pb = capture_->try_get_frame(&pts_us);
    if (pb) {
        content_changed = true;
        encoder_.encode(pb, pts_us);  // encoder takes ownership + CFReleases
        return true;
    }
    // No fresh frame from SCK.  In the normal path that's fine — host
    // loop idles.  In the IDR-on-loss path (force=true) the client is
    // waiting on a keyframe to recover, and SCK may stay dormant
    // indefinitely on static content.  Re-encode the last delivered
    // frame so the encoder has SOMETHING to base its IDR on.
    if (!force) return false;
    pb = capture_->get_last_frame_for_force(&pts_us);
    if (!pb) return false;
    content_changed = false;  // refresh of last frame, not new content.
    encoder_.encode(pb, pts_us);  // encoder takes ownership + CFReleases
    return true;
}

bool MacHostPlatform::re_encode_last(uint64_t pts_us) {
    // Host_loop's heartbeat path — invoked when capture stays silent for
    // longer than min_frame_interval (static screen with no SCK dirty
    // rect emission).  Re-feed the cached last frame so the wire
    // maintains the expected cadence: keeps WiFi power-saving from
    // killing the link and gives FEC groups a steady fill rate.
    if (!encoder_live_ || !capture_) return false;   // lazy encoder down (VIV-12)
    uint64_t cached_pts = 0;
    CVPixelBufferRef pb = capture_->get_last_frame_for_force(&cached_pts);
    if (!pb) return false;
    encoder_.encode(pb, pts_us);  // encoder takes ownership + CFReleases
    return true;
}

bool MacHostPlatform::get_encoded_packet(EncodedPacketView& out) {
    if (!encoder_live_) return false;   // queue is cleared by shutdown()
    vivora::host::MacEncodedPacket pkt;
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
    stop_encoder();   // no-op when the lazy encoder is already down
    // VIV-116: do NOT stop the SCK capture here.  shutdown() runs on every
    // pause (AppController::stopSharing -> HostWorker::stop -> loop exit ->
    // platform teardown).  Stopping + rebuilding the SCStream is exactly what
    // makes macOS 15+/26 re-raise the Screen Recording prompt on Resume, so we
    // stow the live capture in the process-global keepalive instead and the
    // next start_pipeline() adopts it.  (On a genuine app quit the process
    // exits right after; the static keepalive's destructor stops the stream.)
    stow_keepalive_capture();
}

// ---------------------------------------------------------------------------
// Cursor tracking
// ---------------------------------------------------------------------------

namespace {

// Render NSCursor.image into a BGRA byte buffer. Returns false if no image.
// Writes width/height (in pixels), hotspot_x/y (in pixels) on success.
static bool render_cursor_bgra(NSCursor* cursor,
                               std::vector<uint8_t>& out_bgra,
                               uint16_t& out_w, uint16_t& out_h,
                               uint16_t& out_hx, uint16_t& out_hy) {
    if (!cursor) return false;
    NSImage* img = cursor.image;
    if (!img) return false;

    NSSize pt_size = img.size;
    if (pt_size.width <= 0 || pt_size.height <= 0) return false;

    // Pick the largest bitmap representation to avoid blurry upscales.
    CGFloat scale = 1.0;
    for (NSImageRep* rep in img.representations) {
        if (rep.pixelsWide > 0 && rep.size.width > 0) {
            CGFloat s = (CGFloat)rep.pixelsWide / rep.size.width;
            if (s > scale) scale = s;
        }
    }
    const size_t W = (size_t)(pt_size.width  * scale + 0.5);
    const size_t H = (size_t)(pt_size.height * scale + 0.5);
    if (W == 0 || H == 0 || W > 256 || H > 256) return false;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    if (!cs) return false;
    const size_t row_bytes = W * 4;
    out_bgra.assign(W * H * 4, 0);
    CGContextRef ctx = CGBitmapContextCreate(
        out_bgra.data(), W, H, 8, row_bytes, cs,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!ctx) return false;

    NSGraphicsContext* nsctx = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:nsctx];
    [img drawInRect:NSMakeRect(0, 0, W, H)
           fromRect:NSZeroRect
          operation:NSCompositingOperationCopy
           fraction:1.0];
    [NSGraphicsContext restoreGraphicsState];
    CGContextRelease(ctx);

    out_w = (uint16_t)W;
    out_h = (uint16_t)H;
    NSPoint hs = cursor.hotSpot;
    out_hx = (uint16_t)std::max(0.0, std::min((double)W - 1, hs.x * scale));
    out_hy = (uint16_t)std::max(0.0, std::min((double)H - 1, hs.y * scale));
    return true;
}

// Simple 64-bit hash over cursor bitmap + dimensions for change detection.
static uint64_t hash_bgra(const std::vector<uint8_t>& data,
                          uint16_t w, uint16_t h) {
    uint64_t h64 = 1469598103934665603ull; // FNV-1a offset
    auto mix = [&](uint8_t b) {
        h64 ^= b;
        h64 *= 1099511628211ull;
    };
    mix((uint8_t)(w & 0xFF));
    mix((uint8_t)(w >> 8));
    mix((uint8_t)(h & 0xFF));
    mix((uint8_t)(h >> 8));
    // Sample to keep cost low on large cursors: every 4th byte.
    for (size_t i = 0; i < data.size(); i += 4) mix(data[i]);
    return h64;
}

} // namespace

bool MacHostPlatform::get_cursor_state(CursorState& out) {
    @autoreleasepool {
        // NSEvent.mouseLocation is in global screen coordinates with a
        // bottom-left origin. We flip Y to match the host's top-left image
        // space used across the wire protocol.
        NSPoint p = [NSEvent mouseLocation];

        // VIV-95: normalise against the display we are CAPTURING, not the
        // main one.  On a multi-display Mac streaming a secondary screen these
        // are different rectangles, so mainScreen's frame produced coordinates
        // that saturated at an edge and the client's cursor stuck there.
        // NSScreen carries the CGDirectDisplayID under NSScreenNumber, which is
        // what the capture reports.
        NSScreen* screen = nil;
        const uint32_t want_id = capture_ ? capture_->display_id() : 0;
        if (want_id != 0) {
            for (NSScreen* s in [NSScreen screens]) {
                NSNumber* n = s.deviceDescription[@"NSScreenNumber"];
                if (n && (uint32_t)[n unsignedIntValue] == want_id) { screen = s; break; }
            }
        }
        // No capture yet, or the display vanished (unplugged mid-session):
        // mainScreen keeps the cursor roughly sane until capture rebuilds.
        if (!screen) screen = [NSScreen mainScreen];
        if (!screen) return false;
        NSRect frame = screen.frame;
        if (frame.size.width <= 0 || frame.size.height <= 0) return false;

        double x_norm = (p.x - frame.origin.x) / frame.size.width;
        double y_norm = 1.0 - ((p.y - frame.origin.y) / frame.size.height);
        if (x_norm < 0) x_norm = 0; if (x_norm > 1) x_norm = 1;
        if (y_norm < 0) y_norm = 0; if (y_norm > 1) y_norm = 1;

        out.x_norm   = (float)x_norm;
        out.y_norm   = (float)y_norm;
        // The captured desktop cursor is effectively always visible.
        // CGCursorIsVisible was the old signal for a host-hidden cursor
        // (fullscreen games calling CGDisplayHideCursor), but Apple retired
        // it — on macOS 13+/26 it's a deprecated stub that returns false, so
        // it reported the cursor as HIDDEN every tick.  The client took that
        // as "enter relative mode" and hid + clipped the viewer's pointer
        // inside the window even though the Mac cursor was plainly visible
        // (VIV-50).  Report visible; hidden-cursor / relative-input detection
        // for Mac hosts needs a working API (private CGSIsCursorVisible) and
        // is deferred — it only matters for fullscreen 3D apps.
        out.visible  = true;
        out.shape_id = current_shape_id_;

        // Pointer-identity fast-path: if neither the NSCursor instance nor
        // its NSImage changed, skip the full render+hash (CGBitmapContext
        // + drawInRect is O(w*h) and ran on every tick previously).
        NSCursor* cursor = [NSCursor currentSystemCursor];
        std::uintptr_t cursor_id = (std::uintptr_t)(__bridge void*)cursor;
        std::uintptr_t image_id  = cursor
            ? (std::uintptr_t)(__bridge void*)(cursor.image)
            : 0;
        if (cursor_id == last_cursor_id_ && image_id == last_image_id_) {
            return true;
        }
        last_cursor_id_ = cursor_id;
        last_image_id_  = image_id;

        std::vector<uint8_t> bgra;
        uint16_t w = 0, h = 0, hx = 0, hy = 0;
        if (render_cursor_bgra(cursor, bgra, w, h, hx, hy)) {
            uint64_t hash = hash_bgra(bgra, w, h);
            if (hash != last_shape_hash_) {
                last_shape_hash_        = hash;
                current_shape_id_       = ++shape_id_counter_;
                pending_shape_          = true;
                pending_shape_w_        = w;
                pending_shape_h_        = h;
                pending_hotspot_x_      = hx;
                pending_hotspot_y_      = hy;
                pending_shape_bgra_     = std::move(bgra);
                out.shape_id            = current_shape_id_;
            }
        }
    }
    return true;
}

bool MacHostPlatform::take_cursor_shape(CursorShapeView& out) {
    if (!pending_shape_) return false;
    pending_shape_ = false;
    out.id        = current_shape_id_;
    out.width     = pending_shape_w_;
    out.height    = pending_shape_h_;
    out.hotspot_x = pending_hotspot_x_;
    out.hotspot_y = pending_hotspot_y_;
    out.bgra      = std::move(pending_shape_bgra_);
    return true;
}

#endif // VIVORA_MACOS
