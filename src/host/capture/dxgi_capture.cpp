// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef VIVORA_WINDOWS

#include "host/capture/dxgi_capture.h"
#include "common/utils/log.h"
#include "common/utils/metrics.h"
#include <cstring>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace vivora {

static const char* TAG = "CAPTURE";

DxgiCapture::DxgiCapture() = default;

DxgiCapture::~DxgiCapture() {
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
    }
}

bool DxgiCapture::init(uint32_t monitor_index) {
    monitor_index_ = monitor_index;
    if (!init_d3d11()) return false;
    if (!init_output_duplication(monitor_index)) return false;

    log::info(TAG, "Initialized DXGI capture: %ux%u on monitor %u",
              resolution_.width, resolution_.height, monitor_index);
    return true;
}

bool DxgiCapture::init_d3d11() {
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                    // no software rasterizer
        flags,
        feature_levels,
        _countof(feature_levels),
        D3D11_SDK_VERSION,
        device_.GetAddressOf(),
        nullptr,
        context_.GetAddressOf()
    );

    if (FAILED(hr)) {
        log::error(TAG, "D3D11CreateDevice failed: 0x%08X", hr);
        return false;
    }

    return true;
}

bool DxgiCapture::init_output_duplication(uint32_t monitor_index) {
    // Get DXGI device -> adapter -> output
    ComPtr<IDXGIDevice> dxgi_device;
    HRESULT hr = device_.As(&dxgi_device);
    if (FAILED(hr)) {
        log::error(TAG, "Failed to get IDXGIDevice: 0x%08X", hr);
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) {
        log::error(TAG, "Failed to get adapter: 0x%08X", hr);
        return false;
    }

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(monitor_index, output.GetAddressOf());
    if (FAILED(hr)) {
        log::error(TAG, "Failed to enumerate output %u: 0x%08X", monitor_index, hr);
        return false;
    }

    // Get output description for resolution + desktop origin.  The origin
    // matters for input injection on multi-monitor hosts (VIV-50): SendInput
    // absolute coords span the virtual desktop, so a non-primary display's
    // offset must be added to the normalized client coordinates.
    DXGI_OUTPUT_DESC desc;
    output->GetDesc(&desc);
    resolution_.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    resolution_.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
    origin_x_ = desc.DesktopCoordinates.left;
    origin_y_ = desc.DesktopCoordinates.top;

    // Try IDXGIOutput5::DuplicateOutput1 for FP16 HDR capture
    hr = output.As(&output5_);
    if (SUCCEEDED(hr)) {
        DXGI_FORMAT formats[] = {
            DXGI_FORMAT_R16G16B16A16_FLOAT,  // HDR preferred
            DXGI_FORMAT_B8G8R8A8_UNORM,      // SDR fallback
        };
        hr = output5_->DuplicateOutput1(device_.Get(), 0,
                                         _countof(formats), formats,
                                         duplication_.GetAddressOf());
        if (SUCCEEDED(hr)) {
            DXGI_OUTDUPL_DESC dup_desc;
            duplication_->GetDesc(&dup_desc);
            capture_format_ = dup_desc.ModeDesc.Format;
            log::info(TAG, "DuplicateOutput1: format=%u (%s)", capture_format_,
                      capture_format_ == DXGI_FORMAT_R16G16B16A16_FLOAT ? "FP16 HDR" : "BGRA SDR");
        } else {
            log::warn(TAG, "DuplicateOutput1 failed: 0x%08X, falling back", hr);
        }
    } else {
        log::warn(TAG, "IDXGIOutput5 not available: 0x%08X", hr);
    }

    // Fallback to DuplicateOutput (always BGRA)
    if (!duplication_) {
        ComPtr<IDXGIOutput1> output1;
        hr = output.As(&output1);
        if (FAILED(hr)) {
            log::error(TAG, "Failed to get IDXGIOutput1: 0x%08X", hr);
            return false;
        }
        hr = output1->DuplicateOutput(device_.Get(), duplication_.GetAddressOf());
        if (FAILED(hr)) {
            log::error(TAG, "DuplicateOutput failed: 0x%08X. "
                       "Ensure running as desktop app (not UWP) and no other capture active.", hr);
            return false;
        }
        capture_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
        log::info(TAG, "DuplicateOutput fallback: BGRA SDR");
    }

    return true;
}

