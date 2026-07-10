#ifdef VIVORA_WINDOWS

#include "client/render/d3d_renderer.h"
#include "common/utils/log.h"
#include <algorithm>
#include <climits>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace vivora {

// True when the display the window lives on currently has HDR enabled.
// IDXGIOutput6::GetDesc1().ColorSpace reports G2084/P2020 when Windows
// "Use HDR" is on for that output, and G22/P709 when it's an SDR desktop.
// We pick the output whose HMONITOR matches the window; if none matches
// (e.g. the window's monitor hangs off a different adapter than the decode
// device) we fall back to the first output, then to SDR — the safe default
// since tone-mapping down never blows out the image.
static bool display_supports_hdr(IDXGIAdapter* adapter, HWND hwnd) {
    if (!adapter) return false;
    HMONITOR wnd_mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    bool fallback_hdr = false;
    bool have_fallback = false;
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0;
         adapter->EnumOutputs(i, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_OUTPUT_DESC od = {};
        if (FAILED(output->GetDesc(&od))) continue;
        ComPtr<IDXGIOutput6> o6;
        if (FAILED(output.As(&o6))) continue;
        DXGI_OUTPUT_DESC1 od1 = {};
        if (FAILED(o6->GetDesc1(&od1))) continue;
        const bool is_hdr =
            (od1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        if (od.Monitor == wnd_mon) return is_hdr;   // exact match wins
        if (!have_fallback) { fallback_hdr = is_hdr; have_fallback = true; }
    }
    return fallback_hdr;
}

bool D3dRenderer::init(ID3D11Device* device, HWND hwnd,
                       uint32_t frame_width, uint32_t frame_height,
                       uint32_t window_width, uint32_t window_height,
                       DXGI_FORMAT frame_format) {
    // Re-init support (VIV-50 monitor switch changes the frame geometry
    // mid-session): release everything from a previous init FIRST.  An HWND
    // can host only one swapchain — creating the new one while the old still
    // lives fails with E_ACCESSDENIED, which froze the picture after a
    // switch.  Flush the context after the releases so the old swapchain's
    // deferred destruction completes before the new CreateSwapChainForHwnd.
    if (swapchain_) {
        intermediate_srv_.Reset();
        intermediate_vpov_.Reset();
        intermediate_.Reset();
        vp_.Reset();
        vp_enum_.Reset();
        video_context_.Reset();
        video_device_.Reset();
        vs_.Reset(); ps_.Reset(); sampler_.Reset(); cbuffer_.Reset();
        swapchain_.Reset();
        if (context_) context_->Flush();
        has_frame_ = false;
    }
    device_ = device;
    device_->GetImmediateContext(context_.ReleaseAndGetAddressOf());
    frame_width_ = frame_width;
    frame_height_ = frame_height;
    crop_width_  = frame_width;   // default: no crop until host sends StreamInfo
    crop_height_ = frame_height;
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

    // Only keep a PQ passthrough pipeline when the content is HDR *and* the
    // display is in HDR mode.  HDR content on an SDR display takes the
    // tone-mapping path instead (SDR swapchain + VP maps BT.2020/PQ ->
    // BT.709), which avoids the washed-out / over-bright picture you get when
    // PQ-encoded values land on an SDR monitor with no tone-map.
    output_hdr_ = is_hdr_ && display_supports_hdr(adapter.Get(), hwnd);
    if (is_hdr_) {
        log::info("RENDER", "HDR content; display HDR=%s -> %s",
                  output_hdr_ ? "on" : "off",
                  output_hdr_ ? "PQ passthrough" : "tone-map to SDR");
    }

    DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
    sc_desc.Width = window_width;
    sc_desc.Height = window_height;
    // HDR display: 10-bit RGB swap chain with PQ color space.
    // SDR display (incl. HDR content tone-mapped down): standard 8-bit BGRA.
    sc_desc.Format = output_hdr_ ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
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
    // Only when we're presenting genuine PQ pixels to an HDR display.
    if (output_hdr_) {
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
    // R10G10B10A2 when presenting HDR (preserves PQ-encoded values); BGRA8
    // for SDR output, including HDR content the VP has tone-mapped down — the
    // intermediate format must match the VP output color space, not the
    // source, or the tone-mapped SDR pixels would be re-quantised as PQ.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = frame_width_;
    desc.Height = frame_height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = output_hdr_ ? DXGI_FORMAT_R10G10B10A2_UNORM
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
            // Source is BT.2020/PQ YCbCr regardless of the display.
            vc1->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0,
                DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020);
            if (output_hdr_) {
                // HDR display: passthrough to BT.2020/PQ RGB; OS composes
                // into the HDR desktop.
                vc1->VideoProcessorSetOutputColorSpace1(vp_.Get(),
                    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
                log::info("RENDER", "Color space: HDR10 passthrough (BT.2020/PQ)");
            } else {
                // SDR display: the VP tone-maps PQ -> BT.709 G2.2 so the
                // image isn't over-bright.  Hint the source luminance so the
                // tone-mapper rolls off highlights sensibly (best-effort).
                vc1->VideoProcessorSetOutputColorSpace1(vp_.Get(),
                    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
                ComPtr<ID3D11VideoContext2> vc2;
                if (SUCCEEDED(video_context_.As(&vc2))) {
                    DXGI_HDR_METADATA_HDR10 md = {};
                    // Rec.2020 primaries + D65 white, in 0.00002 units.
                    md.RedPrimary[0]   = 35400; md.RedPrimary[1]   = 14600;
                    md.GreenPrimary[0] = 8500;  md.GreenPrimary[1] = 39850;
                    md.BluePrimary[0]  = 6550;  md.BluePrimary[1]  = 2300;
                    md.WhitePoint[0]   = 15635; md.WhitePoint[1]   = 16450;
                    md.MaxMasteringLuminance = 1000u * 10000u;  // 1000 nits
                    md.MinMasteringLuminance = 50;              // 0.005 nits
                    md.MaxContentLightLevel      = 1000;
                    md.MaxFrameAverageLightLevel = 400;
                    vc2->VideoProcessorSetStreamHDRMetaData(
                        vp_.Get(), 0, DXGI_HDR_METADATA_TYPE_HDR10,
                        sizeof(md), &md);
                }
                log::info("RENDER", "Color space: HDR->SDR tone-map (BT.2020/PQ -> BT.709)");
            }
        } else {
            // SDR: studio/limited-range BT.709 in, full-range RGB out.
            // Hosts now uniformly emit limited-range SDR (VIV-84): AMF is set
            // to studio explicitly, NVENC/QSV/VAAPI/VTB emit it by default.
            // This input used to say FULL — hand-matched to AMF's old
            // full-range output but silently wrong for every other encoder.
            vc1->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0,
                DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
            vc1->VideoProcessorSetOutputColorSpace1(vp_.Get(),
                DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            log::info("RENDER", "Color space: SDR BT.709 studio range");
        }
    } else {
        // Fallback: legacy D3D11_VIDEO_PROCESSOR_COLOR_SPACE (SDR only)
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_cs = {};
        input_cs.Usage = 0;
        input_cs.RGB_Range = 0;
        input_cs.YCbCr_Matrix = 1;    // BT.709
        input_cs.YCbCr_xvYCC = 0;
        // Limited range for SDR too — hosts emit studio-range SDR (VIV-84).
        input_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
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
// cb.xy = crop pixel size (what we sample over)
// cb.zw = 1 / intermediate texture size (texel size for sampling)
cbuffer CB         : register(b0) { float4 cb; };

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
    float2 src = cb.xy;          // crop size in pixels
    float2 texel = cb.zw;        // 1 / intermediate size (handles padding)
    // Clamp sampling into the crop region — outside (padding rows/cols)
    // may contain stale pixels from earlier blits or decoder garbage.
    float2 min_uv = texel * 0.5;
    float2 max_uv = (src - 0.5) * texel;

    float2 px = uv * src - 0.5;
    float2 fp = floor(px);
    float2 f  = px - fp;

    float4 col = float4(0, 0, 0, 0);
    [unroll] for (int yy = -1; yy <= 2; ++yy) {
        [unroll] for (int xx = -1; xx <= 2; ++xx) {
            float2 tap_uv = (fp + float2(xx, yy) + 0.5) * texel;
            tap_uv = clamp(tap_uv, min_uv, max_uv);
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

    // Source + dest rects: crop region of the decoded texture.  VP writes
    // to the top-left of the padded intermediate; the shader pass trims
    // the right/bottom padding via UV scaling below.
    RECT rect = { 0, 0, (LONG)crop_width_, (LONG)crop_height_ };
    video_context_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &rect);
    video_context_->VideoProcessorSetStreamDestRect  (vp_.Get(), 0, TRUE, &rect);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    hr = video_context_->VideoProcessorBlt(vp_.Get(), intermediate_vpov_.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        log::error("RENDER", "VideoProcessorBlt failed: 0x%08X", hr);
        return false;
    }

    has_frame_ = true;

    // ---- Pass 2: bicubic shader intermediate -> swapchain back buffer ----
    return present_intermediate();
}

bool D3dRenderer::re_present() {
    if (!has_frame_) return false;
    return present_intermediate();
}

void D3dRenderer::set_keep_aspect(bool keep) {
    if (keep_aspect_ == keep) return;
    keep_aspect_ = keep;
    if (has_frame_) present_intermediate();
}

bool D3dRenderer::present_intermediate() {
    ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf()));
    if (FAILED(hr)) return false;

    ComPtr<ID3D11RenderTargetView> rtv;
    hr = device_->CreateRenderTargetView(back_buffer.Get(), nullptr, rtv.GetAddressOf());
    if (FAILED(hr)) {
        log::error("RENDER", "CreateRenderTargetView(backbuffer) failed: 0x%08X", hr);
        return false;
    }

    // Destination rect inside the window.  Aspect-fit (letterbox/pillarbox)
    // by default; stretch to fill when the user turns aspect off (VIV-74).
    // Crop dims are the real content (not the padded decoded size).
    LONG dst_w, dst_h, dst_x, dst_y;
    if (!keep_aspect_) {
        dst_w = (LONG)window_width_;
        dst_h = (LONG)window_height_;
        dst_x = 0;
        dst_y = 0;
    } else {
        const double frame_aspect  = (double)crop_width_  / crop_height_;
        const double window_aspect = (double)window_width_ / window_height_;
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

    // cbuffer: xy = crop pixel size (real content region we sample over);
    //           zw = 1 / intermediate texture size (texel stride — the
    //                intermediate may be larger than crop due to codec
    //                alignment padding; UV tap positions stay in the
    //                valid content region).
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context_->Map(cbuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        float cb[4] = {
            (float)crop_width_,
            (float)crop_height_,
            1.0f / (float)frame_width_,
            1.0f / (float)frame_height_,
        };
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

bool D3dRenderer::set_crop(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    if (width > frame_width_)  width  = frame_width_;
    if (height > frame_height_) height = frame_height_;
    if (width == crop_width_ && height == crop_height_) return true;

    crop_width_  = width;
    crop_height_ = height;
    log::info("RENDER", "Crop set to %ux%u (decoded texture %ux%u)",
              crop_width_, crop_height_, frame_width_, frame_height_);
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

} // namespace vivora

#endif // VIVORA_WINDOWS
