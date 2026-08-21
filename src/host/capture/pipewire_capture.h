// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#ifdef VIVORA_LINUX

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vivora::host {

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

    // Cursor metadata as parsed from PipeWire's SPA_META_Cursor on each
    // process callback.  Position is normalised 0..1 in capture pixels.
    // Bitmap is BGRA, present only when the host changes cursor shape.
    struct CursorState {
        float    x_norm = 0.0f;
        float    y_norm = 0.0f;
        bool     visible = false;
        // Stable id assigned per unique shape (incremented when bitmap
        // contents differ from the previous one).  0 = no shape yet.
        uint32_t shape_id = 0;
    };
    struct CursorShape {
        uint32_t id = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        uint16_t hotspot_x = 0;
        uint16_t hotspot_y = 0;
        std::vector<uint8_t> bgra;  // width*height*4
    };

    // Empty when screen capture is usable.  Otherwise one sentence the user
    // can act on -- today that means libpipewire is not installed (VIV-126).
    // Cheap and side-effect free; safe to call before constructing anything.
    static std::string unavailable_reason();

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

    // Choose the portal cursor mode for the next start().  embedded=true
    // (default) asks the portal to draw the cursor into the captured pixels
    // (cursor_mode EMBEDDED); embedded=false asks it to leave the cursor out
    // (cursor_mode HIDDEN) — used when the host paints the cursor itself from
    // X11 (X11Cursor) so it isn't drawn twice / frozen (VIV-66).  Must be
    // called before start().
    void set_cursor_embedded(bool embedded);

    // Begin streaming (start the PipeWire thread loop).  Must be called
    // after init() succeeds.
    bool start();

    // Stop streaming, tear down PipeWire and the portal session.
    void stop();

    uint32_t width()  const;
    uint32_t height() const;

    // True once at least one valid SPA_META_Cursor has been observed.
    // Until that happens cursor_state() returns the default (visible=false)
    // value, which the host_loop must NOT publish — clients would
    // misinterpret it as "host hid the cursor" and switch to relative-input
    // / cursor-grab mode.
    bool has_cursor() const;
    // Latest cursor position/visibility as observed in the most recent
    // SPA_META_Cursor.  Cheap to call every frame.
    CursorState cursor_state() const;
    // If the host has changed cursor shape since the last call, fill
    // `out` with the new bitmap and return true.  Same retry-on-loss
    // semantics as the Windows / macOS take_new_cursor_shape methods.
    bool take_new_cursor_shape(CursorShape& out);

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace vivora::host

#endif // VIVORA_LINUX
