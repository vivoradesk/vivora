#pragma once

#include "common/codec/video_codec.h"
#include <cstdint>
#include <cstddef>
#include <vector>

namespace vivora {

// Platform-specific host operations: capture, encode, shutdown.
// One implementation per platform (Windows/DXGI+AMF, macOS/SCK+VTB, Linux/PW+VAAPI).
// The common host loop (run_host_loop) drives the session/bitrate/telemetry logic
// and calls into this interface for platform-specific work.
struct HostPlatform {
    virtual ~HostPlatform() = default;

    // Resolution of the captured display (pixels fed to encoder).
    virtual uint32_t capture_width()  const = 0;
    virtual uint32_t capture_height() const = 0;

    // Resolution in the input coordinate space (may differ from capture,
    // e.g. macOS Retina points vs backing pixels).  Default = capture res.
    virtual uint32_t input_width()  const { return capture_width(); }
    virtual uint32_t input_height() const { return capture_height(); }

    // Encoder control.
    virtual void set_bitrate(uint32_t bps) = 0;
    virtual void request_idr() = 0;

    // The codec the encoder actually produces.  May differ from what the
    // caller requested if the backend had to fall back (e.g. NVENC refusing
    // H.264 and using HEVC instead).  Defaults to HEVC.
    virtual VideoCodec actual_codec() const { return VideoCodec::HEVC; }

    // Capture + encode one frame.  Returns true if a frame was captured
    // (even if encoding produced no output yet).  pts_us receives the
    // presentation timestamp in microseconds.
    //
    // content_changed is set to false when the screen image didn't change
    // (e.g. cursor-only DXGI update).  The caller may skip encoding when
    // !content_changed && !force, but the platform decides the semantics.
    virtual bool capture_and_encode(uint64_t& pts_us,
                                    bool& content_changed,
                                    bool force) = 0;

    // Re-encode the last captured frame to keep wire/FEC alive on idle.
    // Called when one frame interval has passed without a fresh capture
    // (DXGI returns nothing on a truly static screen). The encoder gets
    // the same texture again — should emit a SKIP-type frame (~200 B) so
    // the wire stays warm without burdening FEC with kilobyte intra-refresh
    // payloads on every heartbeat tick.
    // Default no-op for platforms that haven't wired this up yet.
    virtual bool re_encode_last(uint64_t /*pts_us*/) { return false; }

    // Pull the next encoded packet.  Returns false when no more packets
    // are available this iteration.
    struct EncodedPacketView {
        const uint8_t* data = nullptr;
        size_t         len  = 0;
        uint64_t       pts  = 0;
        bool           keyframe = false;
        bool           heartbeat = false;  // skip-frame from re_encode_last
    };
    virtual bool get_encoded_packet(EncodedPacketView& pkt) = 0;

    // Called when no frame was captured (platform may sleep / yield).
    virtual void on_idle() {}

    // Cleanup.
    virtual void shutdown() {}

    // Encoder lifecycle for the lazy-encoder path (Phase B+).
    //   start_encoder() is called when the first viewer attaches.
    //   stop_encoder()  is called when the last viewer drops.
    // Between these calls the platform should release the encoder
    // session (NVENC slot / AMF context / VAAPI surfaces — the
    // expensive GPU state), so a host that's been "Listening" all
    // day uses near-zero GPU until somebody connects.  Capture
    // handles stay open across the gap so reconnect is fast.
    //
    // Default implementations are no-ops, preserving the always-on
    // behaviour for platforms that haven't been wired up yet.
    // start_encoder() returns false on a fatal init failure (e.g. no
    // GPU encoder slot available); host_loop should disconnect the
    // pending client when that happens.
    virtual bool start_encoder() { return true; }
    virtual void stop_encoder() {}

    // Cursor sync (Windows DXGI for now; default no-op elsewhere).
    struct CursorState {
        float    x_norm   = 0.0f;   // 0..1 relative to host screen
        float    y_norm   = 0.0f;
        bool     visible  = false;
        uint32_t shape_id = 0;      // which shape the client should render
    };
    struct CursorShapeView {
        uint32_t id = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        uint16_t hotspot_x = 0;
        uint16_t hotspot_y = 0;
        std::vector<uint8_t> bgra;  // width*height*4 bytes
    };

    // Poll the current cursor position (cheap, once per frame).
    // Returns false if the platform doesn't expose a cursor.
    virtual bool get_cursor_state(CursorState& /*out*/) { return false; }

    // Take the latest cursor shape if it changed since the last call.
    // Returns true when the platform has a new shape to transmit.
    virtual bool take_cursor_shape(CursorShapeView& /*out*/) { return false; }
};

} // namespace vivora
