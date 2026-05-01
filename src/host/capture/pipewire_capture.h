#pragma once

#ifdef DESKBEAM_LINUX

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace deskbeam::host {

// PipeWire-based screen capture for Linux.  Acquires permission to capture
// via xdg-desktop-portal (org.freedesktop.portal.ScreenCast) — works on
// both Wayland (where there is no other path) and X11 (compositors expose
// a PipeWire remote too).
//
// Stage 1 deliverable: opens portal session, connects to PipeWire, and
// invokes the frame callback with metadata for each arriving buffer.
// DMA-BUF import + VAAPI surface mapping is left to Stage 3.
//
// Lifetime: init() drives the portal request flow (synchronous from the
// caller's POV; under the hood it pumps a private dbus loop until the
// portal answers).  start() arms the PipeWire thread loop.  Frames arrive
// on a background thread until stop().
class PipeWireCapture {
public:
    struct Frame {
        // -1 if the buffer arrived via SHM rather than DMA-BUF.
        int dmabuf_fd = -1;
        uint32_t width = 0;
        uint32_t height = 0;
        // SPA video format (e.g. SPA_VIDEO_FORMAT_BGRA, _NV12, _RGBx).
        uint32_t format = 0;
        // Plane stride in bytes (for SHM); 0 for DMA-BUF (modifier-dependent).
        uint32_t stride = 0;
        // Mapped pixel data for SHM frames (PW_STREAM_FLAG_MAP_BUFFERS).
        // null when dmabuf_fd >= 0.  Valid only for the duration of the
        // callback — caller must encode/copy synchronously.
        const uint8_t* data = nullptr;
        size_t         size = 0;
        // Wall-clock at frame arrival, in nanoseconds.
        uint64_t pts_ns = 0;
    };

    using FrameCallback = std::function<void(const Frame&)>;

    PipeWireCapture();
    ~PipeWireCapture();

    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;

    // Forward-declared public so the .cpp can hand a pointer to it to
    // the C callbacks PipeWire / dbus expect, without making the user-
    // facing API depend on libpipewire / libdbus headers.
    struct Impl;

    // Drive the portal request flow.  Blocks until the user accepts the
    // permission dialog (first run only — portal remembers the grant).
    // Returns false on portal error or user denial.  cb is invoked from
    // the PipeWire thread once start() succeeds.
    bool init(FrameCallback cb);

    // Begin streaming (start the PipeWire thread loop).  Must be called
    // after init() succeeds.
    bool start();

    // Stop streaming, tear down PipeWire and the portal session.
    void stop();

    uint32_t width()  const;
    uint32_t height() const;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace deskbeam::host

#endif // DESKBEAM_LINUX
