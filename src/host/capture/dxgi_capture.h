#pragma once

#ifdef DESKBEAM_WINDOWS

#include "host/capture/screen_capture.h"
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

namespace deskbeam {

using Microsoft::WRL::ComPtr;

// Cursor bitmap + hotspot, normalized to BGRA regardless of the DXGI
// shape type (color / monochrome / masked-color).
struct CursorShape {
    uint32_t id = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t hotspot_x = 0;
    uint16_t hotspot_y = 0;
    std::vector<uint8_t> bgra;  // width * height * 4 bytes
};

class DxgiCapture : public IScreenCapture {
public:
    DxgiCapture();
    ~DxgiCapture() override;

    bool init(uint32_t monitor_index = 0) override;
    bool capture_frame(CapturedFrame& frame, uint32_t timeout_ms = 100) override;
    void release_frame(CapturedFrame& frame) override;
    std::vector<MonitorInfo> enumerate_monitors() override;
    Resolution get_resolution() const override;

    // Access to D3D11 device (needed by encoder for zero-copy)
    ID3D11Device* get_device() const { return device_.Get(); }
    ID3D11DeviceContext* get_context() const { return context_.Get(); }
    DXGI_FORMAT get_capture_format() const { return capture_format_; }

    // Current cursor shape id that the last captured frame refers to. 0
    // means no shape has ever been observed.
    uint32_t current_shape_id() const { return current_shape_id_; }

    // If DXGI reported a new cursor shape since the last call, move it into
    // |out| and return true. The caller is expected to send it to clients.
    bool take_new_cursor_shape(CursorShape& out);

private:
    bool init_d3d11();
    bool init_output_duplication(uint32_t monitor_index);
    void update_cursor_shape(UINT buffer_size);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<IDXGIOutput5> output5_;
    DXGI_FORMAT capture_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;

    Resolution resolution_;
    uint64_t frame_count_ = 0;
    bool frame_acquired_ = false;

    // Cursor shape state. DXGI only hands us pointer pixels on change,
    // so we cache the latest shape and hand it to the sender on demand.
    uint32_t current_shape_id_ = 0;
    bool     new_shape_pending_ = false;
    CursorShape pending_shape_;
    std::vector<uint8_t> shape_scratch_;  // reused buffer for GetFramePointerShape

    // Sticky cursor position/visibility. DXGI fills PointerPosition only on
    // frames where the cursor actually moved (LastMouseUpdateTime != 0) —
    // but downstream consumers want to know where the cursor is *now* on
    // every frame, so we carry the last known state forward.
    int32_t sticky_cursor_x_ = 0;
    int32_t sticky_cursor_y_ = 0;
    bool    sticky_cursor_visible_ = true;
};

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
