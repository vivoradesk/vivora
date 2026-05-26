#pragma once

#ifdef VIVORA_MACOS

#include "common/utils/types.h"
#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vivora::host {

struct MacDisplayInfo {
    uint32_t index = 0;          // index as passed to init()
    uint32_t display_id = 0;     // CGDirectDisplayID
    uint32_t width_px = 0;       // native pixels
    uint32_t height_px = 0;
    std::string name;
    bool hdr_capable = false;
};

struct MacCaptureConfig {
    uint32_t display_index = 0;
    uint32_t fps = 60;
    bool show_cursor = false;
    // Request 10-bit HDR capture. If the selected display is SDR, falls back to 8-bit.
    bool prefer_hdr = false;
};

// Low-latency screen capture via ScreenCaptureKit.
// Frames are delivered on an internal dispatch queue; the main loop pulls
// the latest frame via try_get_frame() (drop-oldest semantics — a backlog is
// never built up, old frames are replaced by newer ones before consumption).
class MacScreenCapture {
public:
    MacScreenCapture();
    ~MacScreenCapture();

    MacScreenCapture(const MacScreenCapture&) = delete;
    MacScreenCapture& operator=(const MacScreenCapture&) = delete;

    // Synchronous display enumeration. Blocks briefly on the SCShareableContent API.
    static std::vector<MacDisplayInfo> enumerate_displays();

    bool init(const MacCaptureConfig& config);
    bool start();
    void stop();

    // Try to get the most recent captured frame. Returns nullptr if no new frame
    // has arrived since the last call. The caller owns the returned reference
    // and must CFRelease() it when done.
    CVPixelBufferRef try_get_frame(uint64_t* out_pts_us);

    // Force-return the LAST delivered frame even if it was already pulled.
    // Used by the IDR-on-loss path on Mac: SCK doesn't emit new frames when
    // screen content is static, but the host_loop still needs SOMETHING to
    // hand the encoder so it can produce the requested keyframe.  Returns
    // nullptr only if no frame has EVER been delivered (cold-start before
    // the first capture).  Caller owns the reference and must CFRelease().
    CVPixelBufferRef get_last_frame_for_force(uint64_t* out_pts_us);

    // Backing pixel dimensions of the captured stream (what the encoder sees).
    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    // Display's logical "points" dimensions — CGEventPost / input coords
    // live in this space, not in backing pixels.
    uint32_t points_width()  const { return points_w_; }
    uint32_t points_height() const { return points_h_; }
    bool hdr_active() const { return hdr_active_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t points_w_ = 0;
    uint32_t points_h_ = 0;
    bool hdr_active_ = false;
};

} // namespace vivora::host

#endif // VIVORA_MACOS
