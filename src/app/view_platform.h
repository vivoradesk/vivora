#pragma once

#include "common/codec/video_codec.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/input_event.h"
#include "common/protocol/stream_info.h"
#include <cstdint>
#include <cstddef>
#include <functional>

namespace deskbeam {

// Platform-specific view (client) operations: windowing, decode, render.
// One implementation per platform.
// The common view loop (run_view_loop) drives session/IDR-recovery/keyframe
// gating and calls into this interface for platform work.
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

    // Cleanup.
    virtual void shutdown() {}
};

} // namespace deskbeam
