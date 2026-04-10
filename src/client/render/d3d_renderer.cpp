#ifdef DESKBEAM_WINDOWS

#include "client/render/d3d_renderer.h"
#include "common/utils/log.h"
#include <algorithm>
#include <climits>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

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

    if (!create_intermediate())     return false;
    if (!create_video_processor())  return false;
    if (!create_shader_pipeline())  return false;

    log::info("RENDER", "D3D11 renderer initialized: frame %ux%u -> window %ux%u (bicubic)",
              frame_width, frame_height, window_width, window_height);
    return true;
}

bool D3dRenderer::create_intermediate() {
    // Frame-size RGB texture: VP writes here, shader pass reads from here.
    // R10G10B10A2 for HDR (preserves PQ-encoded values), BGRA8 for SDR.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = frame_width_;
    desc.Height = frame_height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = is_hdr_ ? DXGI_FORMAT_R10G10B10A2_UNORM
                          : DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, intermediate_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        log::error("RENDER", "Intermediate texture create failed: 0x%08X", hr);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    hr = device_->CreateShaderResourceView(intermediate_.Get(), &srv_desc,
                                           intermediate_srv_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        log::error("RENDER", "Intermediate SRV create failed: 0x%08X", hr);
        return false;
    }
    return true;
}

bool D3dRenderer::create_video_processor() {
    HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(video_device_.GetAddressOf()));
    if (FAILED(hr)) {
        log::error("RENDER", "ID3D11VideoDevice not supported: 0x%08X", hr);
        return false;
    }

    context_->QueryInterface(IID_PPV_ARGS(video_context_.GetAddressOf()));

    // VP is used ONLY for NV12/P010 -> RGB conversion at native resolution.
    // No scaling, no letterbox — that's done by the bicubic shader pass.
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC vp_desc = {};
    vp_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    vp_desc.InputWidth = frame_width_;
    vp_desc.InputHeight = frame_height_;
    vp_desc.OutputWidth = frame_width_;
    vp_desc.OutputHeight = frame_height_;
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

    // VPO view for the intermediate texture — VP writes RGB pixels here.
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    hr = video_device_->CreateVideoProcessorOutputView(
        intermediate_.Get(), vp_enum_.Get(), &ovd,
        intermediate_vpov_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        log::error("RENDER", "CreateVideoProcessorOutputView(intermediate) failed: 0x%08X", hr);
        return false;
    }
    return true;
}

// Vertex shader: full-screen triangle from SV_VertexID, no input layout.
static const char* kVsSource = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

// Pixel shader: 16-tap Mitchell-Netravali bicubic (B=1/3, C=1/3).
//
// Catmull-Rom (B=0, C=0.5) is sharper but its negative lobes cause a
// visible "ringing" halo on high-contrast edges — which is exactly the
// worst case for text on a remote desktop, and turns sharpened text into
// a hard-to-read crunch. Mitchell-Netravali has much smaller negative
// lobes and is the classic "subjectively best" choice for photographic
// content with sharp edges. Clearly sharper than bilinear, no crunch.
//
// We use a plain 16-tap (4x4) instead of the 9-tap bilinear-optimized
// Catmull-Rom trick because Mitchell has different weight ratios and the
// offset optimization doesn't directly apply. 16 samples @ 1920x1080@60
// is ~0.1-0.2 ms on any modern GPU — not a latency concern.
static const char* kPsSource = R"(
Texture2D tex      : register(t0);
SamplerState samp  : register(s0);
cbuffer CB         : register(b0) { float4 src_size; };

static const float B = 1.0 / 3.0;
static const float C = 1.0 / 3.0;

float mitchell(float x) {
    x = abs(x);
    float x2 = x * x;
    float x3 = x2 * x;
    if (x < 1.0) {
        return ((12.0 - 9.0 * B - 6.0 * C) * x3
              + (-18.0 + 12.0 * B + 6.0 * C) * x2
              + (6.0 - 2.0 * B)) / 6.0;
    }
    if (x < 2.0) {
        return ((-B - 6.0 * C) * x3
              + (6.0 * B + 30.0 * C) * x2
              + (-12.0 * B - 48.0 * C) * x
              + (8.0 * B + 24.0 * C)) / 6.0;
    }
    return 0.0;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 src = src_size.xy;
    float2 inv_src = 1.0 / src;
    float2 px = uv * src - 0.5;
    float2 fp = floor(px);
    float2 f  = px - fp;

    float4 col = float4(0, 0, 0, 0);
    [unroll] for (int yy = -1; yy <= 2; ++yy) {
        [unroll] for (int xx = -1; xx <= 2; ++xx) {
            float2 tap_uv = (fp + float2(xx, yy) + 0.5) * inv_src;
            float w = mitchell(float(xx) - f.x) * mitchell(float(yy) - f.y);
            col += tex.SampleLevel(samp, tap_uv, 0) * w;
        }
    }
    return float4(col.rgb, 1.0);
}
)";

