#pragma once

#ifdef DESKBEAM_MACOS

#include "common/protocol/input_event.h"

#include <cstdint>
#include <cstddef>
#include <functional>

namespace deskbeam {

// Cocoa window hosting an AVSampleBufferDisplayLayer.
// Accepts HEVC Annex-B encoded frames and displays them.
class MacVideoView {
public:
    MacVideoView();
    ~MacVideoView();

    MacVideoView(const MacVideoView&) = delete;
    MacVideoView& operator=(const MacVideoView&) = delete;

    // Create window and activate NSApplication. Must be called from main thread.
    bool create_window(const char* title, uint32_t width, uint32_t height);

    // Feed one HEVC frame (Annex-B format with 00 00 00 01 start codes).
    // Keyframe frames should contain VPS/SPS/PPS + IDR NAL units.
    // Returns false if the frame was dropped (e.g. no format yet on non-keyframe).
    bool submit_frame(const uint8_t* data, size_t len, uint64_t pts_us, bool keyframe);

    // Pump NSApplication events non-blocking. Call regularly from the main loop.
    void pump_events();

    // True if the user closed the window.
    bool should_close() const { return should_close_; }

    // Set input event callback. Called from main thread during pump_events().
    using InputCallback = std::function<void(const protocol::InputEvent&)>;
    void set_input_callback(InputCallback cb);

private:
    void* impl_;           // opaque pointer to Obj-C++ impl struct
    bool should_close_ = false;
};

} // namespace deskbeam

#endif // DESKBEAM_MACOS
