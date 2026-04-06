#ifdef DESKBEAM_WINDOWS

#include "client/render/d3d_renderer.h"
#include "common/utils/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#pragma comment(lib, "dxgi.lib")

namespace deskbeam {

bool D3dRenderer::init(ID3D11Device* device, HWND hwnd,
                       uint32_t frame_width, uint32_t frame_height,
                       uint32_t window_width, uint32_t window_height,
                       DXGI_FORMAT frame_format) {
    device_ = device;
    device_->GetImmediateContext(context_.GetAddressOf());
    frame_width_ = frame_width;
    frame_height_ = frame_height;
    window_width_ = window_width;
    window_height_ = window_height;
    frame_format_ = frame_format;
    is_hdr_ = (frame_format == DXGI_FORMAT_P010 || frame_format == DXGI_FORMAT_P016);

    // Create swapchain
    ComPtr<IDXGIDevice> dxgi_device;
    device_->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf()));

    ComPtr<IDXGIAdapter> adapter;
    dxgi_device->GetAdapter(adapter.GetAddressOf());

    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));

    DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
    sc_desc.Width = window_width;
    sc_desc.Height = window_height;
    // HDR: 10-bit RGB swap chain with PQ color space — let OS tone-map to display.
    // SDR: standard 8-bit BGRA.
    sc_desc.Format = is_hdr_ ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = 2;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.Flags = 0;

    HRESULT hr = factory->CreateSwapChainForHwnd(
        device, hwnd, &sc_desc, nullptr, nullptr,
        swapchain_.GetAddressOf());
    if (FAILED(hr)) {
        log::error("RENDER", "CreateSwapChainForHwnd failed: 0x%08X", hr);
        return false;
    }

    // Tag swap chain color space so OS knows how to composite / tone-map.
    if (is_hdr_) {
        ComPtr<IDXGISwapChain3> sc3;
        if (SUCCEEDED(swapchain_.As(&sc3))) {
            HRESULT cs_hr = sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            if (FAILED(cs_hr)) {
                log::warn("RENDER", "SetColorSpace1(HDR10) failed: 0x%08X", cs_hr);
            } else {
                log::info("RENDER", "Swap chain color space: HDR10 (BT.2020/PQ)");
            }
        }
    }

    if (!create_video_processor()) return false;

    log::info("RENDER", "D3D11 renderer initialized: frame %ux%u -> window %ux%u",
              frame_width, frame_height, window_width, window_height);
    return true;
}

bool D3dRenderer::create_video_processor() {
    HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(video_device_.GetAddressOf()));
    if (FAILED(hr)) {
        log::error("RENDER", "ID3D11VideoDevice not supported: 0x%08X", hr);
        return false;
    }

    context_->QueryInterface(IID_PPV_ARGS(video_context_.GetAddressOf()));

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC vp_desc = {};
    vp_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    vp_desc.InputWidth = frame_width_;
    vp_desc.InputHeight = frame_height_;
    vp_desc.OutputWidth = window_width_;
    vp_desc.OutputHeight = window_height_;
    vp_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = video_device_->CreateVideoProcessorEnumerator(&vp_desc, vp_enum_.GetAddressOf());
    if (FAILED(hr)) {
        log::error("RENDER", "CreateVideoProcessorEnumerator failed: 0x%08X", hr);
        return false;
    }

    hr = video_device_->CreateVideoProcessor(vp_enum_.Get(), 0, vp_.GetAddressOf());
    if (FAILED(hr)) {
        log::error("RENDER", "CreateVideoProcessor failed: 0x%08X", hr);
        return false;
    }

    // Set color space using the modern DXGI_COLOR_SPACE_TYPE API (more reliable
    // across GPU vendors) with legacy fallback.
    // Color space depends on content:
    //   SDR (NV12): BT.709 full range YCbCr -> BT.709 full range RGB (G22)
    //   HDR (P010): BT.2020 ST.2084 (PQ) YCbCr -> tone-mapped G22 RGB for SDR output
    ComPtr<ID3D11VideoContext1> vc1;
    hr = video_context_.As(&vc1);
    if (SUCCEEDED(hr)) {
        if (is_hdr_) {
            // HDR passthrough: BT.2020/PQ YCbCr -> BT.2020/PQ RGB.
            // OS composes into HDR desktop (or tone-maps for SDR monitor).
            vc1->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0,
                DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020);
            vc1->VideoProcessorSetOutputColorSpace1(vp_.Get(),
                DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            log::info("RENDER", "Color space: HDR10 passthrough (BT.2020/PQ)");
        } else {
            // SDR: full range BT.709 straight through
            vc1->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0,
                DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709);
            vc1->VideoProcessorSetOutputColorSpace1(vp_.Get(),
                DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            log::info("RENDER", "Color space: SDR BT.709 full range");
        }
    } else {
        // Fallback: legacy D3D11_VIDEO_PROCESSOR_COLOR_SPACE (SDR only)
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_cs = {};
        input_cs.Usage = 0;
        input_cs.RGB_Range = 0;
        input_cs.YCbCr_Matrix = 1;    // BT.709
        input_cs.YCbCr_xvYCC = 0;
        input_cs.Nominal_Range = is_hdr_
            ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235
            : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        video_context_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &input_cs);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_cs = {};
        output_cs.Usage = 0;
        output_cs.RGB_Range = 0;
        output_cs.YCbCr_Matrix = 1;
        output_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        video_context_->VideoProcessorSetOutputColorSpace(vp_.Get(), &output_cs);
        log::info("RENDER", "Color space set via legacy API");
    }

    return true;
}

