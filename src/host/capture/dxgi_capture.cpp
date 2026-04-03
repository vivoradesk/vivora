#ifdef DESKBEAM_WINDOWS

#include "host/capture/dxgi_capture.h"
#include "common/utils/log.h"
#include "common/utils/metrics.h"
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace deskbeam {

static const char* TAG = "CAPTURE";

DxgiCapture::DxgiCapture() = default;

DxgiCapture::~DxgiCapture() {
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
    }
}

bool DxgiCapture::init(uint32_t monitor_index) {
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

    // Get output description for resolution
    DXGI_OUTPUT_DESC desc;
    output->GetDesc(&desc);
    resolution_.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    resolution_.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

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
    ScopedTimer timer(TAG, "capture_frame");

    if (!duplication_) return false;

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
        log::warn(TAG, "Access lost, reinitializing...");
        duplication_.Reset();
        // Caller should re-init
        return false;
    }

    if (FAILED(hr)) {
        log::error(TAG, "AcquireNextFrame failed: 0x%08X", hr);
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

    frame.texture = texture.Get();
    frame.resolution = resolution_;
    frame.frame_index = ++frame_count_;
    frame.capture_time = Clock::now();

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

    // Cursor info
    if (frame_info.LastMouseUpdateTime.QuadPart != 0) {
        frame.cursor.x = frame_info.PointerPosition.Position.x;
        frame.cursor.y = frame_info.PointerPosition.Position.y;
        frame.cursor.visible = frame_info.PointerPosition.Visible;
    }

    return true;
}

void DxgiCapture::release_frame(CapturedFrame& frame) {
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }
    frame.texture = nullptr;
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

// Factory implementation
std::unique_ptr<IScreenCapture> IScreenCapture::create() {
    return std::make_unique<DxgiCapture>();
}

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