bool D3dRenderer::create_shader_pipeline() {
    auto compile = [](const char* src, const char* entry, const char* target,
                      ID3DBlob** out_blob) -> bool {
        ComPtr<ID3DBlob> err;
        HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
                                entry, target, 0, 0, out_blob, err.GetAddressOf());
        if (FAILED(hr)) {
            log::error("RENDER", "Shader compile (%s) failed: %s",
                       target,
                       err ? (const char*)err->GetBufferPointer() : "no error blob");
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> vs_blob, ps_blob;
    if (!compile(kVsSource, "main", "vs_4_0", vs_blob.GetAddressOf())) return false;
    if (!compile(kPsSource, "main", "ps_4_0", ps_blob.GetAddressOf())) return false;

    HRESULT hr = device_->CreateVertexShader(vs_blob->GetBufferPointer(),
                                             vs_blob->GetBufferSize(),
                                             nullptr, vs_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { log::error("RENDER", "CreateVertexShader: 0x%08X", hr); return false; }
    hr = device_->CreatePixelShader(ps_blob->GetBufferPointer(),
                                    ps_blob->GetBufferSize(),
                                    nullptr, ps_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { log::error("RENDER", "CreatePixelShader: 0x%08X", hr); return false; }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sd, sampler_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { log::error("RENDER", "CreateSamplerState: 0x%08X", hr); return false; }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = 16;            // float4
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&cbd, nullptr, cbuffer_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { log::error("RENDER", "CreateBuffer(cbuffer): 0x%08X", hr); return false; }

    log::info("RENDER", "Shader pipeline ready (Mitchell-Netravali bicubic)");
    return true;
}

bool D3dRenderer::render(ID3D11Texture2D* texture, uint32_t subresource) {
    if (!swapchain_ || !vp_ || !ps_) return false;

    // ---- Pass 1: VP NV12/P010 -> RGB intermediate (no scaling) ----
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

    RECT full = { 0, 0, (LONG)frame_width_, (LONG)frame_height_ };
    video_context_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &full);
    video_context_->VideoProcessorSetStreamDestRect  (vp_.Get(), 0, TRUE, &full);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    hr = video_context_->VideoProcessorBlt(vp_.Get(), intermediate_vpov_.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        log::error("RENDER", "VideoProcessorBlt failed: 0x%08X", hr);
        return false;
    }

    // ---- Pass 2: bicubic shader intermediate -> swapchain back buffer ----
    ComPtr<ID3D11Texture2D> back_buffer;
    hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf()));
    if (FAILED(hr)) return false;

    ComPtr<ID3D11RenderTargetView> rtv;
    hr = device_->CreateRenderTargetView(back_buffer.Get(), nullptr, rtv.GetAddressOf());
    if (FAILED(hr)) {
        log::error("RENDER", "CreateRenderTargetView(backbuffer) failed: 0x%08X", hr);
        return false;
    }

    // Aspect-fit (letterbox) the video rect inside the window.
    const double frame_aspect  = (double)frame_width_  / frame_height_;
    const double window_aspect = (double)window_width_ / window_height_;
    LONG dst_w, dst_h, dst_x, dst_y;
    if (frame_aspect > window_aspect) {
        dst_w = window_width_;
        dst_h = (LONG)(window_width_ / frame_aspect);
        dst_x = 0;
        dst_y = (window_height_ - dst_h) / 2;
    } else {
        dst_h = window_height_;
        dst_w = (LONG)(window_height_ * frame_aspect);
        dst_x = (window_width_ - dst_w) / 2;
        dst_y = 0;
    }

    // Clear the whole back buffer (letterbox bars stay black).
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context_->ClearRenderTargetView(rtv.Get(), clear);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = (FLOAT)dst_x;
    vp.TopLeftY = (FLOAT)dst_y;
    vp.Width    = (FLOAT)dst_w;
    vp.Height   = (FLOAT)dst_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);

    ID3D11RenderTargetView* rtvs[] = { rtv.Get() };
    context_->OMSetRenderTargets(1, rtvs, nullptr);

    // Update cbuffer (frame size — used by bicubic to compute texel positions).
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context_->Map(cbuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        float cb[4] = { (float)frame_width_, (float)frame_height_, 0.0f, 0.0f };
        std::memcpy(mapped.pData, cb, sizeof(cb));
        context_->Unmap(cbuffer_.Get(), 0);
    }

    context_->IASetInputLayout(nullptr);
    context_->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    context_->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer*             cbs[]  = { cbuffer_.Get() };
    ID3D11ShaderResourceView* srvs[] = { intermediate_srv_.Get() };
    ID3D11SamplerState*       sams[] = { sampler_.Get() };
    context_->PSSetConstantBuffers(0, 1, cbs);
    context_->PSSetShaderResources(0, 1, srvs);
    context_->PSSetSamplers(0, 1, sams);

    context_->Draw(3, 0);

    // Unbind so the next frame's VP write to intermediate isn't blocked.
    ID3D11ShaderResourceView* null_srv[] = { nullptr };
    context_->PSSetShaderResources(0, 1, null_srv);
    ID3D11RenderTargetView*   null_rtv[] = { nullptr };
    context_->OMSetRenderTargets(1, null_rtv, nullptr);

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

    // VP and intermediate are sized to the FRAME, not the window — no need
    // to recreate them on window resize. Only the swap chain back buffers
    // need to grow/shrink.
    HRESULT hr = swapchain_->ResizeBuffers(0, width, height,
                                            DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        log::error("RENDER", "ResizeBuffers failed: 0x%08X", hr);
        return false;
    }
    return true;
}

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