bool D3dRenderer::render(ID3D11Texture2D* texture, uint32_t subresource) {
    if (!swapchain_ || !vp_) return false;

    // Create input view from decoded texture
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc = {};
    input_desc.FourCC = 0;
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_desc.Texture2D.ArraySlice = subresource;

    ComPtr<ID3D11VideoProcessorInputView> input_view;
    HRESULT hr = video_device_->CreateVideoProcessorInputView(
        texture, vp_enum_.Get(), &input_desc, input_view.GetAddressOf());
    if (FAILED(hr)) {
        log::error("RENDER", "CreateVideoProcessorInputView failed: 0x%08X", hr);
        return false;
    }

    // Create output view from swapchain back buffer
    ComPtr<ID3D11Texture2D> back_buffer;
    hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf()));
    if (FAILED(hr)) return false;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc = {};
    output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;

    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    hr = video_device_->CreateVideoProcessorOutputView(
        back_buffer.Get(), vp_enum_.Get(), &output_desc, output_view.GetAddressOf());
    if (FAILED(hr)) {
        log::error("RENDER", "CreateVideoProcessorOutputView failed: 0x%08X", hr);
        return false;
    }

    // Configure scaling rects: full frame -> aspect-fit in window
    RECT src_rect = { 0, 0, (LONG)frame_width_, (LONG)frame_height_ };
    video_context_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src_rect);

    // Aspect-fit frame into window (letterbox if needed)
    double frame_aspect = (double)frame_width_ / frame_height_;
    double window_aspect = (double)window_width_ / window_height_;
    LONG dst_w, dst_h, dst_x, dst_y;
    if (frame_aspect > window_aspect) {
        // Frame wider — fit width, letterbox top/bottom
        dst_w = window_width_;
        dst_h = (LONG)(window_width_ / frame_aspect);
        dst_x = 0;
        dst_y = (window_height_ - dst_h) / 2;
    } else {
        // Frame taller — fit height, letterbox left/right
        dst_h = window_height_;
        dst_w = (LONG)(window_height_ * frame_aspect);
        dst_x = (window_width_ - dst_w) / 2;
        dst_y = 0;
    }
    RECT dst_rect = { dst_x, dst_y, dst_x + dst_w, dst_y + dst_h };
    video_context_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst_rect);

    // Clear background (letterbox bars) to black
    D3D11_VIDEO_COLOR bg_color = {};
    bg_color.RGBA.A = 1.0f;
    video_context_->VideoProcessorSetOutputBackgroundColor(vp_.Get(), FALSE, &bg_color);

    // Blit NV12/P010 -> BGRA via Video Processor
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();

    hr = video_context_->VideoProcessorBlt(vp_.Get(), output_view.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        log::error("RENDER", "VideoProcessorBlt failed: 0x%08X", hr);
        return false;
    }

    // Present
    hr = swapchain_->Present(0, 0); // vsync off for lowest latency
    if (FAILED(hr)) {
        log::error("RENDER", "Present failed: 0x%08X", hr);
        return false;
    }

    return true;
}

bool D3dRenderer::resize(uint32_t width, uint32_t height) {
    if (width == window_width_ && height == window_height_) return true;
    if (width == 0 || height == 0) return true;
    window_width_ = width;
    window_height_ = height;

    // Release old VP before resizing swap chain
    vp_.Reset();
    vp_enum_.Reset();

    HRESULT hr = swapchain_->ResizeBuffers(0, width, height,
                                            DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        log::error("RENDER", "ResizeBuffers failed: 0x%08X", hr);
        return false;
    }

    return create_video_processor();
}

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
