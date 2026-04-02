#pragma once

#include "host/capture/frame.h"
#include <memory>
#include <vector>
#include <functional>

namespace deskbeam {

// Platform-independent screen capture interface
class IScreenCapture {
public:
    virtual ~IScreenCapture() = default;

    // Initialize capture for the given monitor index
    virtual bool init(uint32_t monitor_index = 0) = 0;

    // Capture a frame. Returns false if no new frame is available
    // (e.g., screen hasn't changed). timeout_ms=0 means non-blocking.
    virtual bool capture_frame(CapturedFrame& frame, uint32_t timeout_ms = 100) = 0;

    // Release resources from a previously captured frame
    virtual void release_frame(CapturedFrame& frame) = 0;

    // Enumerate available monitors
    virtual std::vector<MonitorInfo> enumerate_monitors() = 0;

    // Get current capture resolution
    virtual Resolution get_resolution() const = 0;

    // Factory: create platform-appropriate capture instance
    static std::unique_ptr<IScreenCapture> create();
};

} // namespace deskbeam
