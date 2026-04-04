#pragma once

#ifdef DESKBEAM_WINDOWS

#include "common/protocol/input_event.h"
#include <cstdint>

namespace deskbeam::host {

class InputInjector {
public:
    // Set host screen resolution for absolute coordinate mapping
    void set_screen_resolution(uint32_t width, uint32_t height);

    // Inject a single input event
    void inject(const protocol::InputEvent& event);

private:
    uint32_t screen_w_ = 1920;
    uint32_t screen_h_ = 1080;
};

} // namespace deskbeam::host

#endif // DESKBEAM_WINDOWS
