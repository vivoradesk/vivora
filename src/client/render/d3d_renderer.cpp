#ifdef DESKBEAM_WINDOWS

#include "client/render/d3d_renderer.h"
#include "common/utils/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#pragma comment(lib, "dxgi.lib")

namespace deskbeam {

bool D3dRenderer::init(ID3D11Device* device, HWND hwnd, uint32_t width, uint32_t height) {
    device_ = device;
    device_->GetImmediateContext(context_.GetAddressOf());
    width_ = width;
    height_ = height;

    // Create swapchain
    ComPtr<IDXGIDevice> dxgi_device;
    device_->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf()));

    ComPtr<IDXGIAdapter> adapter;
    dxgi_device->GetAdapter(adapter.GetAddressOf());

    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));

    DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
    sc_desc.Width = width;
    sc_desc.Height = height;
    sc_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
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

    if (!create_video_processor()) return false;

    log::info("RENDER", "D3D11 renderer initialized: %ux%u", width, height);
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
    vp_desc.InputWidth = width_;
    vp_desc.InputHeight = height_;
    vp_desc.OutputWidth = width_;
    vp_desc.OutputHeight = height_;
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
    if (width == width_ && height == height_) return true;
    width_ = width;
    height_ = height;

    // Release old views before resizing
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
