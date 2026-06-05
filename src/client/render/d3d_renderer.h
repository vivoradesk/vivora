#pragma once

#ifdef VIVORA_WINDOWS

#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>

struct HWND__;
typedef HWND__* HWND;

namespace vivora {

using Microsoft::WRL::ComPtr;

// Renders NV12/P010 decoded textures to a window using D3D11 swapchain.
class D3dRenderer {
public:
    // Initialize with decoder's D3D11 device, target window,
    // decoded frame size (VP input), window client size (swap chain + VP output),
    // and the decoded texture format (NV12 = SDR, P010 = HDR).
    bool init(ID3D11Device* device, HWND hwnd,
              uint32_t frame_width, uint32_t frame_height,
              uint32_t window_width, uint32_t window_height,
              DXGI_FORMAT frame_format);

    // Render a decoded frame texture (NV12/P010 from MF decoder).
    // subresource = texture array index from DecodedFrame.
    bool render(ID3D11Texture2D* texture, uint32_t subresource);

    // Re-present the last frame from intermediate texture (Pass 2 only).
    // Call after resize to avoid showing a blank/stale frame.
    bool re_present();

    // Resize swapchain (call when window resizes).
    bool resize(uint32_t width, uint32_t height);

    // Set the real (pre-encoder-padding) content dimensions.  The decoded
    // texture may be larger — QSV rounds NV12 to 16-pixel alignment — so
    // we crop by sourcing only the top-left rectangle during VP blt and
    // using these dims for the bicubic aspect-fit pass.  No-op if the
    // crop matches the full decoded size.
    bool set_crop(uint32_t width, uint32_t height);

private:
    bool create_video_processor();
    bool create_intermediate();
    bool create_shader_pipeline();
    bool present_intermediate();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11VideoProcessorEnumerator> vp_enum_;
    ComPtr<ID3D11VideoProcessor> vp_;

    // Pass 1: VP renders NV12/P010 -> RGB into this intermediate at native
    // frame resolution (no scaling). Pass 2: shader bicubic-scales the
    // intermediate to the swap chain back buffer.
    ComPtr<ID3D11Texture2D> intermediate_;
    ComPtr<ID3D11VideoProcessorOutputView> intermediate_vpov_;
    ComPtr<ID3D11ShaderResourceView> intermediate_srv_;

    // Bicubic upsample/downsample shader pipeline.
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> cbuffer_;

    uint32_t frame_width_ = 0;   // decoded texture dims (may include codec padding)
    uint32_t frame_height_ = 0;
    uint32_t crop_width_ = 0;    // real content dims (<= frame_*); fed to intermediate
    uint32_t crop_height_ = 0;
    uint32_t window_width_ = 0;
    uint32_t window_height_ = 0;
    DXGI_FORMAT frame_format_ = DXGI_FORMAT_NV12;
    bool is_hdr_ = false;      // decoded content is HDR (P010/P016 / BT.2020 PQ)
    // The client display actually has HDR enabled.  Only when this is true do
    // we keep a PQ passthrough pipeline; otherwise (HDR content on an SDR
    // display) the VideoProcessor tone-maps BT.2020/PQ -> BT.709/SDR so the
    // picture isn't blown out / washed out.
    bool output_hdr_ = false;
    bool has_frame_ = false;
};

} // namespace vivora

#endif // VIVORA_WINDOWS
