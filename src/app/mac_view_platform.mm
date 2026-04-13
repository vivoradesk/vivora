#ifdef DESKBEAM_MACOS

#include "app/mac_view_platform.h"
#include "common/utils/log.h"
#include <utility>

bool MacViewPlatform::init(const char* /*host_ip*/, uint16_t /*port*/) {
    if (!view_.create_window("DeskBeam", 1280, 720)) {
        deskbeam::log::error("VIEW", "Failed to create window");
        return false;
    }
    return true;
}

void MacViewPlatform::set_input_callback(InputCallback cb) {
    view_.set_input_callback(std::move(cb));
}

bool MacViewPlatform::pump_events() {
    view_.pump_events();
    return !view_.should_close();
}

bool MacViewPlatform::decode(const uint8_t* data, size_t len,
                              uint32_t timestamp, bool keyframe,
                              uint16_t /*seq_no*/) {
    return view_.submit_frame(data, len, timestamp, keyframe);
}

int MacViewPlatform::render() {
    // Mac view decodes + renders inside submit_frame, so each
    // successful decode() call is also a render.  Return 0 here
    // to avoid double-counting — the common loop counts decode().
    return 0;
}

void MacViewPlatform::shutdown() {}

#endif // DESKBEAM_MACOS
