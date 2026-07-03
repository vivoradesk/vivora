#pragma once

#include "common/codec/video_codec.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/input_event.h"
#include "common/protocol/stream_info.h"
#include <cstdint>
#include <cstddef>
#include <functional>

namespace vivora {

class IVideoPipeline;  // app/video_pipeline.h (threaded pipeline, VIV-81)

// Platform-specific view (client) operations: windowing, decode, render.
// One implementation per platform.
// The common view loop (run_view_loop) drives session/IDR-recovery/keyframe
// gating and calls into this interface for platform work.
// Live diagnostics surfaced to the user through an optional HUD overlay.
// Populated by the view loop once per second from session + decoder stats
// and pushed to the platform via `update_stats`.  Toggled with F9; default
// hidden so the typical user never sees it.
struct StatsView {
    float    fps        = 0.0f;   // decoded frames per second
    float    arrived_fps= 0.0f;   // network-assembled frames per second
    float    rtt_ms     = 0.0f;   // round-trip time to host
    uint32_t bitrate_kbps = 0;    // inbound (actual wire) bitrate from main socket
    uint32_t encoding_kbps = 0;   // host's encoder target bitrate (HostStats)
    float    reject_pct = 0.0f;   // decoder reject rate over last 1s
    float    drop_pct   = 0.0f;   // network frame drop rate over last 1s
    uint16_t target_fps = 60;     // current adaptive framerate target
    uint32_t plc_pct    = 0;      // audio PLC rate over last 1s (×100)
    uint32_t audio_pps  = 0;      // audio packets per second
    uint64_t total_rejected = 0;  // cumulative decoder rejections
    uint64_t total_dropped  = 0;  // cumulative network drops
    uint64_t fec_recovered  = 0;  // cumulative FEC-recovered packets
    uint64_t fec_groups_failed = 0;  // FEC groups that exceeded M parity
    uint32_t width      = 0;      // decoded frame width
    uint32_t height     = 0;      // decoded frame height
    bool     hdr        = false;  // BT.2020 + PQ if true
    char     decoder[16] = {0};   // backend name: "SW HEVC", "VAAPI HEVC", etc.
    // Extra fields for the in-stream menu header (VIV-74).
    char     codec[8]    = {0};   // negotiated codec: "HEVC", "H.264"
    char     transport[8]= {0};   // "P2P" or "Relay"
    uint32_t session_seconds = 0; // elapsed since the session connected
};

// Callbacks the in-stream overlay menu (VIV-74) invokes when the user
// changes a setting.  Wired by the view loop (which owns the session) and
// handed to the platform, which passes them to its menu widget.  Any field
// may be empty; the menu guards before calling.
struct MenuActions {
    std::function<void(float)> set_volume;     // linear gain 0..1
    std::function<void(bool)>  set_muted;
    std::function<void(bool)>  set_view_only;  // true = stop forwarding input
    std::function<void()>      disconnect;     // end the session
};

struct ViewPlatform {
    virtual ~ViewPlatform() = default;

    using InputCallback = std::function<void(const protocol::InputEvent&)>;

    // Wire input callback (window events → session → host).
    virtual void set_input_callback(InputCallback cb) = 0;

    // Process windowing events.  Returns false if the window was closed.
    virtual bool pump_events() = 0;

    // Lazily initialize the decoder for the negotiated codec (known only
    // after the handshake). Idempotent — subsequent calls with the same
    // codec are no-ops.  Returns true if decoder is ready.
    virtual bool init_decoder(VideoCodec codec) = 0;

    // Decode an assembled frame.  Called only after keyframe gating passes.
    virtual bool decode(const uint8_t* data, size_t len,
                        uint32_t timestamp, bool keyframe,
                        uint16_t seq_no) = 0;

    // Render any decoded output.  Returns number of frames rendered.
    virtual int render() = 0;

    // Called when decoder should discard stale reference frames (after drop).
    virtual void flush_decoder() {}

    // Upload a new cursor shape. Called once per shape_id; the platform caches
    // it keyed by shape_id so subsequent position updates can reference it.
    virtual void upload_cursor_shape(const protocol::CursorShapeMessage& /*shape*/) {}
    // Update the current cursor position / visibility. Called every frame.
    virtual void update_cursor_position(const protocol::CursorPositionMessage& /*pos*/) {}

    // Notify the platform of the real (pre-encoder-padding) stream size.
    // The decoded texture may be larger due to codec alignment (QSV rounds
    // to 16 pixels); these are the authoritative content dims.
    virtual void set_stream_size(uint32_t /*width*/, uint32_t /*height*/) {}

    // Push a diagnostics snapshot to the HUD overlay (no-op if HUD off).
    // Called by the view loop ~once per second.
    virtual void update_stats(const StatsView& /*stats*/) {}

    // Centred status overlay shown before the first frame arrives — e.g.
    // "Connecting…", "Waiting for host to accept…", or a close reason.
    // Empty string hides it.  No-op on platforms without an overlay yet.
    virtual void set_status(const char* /*text*/) {}

    // Provide the callbacks the in-stream menu (VIV-74) invokes.  No-op on
    // platforms that don't implement the overlay menu yet (Linux/macOS).
    virtual void set_menu_actions(const MenuActions& /*actions*/) {}

    // Threaded pipeline (VIV-81): the platform's decode→present split, used
    // only when VIVORA_PIPELINE=threaded.  Returns null on the legacy path
    // and on platforms that don't implement it yet.
    virtual IVideoPipeline* video_pipeline() { return nullptr; }

    // Cleanup.
    virtual void shutdown() {}
};

} // namespace vivora
