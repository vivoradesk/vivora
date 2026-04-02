#pragma once

#ifdef DESKBEAM_WINDOWS

#include "host/capture/screen_capture.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace deskbeam {

using Microsoft::WRL::ComPtr;

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

private:
    bool init_d3d11();
    bool init_output_duplication(uint32_t monitor_index);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<IDXGIOutput1> output_;

    Resolution resolution_;
    uint64_t frame_count_ = 0;
    bool frame_acquired_ = false;
};

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
