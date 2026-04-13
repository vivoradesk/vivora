#pragma once

#include "common/protocol/input_event.h"
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

    // Decode an assembled frame.  Called only after keyframe gating passes.
    virtual bool decode(const uint8_t* data, size_t len,
                        uint32_t timestamp, bool keyframe,
                        uint16_t seq_no) = 0;

    // Render any decoded output.  Returns number of frames rendered.
    virtual int render() = 0;

    // Called when decoder should discard stale reference frames (after drop).
    virtual void flush_decoder() {}

    // Cleanup.
    virtual void shutdown() {}
};

} // namespace deskbeam