bool DxgiCapture::capture_frame(CapturedFrame& frame, uint32_t timeout_ms) {
    // Silently recreate the duplication if it was dropped on a previous tick
    // (exclusive fullscreen enter/exit, UAC, secure-desktop, mode switch).
    // init_output_duplication logs on success, so we stay quiet here on the
    // failure path — next tick will retry.
    if (!duplication_) {
        if (!init_output_duplication(monitor_index_)) return false;
    }

    // Release previous frame if held
    if (frame_acquired_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info;
    ComPtr<IDXGIResource> desktop_resource;

    HRESULT hr = duplication_->AcquireNextFrame(
        timeout_ms, &frame_info, desktop_resource.GetAddressOf());

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false; // no new frame
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        log::warn(TAG, "Access lost, will reinit on next tick");
        duplication_.Reset();
        return false;
    }

    if (FAILED(hr)) {
        // Any other failure (observed: DXGI_ERROR_INVALID_CALL 0x887A0001 after
        // language-switcher popup + game minimize) means the duplication handle
        // is permanently broken — drop it so the next tick re-runs init.
        log::error(TAG, "AcquireNextFrame failed: 0x%08X, will reinit on next tick", hr);
        duplication_.Reset();
        return false;
    }

    frame_acquired_ = true;

    // Get the texture — stays in GPU memory
    ComPtr<ID3D11Texture2D> texture;
    hr = desktop_resource.As(&texture);
    if (FAILED(hr)) {
        log::error(TAG, "Failed to get texture from resource: 0x%08X", hr);
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
        return false;
    }

    // ComPtr operator= AddRef's texture — safe to outlive the local
    // ComPtr here, but the DXGI frame itself is released on next
    // release_frame() call so consumers must copy or process synchronously.
    frame.texture = texture;
    frame.resolution = resolution_;
    frame.frame_index = ++frame_count_;
    frame.capture_time = Clock::now();

    // Desktop image changed if a present occurred or frames accumulated
    frame.content_changed = (frame_info.LastPresentTime.QuadPart != 0) ||
                            (frame_info.AccumulatedFrames > 0);

    // Get dirty rects
    frame.dirty_rects.clear();
    if (frame_info.TotalMetadataBufferSize > 0) {
        UINT buf_size = frame_info.TotalMetadataBufferSize;
        std::vector<BYTE> meta_buf(buf_size);
        UINT move_rects_size = 0;

        // Skip move rects, get dirty rects
        hr = duplication_->GetFrameMoveRects(buf_size, reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta_buf.data()), &move_rects_size);
        if (SUCCEEDED(hr)) {
            UINT dirty_rects_size = buf_size - move_rects_size;
            if (dirty_rects_size > 0) {
                std::vector<RECT> rects(dirty_rects_size / sizeof(RECT));
                hr = duplication_->GetFrameDirtyRects(
                    dirty_rects_size, rects.data(), &dirty_rects_size);
                if (SUCCEEDED(hr)) {
                    UINT count = dirty_rects_size / sizeof(RECT);
                    frame.dirty_rects.reserve(count);
                    for (UINT i = 0; i < count; ++i) {
                        frame.dirty_rects.push_back({
                            rects[i].left,
                            rects[i].top,
                            static_cast<uint32_t>(rects[i].right - rects[i].left),
                            static_cast<uint32_t>(rects[i].bottom - rects[i].top)
                        });
                    }
                }
            }
        }
    }

    // Cursor info: DXGI fills PointerPosition only on frames where the
    // cursor changed. Carry the last known state forward so every frame
    // has valid cursor data, otherwise downstream sees "invisible" flicker
    // on idle ticks and the client cursor flickers between the host shape
    // and BlankCursor.
    if (frame_info.LastMouseUpdateTime.QuadPart != 0) {
        sticky_cursor_x_       = frame_info.PointerPosition.Position.x;
        sticky_cursor_y_       = frame_info.PointerPosition.Position.y;
        sticky_cursor_visible_ = frame_info.PointerPosition.Visible != 0;
    }
    frame.cursor.x       = sticky_cursor_x_;
    frame.cursor.y       = sticky_cursor_y_;
    frame.cursor.visible = sticky_cursor_visible_;

    // Cursor shape: DXGI only fills the pointer-shape buffer when the
    // shape actually changed. Fetching it is cheap when it's empty.
    if (frame_info.PointerShapeBufferSize > 0) {
        update_cursor_shape(frame_info.PointerShapeBufferSize);
    }

    return true;
}

