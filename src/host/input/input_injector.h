#pragma once

#include "common/protocol/input_event.h"
#include <cstdint>
#include <memory>

namespace deskbeam::host {

// Platform-independent input injector interface.
class InputInjector {
public:
    virtual ~InputInjector() = default;

    // Host screen resolution, used to convert normalized coords to absolute.
    virtual void set_screen_resolution(uint32_t width, uint32_t height) = 0;

    // Inject a single input event into the host OS.
    virtual void inject(const protocol::InputEvent& event) = 0;

    // Factory: create the platform-appropriate injector.
    static std::unique_ptr<InputInjector> create();
};

} // namespace deskbeam::host
