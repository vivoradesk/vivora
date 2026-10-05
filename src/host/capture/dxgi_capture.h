// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#ifdef VIVORA_WINDOWS

#include "host/capture/screen_capture.h"
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <string>

namespace vivora {

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

    // Runtime monitor switch (VIV-50).  Tears down the current output
    // duplication and re-creates it for |monitor_index| on the SAME D3D11
    // device, so an encoder built on get_device() stays valid (provided both
    // displays hang off the same adapter — multi-GPU switching would need a
    // device rebuild, not supported here).  Updates get_resolution() and the
    // capture format.  Returns false if the new output can't be duplicated.
    bool switch_monitor(uint32_t monitor_index);
    uint32_t current_monitor_index() const { return monitor_index_; }

    // Display hot-plug / mode change (VIV-147).  DXGI output enumeration is
    // frozen at the factory that created the device: a monitor attached or
    // detached after startup never shows up in EnumOutputs, and a mode change
    // leaves DXGI_OUTPUT_DESC reporting the OLD desktop rectangle.  Polling
    // this (cheap: IDXGIFactory1::IsCurrent plus one output desc read) both
    // refreshes our factory/adapter so enumerate_monitors() sees the new
    // topology, and reports whether anything actually moved.
    //
    // Returns true once per detected change.  When the *captured* display's
    // geometry changed, the duplication is dropped as well so the next
    // capture_frame() re-creates it at the new size — the caller must rebuild
    // its encoder (get_resolution() has already been updated).
    bool poll_display_change();

    // True (once) when the captured display's own geometry changed since the
    // last call — as opposed to some other display merely coming or going.
    // Set by ANY re-duplication that lands on a different size, including the
    // silent one capture_frame() runs after DXGI_ERROR_ACCESS_LOST, so a mode
    // change between two polls is not lost.  The caller must rebuild its
    // encoder when this returns true.
    bool take_geometry_change();

    // Desktop origin of the captured display (top-left corner in virtual
    // desktop coordinates).  (0,0) for the primary; non-primary displays
    // sit at an offset that input injection must add (VIV-50).
    int32_t origin_x() const { return origin_x_; }
    int32_t origin_y() const { return origin_y_; }

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

    // Re-create the DXGI factory when it has gone stale (IsCurrent() == false)
    // and re-resolve our device's adapter inside it by LUID.  Every output
    // enumeration goes through here, so hot-plugged displays become visible
    // without restarting the host.  Returns false if the adapter is gone.
    bool ensure_factory();

    // Reconcile resolution_ with the size duplication actually produces.
    void adopt_duplication_geometry(const DXGI_OUTDUPL_DESC& dup_desc);

    // The only writer of resolution_: records the new size and latches
    // geometry_dirty_ so the encoder gets rebuilt for it.
    void note_resolution(uint32_t w, uint32_t h);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIFactory1> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    LUID adapter_luid_ = {};
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<IDXGIOutput5> output5_;
    DXGI_FORMAT capture_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;

    Resolution resolution_;
    int32_t origin_x_ = 0;   // captured display's virtual-desktop origin (VIV-50)
    int32_t origin_y_ = 0;
    uint64_t frame_count_ = 0;
    bool frame_acquired_ = false;
    // Saved so capture_frame() can silently re-run init_output_duplication()
    // after a DXGI_ERROR_ACCESS_LOST (exclusive-fullscreen enter/exit, UAC
    // prompt, secure-desktop switch, display-mode change, etc.).
    uint32_t monitor_index_ = 0;
    // Captured output's GDI device name (\\.\DISPLAY2).  EnumOutputs indices
    // shift when a display is added or removed, so after a topology change we
    // re-find "our" display by name instead of trusting the stale index.
    std::wstring output_name_;
    // The display the session actually asked for (startup --display / a client
    // SelectMonitor).  Differs from output_name_ only while we are on a
    // fallback display because the chosen one was unplugged; poll_display_
    // change() returns to it as soon as it comes back.
    std::wstring wanted_name_;
    // Desktop rectangle of the captured output as of the last duplication —
    // poll_display_change() compares against it to notice a mode change.
    RECT output_rect_ = {};
    // Sticky "resolution_ moved since the caller last looked" flag; see
    // take_geometry_change().
    bool geometry_dirty_ = false;
    // Desktop layout signatures.  layout_sig_ drives factory re-creation
    // (ensure_factory); poll_sig_ is poll_display_change's own copy so the two
    // callers never consume each other's change.
    std::string layout_sig_;
    std::string poll_sig_;
    // Throttle for re-duplication attempts while the desktop is mid-change.
    TimePoint last_dup_attempt_{};
    static constexpr int64_t DUP_RETRY_MS = 100;
    // Set by every (re)duplication: read the real format/size off the first
    // texture DXGI delivers, because ModeDesc can disagree with it.
    bool probe_next_frame_ = true;

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

} // namespace vivora

#endif // VIVORA_WINDOWS
