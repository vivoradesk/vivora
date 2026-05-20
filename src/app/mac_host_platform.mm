#ifdef VIVORA_MACOS

#include "app/mac_host_platform.h"
#include "common/codec/bitrate_controller.h"
#include "common/utils/log.h"
#include <thread>
#include <chrono>
#include <utility>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

bool MacHostPlatform::init(uint32_t display_index,
                            uint32_t manual_bitrate_bps,
                            vivora::VideoCodec codec) {
    // NSCursor / NSScreen need the AppKit shared app initialized. Host mode
    // doesn't create a UI, so make sure the singleton exists.
    [NSApplication sharedApplication];

    if (codec != vivora::VideoCodec::HEVC) {
        vivora::log::warn("HOST", "macOS VideoToolbox currently supports HEVC only; --codec=h264 ignored");
    }
    auto displays = vivora::host::MacScreenCapture::enumerate_displays();
    if (displays.empty()) {
        vivora::log::error("HOST", "No displays found (check Screen Recording permission)");
        return false;
    }
    vivora::log::info("HOST", "Available displays:");
    for (const auto& d : displays) {
        vivora::log::info("HOST", "  [%u] %s %s", d.index, d.name.c_str(),
                            d.hdr_capable ? "(HDR capable)" : "");
    }
    if (display_index >= displays.size()) {
        vivora::log::error("HOST", "Display index %u out of range", display_index);
        return false;
    }

    vivora::host::MacCaptureConfig ccfg;
    ccfg.display_index = display_index;
    ccfg.fps = 60;
    ccfg.show_cursor = false;
    // Auto-detect: capture HDR if the display reports HDR, otherwise SDR.
    // MacScreenCapture honours prefer_hdr only when display_is_hdr() agrees.
    ccfg.prefer_hdr = true;
    if (!capture_.init(ccfg)) {
        vivora::log::error("HOST", "Failed to init capture");
        return false;
    }
    if (!capture_.start()) {
        vivora::log::error("HOST", "Failed to start capture");
        return false;
    }

    uint32_t bitrate = manual_bitrate_bps;
    if (bitrate == 0)
        bitrate = vivora::codec::default_bitrate_for(capture_.width(), capture_.height(), 60);

    vivora::host::MacEncoderConfig ecfg;
    ecfg.width = capture_.width();
    ecfg.height = capture_.height();
    ecfg.fps = 60;
    ecfg.bitrate_bps = bitrate;
    ecfg.idr_period = 120;
    ecfg.hdr = capture_.hdr_active();
    if (!encoder_.init(ecfg)) {
        vivora::log::error("HOST", "Failed to init encoder");
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
    encoder_.shutdown();
    capture_.stop();
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

        NSScreen* screen = [NSScreen mainScreen];
        if (!screen) return false;
        NSRect frame = screen.frame;
        if (frame.size.width <= 0 || frame.size.height <= 0) return false;

        double x_norm = (p.x - frame.origin.x) / frame.size.width;
        double y_norm = 1.0 - ((p.y - frame.origin.y) / frame.size.height);
        if (x_norm < 0) x_norm = 0; if (x_norm > 1) x_norm = 1;
        if (y_norm < 0) y_norm = 0; if (y_norm > 1) y_norm = 1;

        out.x_norm   = (float)x_norm;
        out.y_norm   = (float)y_norm;
        // CGCursorIsVisible is imperfect (Apple flags it "with known issues"
        // on 10.9+) but catches the primary target — fullscreen games that
        // call CGDisplayHideCursor. Client-side 50ms debouncer absorbs brief
        // flicker. Private CGSIsCursorVisible is a future upgrade if needed.
        out.visible  = CGCursorIsVisible() ? true : false;
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