void DxgiCapture::update_cursor_shape(UINT buffer_size) {
    if (shape_scratch_.size() < buffer_size) shape_scratch_.resize(buffer_size);

    DXGI_OUTDUPL_POINTER_SHAPE_INFO info = {};
    UINT required = 0;
    HRESULT hr = duplication_->GetFramePointerShape(
        buffer_size, shape_scratch_.data(), &required, &info);
    if (FAILED(hr)) {
        log::warn(TAG, "GetFramePointerShape failed: 0x%08X", hr);
        return;
    }

    // DXGI reports Height doubled for monochrome (AND mask + XOR mask stacked).
    const bool is_mono = (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME);
    const uint32_t out_w = info.Width;
    const uint32_t out_h = is_mono ? (info.Height / 2) : info.Height;
    if (out_w == 0 || out_h == 0 || out_w > 256 || out_h > 256) {
        log::warn(TAG, "Implausible cursor shape %ux%u type=%u — ignored",
                  out_w, out_h, info.Type);
        return;
    }

    CursorShape s;
    s.width     = static_cast<uint16_t>(out_w);
    s.height    = static_cast<uint16_t>(out_h);
    s.hotspot_x = static_cast<uint16_t>(info.HotSpot.x);
    s.hotspot_y = static_cast<uint16_t>(info.HotSpot.y);
    s.bgra.assign(static_cast<size_t>(out_w) * out_h * 4u, 0);

    const uint8_t* src = shape_scratch_.data();
    uint8_t* dst = s.bgra.data();
    const UINT pitch = info.Pitch;

    switch (info.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR: {
        // BGRA already. Copy row by row since source pitch may differ from
        // our tight `width*4` layout.
        const uint32_t row_bytes = out_w * 4u;
        for (uint32_t y = 0; y < out_h; ++y) {
            std::memcpy(dst + y * row_bytes, src + y * pitch, row_bytes);
        }
        break;
    }
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
        // Per DXGI spec the alpha byte is *inverted* vs. a normal BGRA:
        //   a == 0x00 → the pixel replaces the desktop (opaque draw).
        //   a == 0xFF → the pixel XORs the desktop (inversion cursor).
        // We can't do XOR on a flat cursor bitmap, so we approximate the
        // XOR region as transparent — for the Windows hand cursor this
        // corresponds to the area *outside* the visible hand, giving a
        // clean cutout instead of a black box.
        for (uint32_t y = 0; y < out_h; ++y) {
            const uint8_t* s_row = src + y * pitch;
            uint8_t* d_row = dst + y * out_w * 4u;
            for (uint32_t x = 0; x < out_w; ++x) {
                uint8_t b = s_row[x*4+0], g = s_row[x*4+1];
                uint8_t r = s_row[x*4+2], a = s_row[x*4+3];
                d_row[x*4+0] = b;
                d_row[x*4+1] = g;
                d_row[x*4+2] = r;
                d_row[x*4+3] = (a == 0) ? 255 : 0;
            }
        }
        break;
    }
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
    default: {
        // 1bpp AND/XOR masks. For each pixel:
        //   AND=0, XOR=0 -> opaque black
        //   AND=0, XOR=1 -> opaque white
        //   AND=1, XOR=0 -> transparent
        //   AND=1, XOR=1 -> inversion (not supported) -> opaque white
        const uint8_t* and_mask = src;
        const uint8_t* xor_mask = src + pitch * out_h;
        for (uint32_t y = 0; y < out_h; ++y) {
            const uint8_t* and_row = and_mask + y * pitch;
            const uint8_t* xor_row = xor_mask + y * pitch;
            uint8_t* d_row = dst + y * out_w * 4u;
            for (uint32_t x = 0; x < out_w; ++x) {
                uint8_t bit    = 0x80u >> (x & 7);
                bool and_bit = (and_row[x >> 3] & bit) != 0;
                bool xor_bit = (xor_row[x >> 3] & bit) != 0;
                uint8_t color = xor_bit ? 255 : 0;
                uint8_t alpha = and_bit ? (xor_bit ? 255 : 0) : 255;
                d_row[x*4+0] = color;
                d_row[x*4+1] = color;
                d_row[x*4+2] = color;
                d_row[x*4+3] = alpha;
            }
        }
        break;
    }
    }

    s.id = ++current_shape_id_;
    pending_shape_ = std::move(s);
    new_shape_pending_ = true;
}

bool DxgiCapture::take_new_cursor_shape(CursorShape& out) {
    if (!new_shape_pending_) return false;
    out = std::move(pending_shape_);
    new_shape_pending_ = false;
    return true;
}

void DxgiCapture::release_frame(CapturedFrame& frame) {
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }
    frame.texture.Reset();
}

std::vector<MonitorInfo> DxgiCapture::enumerate_monitors() {
    std::vector<MonitorInfo> monitors;

    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device_.As(&dxgi_device))) return monitors;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf()))) return monitors;

    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; adapter->EnumOutputs(i, output.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_OUTPUT_DESC desc;
        output->GetDesc(&desc);

        MonitorInfo info;
        info.index = i;
        // Convert wide string to narrow
        char name_buf[128];
        wcstombs(name_buf, desc.DeviceName, sizeof(name_buf));
        info.name = name_buf;
        info.bounds.x = desc.DesktopCoordinates.left;
        info.bounds.y = desc.DesktopCoordinates.top;
        info.bounds.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        info.bounds.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        info.resolution.width = info.bounds.width;
        info.resolution.height = info.bounds.height;
        info.primary = (i == 0);

        monitors.push_back(std::move(info));
        output.Reset();
    }

    return monitors;
}

Resolution DxgiCapture::get_resolution() const {
    return resolution_;
}

bool DxgiCapture::switch_monitor(uint32_t monitor_index) {
    // Release any held frame and drop the current duplication before moving.
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }
    duplication_.Reset();
    output5_.Reset();
    monitor_index_ = monitor_index;
    if (!init_output_duplication(monitor_index)) {
        log::error(TAG, "switch_monitor: failed to duplicate output %u", monitor_index);
        return false;
    }
    log::info(TAG, "Switched capture to monitor %u: %ux%u",
              monitor_index, resolution_.width, resolution_.height);
    return true;
}

// Factory implementation
std::unique_ptr<IScreenCapture> IScreenCapture::create() {
    return std::make_unique<DxgiCapture>();
}

} // namespace vivora

#endif // VIVORA_WINDOWS
