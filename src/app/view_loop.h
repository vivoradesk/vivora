#pragma once

#include "app/view_platform.h"
#include <cstdint>

namespace deskbeam {

struct ViewLoopConfig {
    const char* host_ip = nullptr;
    uint16_t    port    = 9876;
};

// Run the view (client) main loop.  Blocks until disconnected or window closed.
// Platform-specific decode/render is delegated to |platform|.
int run_view_loop(ViewPlatform& platform, const ViewLoopConfig& cfg);

} // namespace deskbeam
