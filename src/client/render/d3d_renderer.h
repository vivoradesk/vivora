#pragma once

#ifdef DESKBEAM_WINDOWS

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>

struct HWND__;
typedef HWND__* HWND;

namespace deskbeam {

using Microsoft::WRL::ComPtr;

// Renders NV12/P010 decoded textures to a window using D3D11 swapchain.
class D3dRenderer {
public:
    // Initialize with the decoder's D3D11 device and target window.
    bool init(ID3D11Device* device, HWND hwnd, uint32_t width, uint32_t height);

    // Render a decoded frame texture (NV12/P010 from MF decoder).
    // subresource = texture array index from DecodedFrame.
    bool render(ID3D11Texture2D* texture, uint32_t subresource);

    // Resize swapchain (call when window resizes).
    bool resize(uint32_t width, uint32_t height);

private:
    bool create_video_processor();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11VideoProcessorEnumerator> vp_enum_;
    ComPtr<ID3D11VideoProcessor> vp_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
