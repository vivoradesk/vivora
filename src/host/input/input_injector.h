#pragma once

#include "common/protocol/input_event.h"
#include <cstdint>
#include <memory>

namespace vivora::host {

// Platform-independent input injector interface.
class InputInjector {
public:
    virtual ~InputInjector() = default;

    // Host screen resolution, used to convert normalized coords to absolute.
    virtual void set_screen_resolution(uint32_t width, uint32_t height) = 0;

    // Virtual-desktop origin of the captured display (VIV-50).  Default
    // no-op: Mac/Linux hosts capture a single display today; the Windows
    // injector uses it to land absolute coords on a non-primary monitor.
    virtual void set_screen_origin(int32_t /*x*/, int32_t /*y*/) {}

    // Inject a single input event into the host OS.
    virtual void inject(const protocol::InputEvent& event) = 0;

    // Factory: create the platform-appropriate injector.
    static std::unique_ptr<InputInjector> create();
};

} // namespace vivora::host
