#pragma once

#include "common/utils/types.h"

#ifdef DESKBEAM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
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
    // The texture stays in GPU memory — no CPU copy.  ComPtr takes an
    // explicit ref when the capturer hands it out, so the caller can't
    // accidentally outlive the DXGI frame by one call to ReleaseFrame().
    // Consumers needing a raw ID3D11Texture2D* for D3D11 APIs use
    // `texture.Get()` — the ref is held for the lifetime of CapturedFrame.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
#endif

    // True if desktop image content changed (not just cursor movement)
    bool content_changed = false;

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
