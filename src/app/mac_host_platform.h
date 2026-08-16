#pragma once
#ifdef VIVORA_MACOS

#include "app/host_platform.h"
#include "host/capture/mac_screen_capture.h"
#include "host/encode/mac_videotoolbox_encoder.h"
#include "host/encode/video_encoder.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class MacHostPlatform : public vivora::HostPlatform {
public:
    // `stream_fps` is the VIV-67 framerate cap — it drives both the SCK
    // capture minimum-interval and the VideoToolbox expected rate.
    // Defaulted so the legacy CLI keeps compiling.
    bool init(uint32_t display_index, uint32_t manual_bitrate_bps,
              vivora::VideoCodec codec, uint16_t stream_fps = 60);

    // Human-readable reason set when init() fails, so the GUI worker can
    // surface the real cause instead of a generic string (VIV-111).  Empty
    // when init() succeeded.
    const std::string& init_error() const { return init_error_; }

    uint32_t capture_width()  const override;
    uint32_t capture_height() const override;
    uint32_t input_width()    const override;
    uint32_t input_height()   const override;

    void set_bitrate(uint32_t bps) override;
    void request_idr() override;

    // Report the codec the VideoToolbox session actually emits so host_loop
    // can advertise it to the viewer. HEVC by default; H.264 when negotiated
    // as the fallback (VIV-7). This mirrors what the encoder was built with,
    // not a re-negotiation.
    vivora::VideoCodec actual_codec() const override { return codec_; }

    // Monitor selection (VIV-50).  ScreenCaptureKit enumerates displays
    // directly (unlike the Linux portal), so this mirrors the Windows path:
    // stop capture+encoder, re-init capture on the new SCDisplay, rebuild the
    // encoder at the new resolution.
    std::vector<vivora::protocol::MonitorDesc> list_monitors() override;
    bool select_monitor(uint32_t index, bool seed_cursor) override;

    // Phase B+ lazy encoder (VIV-12).  start_encoder() (re)creates the
    // VideoToolbox session from the live capture geometry + remembered
    // bitrate when the first viewer attaches; stop_encoder() invalidates
    // it when the last viewer drops.  ScreenCaptureKit capture is
    // deliberately NOT stopped across the gap — VIV-95 tracks live races
    // in the SCK stop/restart paths, and the VT session is the GPU cost
    // anyway.  Frames landing while the encoder is down are simply never
    // pulled from the capture's latest-frame slot (capture_and_encode
    // early-returns), which is free.
    bool start_encoder() override;
    void stop_encoder() override;

    bool capture_and_encode(uint64_t& pts_us,
                            bool& content_changed,
                            bool force) override;
    bool re_encode_last(uint64_t pts_us) override;
    bool get_encoded_packet(EncodedPacketView& out) override;
    void on_idle() override;
    void shutdown() override;

    bool get_cursor_state(CursorState& out) override;
    bool take_cursor_shape(CursorShapeView& out) override;

private:
    // VIV-116: heap-owned so the live ScreenCaptureKit session can be moved
    // into a process-global keepalive slot on pause and adopted back on
    // resume WITHOUT relocating the MacScreenCapture object.  The SCK stream
    // delegate + wake observer capture `this` (the MacScreenCapture*), so the
    // pointee address must stay stable across a stow/adopt — moving a
    // unique_ptr transfers the pointer but never moves the object, which a
    // by-value member could not guarantee.  Keeping the session alive across a
    // user-initiated pause means Resume never creates a fresh SCStream, so
    // macOS does not re-raise the Screen Recording prompt.
    std::unique_ptr<vivora::host::MacScreenCapture> capture_;
    vivora::host::MacVideoToolboxEncoder encoder_;
    std::vector<uint8_t> pkt_buf_;
    uint32_t current_display_index_ = 0;     // captured SCDisplay index (VIV-50)
    uint32_t manual_bitrate_bps_    = 0;     // user-pinned bitrate (0 = auto)
    uint16_t stream_fps_            = 60;    // VIV-67 framerate cap
    std::string init_error_;                 // reason on init() failure (VIV-111)
    // Negotiated wire codec (VIV-7). HEVC by default; a viewer whose decoder
    // can't init HEVC negotiates H.264, and start_encoder() honours it.
    vivora::VideoCodec codec_       = vivora::VideoCodec::HEVC;
    // Lazy-encoder state (VIV-12).  encoder_live_ mirrors whether the VT
    // session exists; live_bitrate_bps_ caches the last set_bitrate() so a
    // stop/start cycle resumes at the adaptive controller's last rate.
    // All accessed on the host_loop thread only — no locking needed.
    bool     encoder_live_    = false;
    uint32_t live_bitrate_bps_ = 0;
    // (Re)build capture for `display_index`.  Shared by init() and
    // select_monitor().  The encoder is built separately by start_encoder()
    // when a viewer is attached (Phase B+).  Returns false leaving the
    // object unusable on failure.  First tries to adopt a live SCK session
    // stowed by a previous pause (VIV-116); only creates a fresh SCStream —
    // and thus risks a Screen Recording re-prompt — when nothing compatible
    // is stowed.
    bool start_pipeline(uint32_t display_index);

    // VIV-116 keepalive helpers.  adopt() moves a compatible stowed capture
    // into capture_ (returns true on adoption); stow() moves the live capture
    // out to the global keepalive so Resume can adopt it instead of building a
    // new SCStream.  A compatible capture matches both the display index and
    // the requested stream fps (the only capture-side config that varies).
    bool adopt_keepalive_capture(uint32_t display_index);
    void stow_keepalive_capture();

    // Cursor tracking state.
    uint64_t  last_shape_hash_ = 0;
    uint32_t  shape_id_counter_ = 0;
    uint32_t  current_shape_id_ = 0;
    bool      pending_shape_    = false;
    uint16_t  pending_shape_w_  = 0;
    uint16_t  pending_shape_h_  = 0;
    uint16_t  pending_hotspot_x_ = 0;
    uint16_t  pending_hotspot_y_ = 0;
    std::vector<uint8_t> pending_shape_bgra_;

    // Pointer-identity cache: skip expensive CGBitmapContext render when
    // NSCursor and its NSImage are the same objects as last tick. Plain
    // uintptr_t — no retain: system cursors are Apple singletons, and app
    // custom cursors outlive the window of a single tick. Hash comparison
    // is the fallback if the ABA problem ever occurs.
    std::uintptr_t last_cursor_id_ = 0;
    std::uintptr_t last_image_id_  = 0;
};

#endif // VIVORA_MACOS
