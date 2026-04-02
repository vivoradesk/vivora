#pragma once

#include "common/utils/types.h"

#ifdef DESKBEAM_WINDOWS
#include <d3d11.h>
#endif

#include <cstdint>
#include <vector>

namespace deskbeam {

// GPU frame captured from screen
struct CapturedFrame {
    Resolution resolution;
    uint64_t frame_index = 0;
    TimePoint capture_time;

#ifdef DESKBEAM_WINDOWS
    // The texture stays in GPU memory — no CPU copy
    ID3D11Texture2D* texture = nullptr;
#endif

    // Dirty rectangles — regions that changed since last frame
    std::vector<Rect> dirty_rects;

    // Cursor info
    struct {
        int32_t x = 0;
        int32_t y = 0;
        bool visible = false;
    } cursor;
};

// Monitor info for multi-monitor enumeration
struct MonitorInfo {
    uint32_t index = 0;
    std::string name;
    Resolution resolution;
    Rect bounds;
    bool primary = false;
};

} // namespace deskbeam
