#pragma once

#include "app/host_platform.h"
#include <cstdint>

namespace deskbeam {

struct HostLoopConfig {
    uint16_t port = 9876;
    uint32_t manual_bitrate_bps = 0;   // 0 = auto from resolution
};

// Run the host main loop.  Blocks until the client disconnects.
// All platform-specific work is delegated to |platform|.
int run_host_loop(HostPlatform& platform, const HostLoopConfig& cfg);

} // namespace deskbeam
