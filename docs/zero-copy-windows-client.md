# Zero-Copy Decode → Render on Windows

How client keeps a decoded video frame entirely in GPU memory
from `IMFTransform::ProcessOutput` all the way to `IDXGISwapChain::Present`,
with no CPU pixel reads, no `Map`/`Unmap` round trips, no `memcpy` of
the framebuffer. Lift-and-shift notes for porting to another project.

Target platform: Windows 10+, D3D11.1, Media Foundation, optional HDR via
DXGI 1.4 color spaces.

## Architecture at a glance

```
H.264/HEVC NAL bytes
       │
       ▼
┌──────────────────────────────┐
│ IMFTransform decoder MFT     │  D3D11VA via DXGI device manager
│ (Media Foundation HEVC/H264) │  output sample = IMFSample
└──────────────┬───────────────┘
               │ IMFDXGIBuffer → ID3D11Texture2D* + subresource
               ▼
┌──────────────────────────────┐
│ D3dRenderer Pass 1           │  Pure GPU YUV→RGB +
│ ID3D11VideoProcessor         │  color-space conversion (BT.709/HDR10)
│ NV12/P010 → RGB intermediate │
└──────────────┬───────────────┘
               │ ID3D11ShaderResourceView on intermediate
               ▼
┌──────────────────────────────┐
│ D3dRenderer Pass 2           │  Full-screen triangle, no vertex buffer
│ HLSL bicubic shader          │  16-tap Mitchell-Netravali resample
│ intermediate → back buffer   │  Aspect-fit / letterbox viewport
└──────────────┬───────────────┘
               │ swap chain Present(0, 0)
               ▼
            HWND
```

A single `ID3D11Device` is shared end-to-end:

```
   ┌─────────────────────────┐
   │ ID3D11Device (created   │
   │  with VIDEO_SUPPORT +   │
   │  MULTITHREADED)         │
   └────┬─────────────┬──────┘
        │             │
        ▼             ▼
   MFT decoder    D3dRenderer
   (via DXGI       (swap chain,
    DeviceManager) VideoProcessor,
                    shaders)
```

If the decoder or the renderer creates the device, the other side reuses
it. **Never two devices.** Texture sharing across D3D11 devices requires
keyed mutex / shared handles and kills latency.

## Required dependencies

Link libs (Windows SDK):

```cpp
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")   // only if compiling HLSL at runtime
```

Headers:

```cpp
#include <d3d11_4.h>       // or d3d11.h if you don't need 11.4 features
#include <dxgi1_4.h>       // IDXGISwapChain3::SetColorSpace1 for HDR
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wrl/client.h>    // Microsoft::WRL::ComPtr
```

## Stage 1 — D3D11 device setup

```cpp
bool create_device(ComPtr<ID3D11Device>& device,
                   ComPtr<ID3D11DeviceContext>& context)
{
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    // VIDEO_SUPPORT is mandatory for D3D11VA decoders to bind.
    // Without it the MFT silently falls back to software decode
    // (or refuses to ProcessMessage SET_D3D_MANAGER).
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    HRESULT hr = D3D11CreateDevice(
        nullptr,                  // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels, _countof(levels),
        D3D11_SDK_VERSION,
        device.GetAddressOf(),
        nullptr,
        context.GetAddressOf());
    if (FAILED(hr)) return false;

    // Media Foundation calls the device from its worker threads.  Without
    // multi-thread protection you get random AccessViolation crashes
    // inside ProcessOutput on hybrid load.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device.As(&mt))) {
        mt->SetMultithreadProtected(TRUE);
    }
    return true;
}
```

Two non-obvious gotchas:

* `D3D11_CREATE_DEVICE_VIDEO_SUPPORT` — needed for MF D3D11VA.
* `ID3D10Multithread::SetMultithreadProtected(TRUE)` — needed because MF
  schedules work on its own threads; without protection the immediate
  context is corrupted by concurrent calls.

## Stage 2 — Media Foundation decoder

### 2a. Init MF and create the DXGI device manager

```cpp
// COM may already be initialized by the host UI framework (Qt is STA).
// Accept either mode — fail only on hard errors.
HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
if (FAILED(hr)) {
    hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        return false;
    }
}
hr = MFStartup(MF_VERSION);  // remember to MFShutdown() in dtor
if (FAILED(hr)) return false;

// DXGI device manager is the bridge between MF and D3D11.
UINT reset_token = 0;
IMFDXGIDeviceManager* device_manager = nullptr;
hr = MFCreateDXGIDeviceManager(&reset_token, &device_manager);
if (FAILED(hr)) return false;

hr = device_manager->ResetDevice(device.Get(), reset_token);
if (FAILED(hr)) return false;
```

### 2b. Pick the decoder MFT

```cpp
const GUID subtype = is_hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;

MFT_REGISTER_TYPE_INFO input_info = { MFMediaType_Video, subtype };
IMFActivate** activates = nullptr;
UINT32 count = 0;
hr = MFTEnumEx(
    MFT_CATEGORY_VIDEO_DECODER,
    MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
    &input_info, nullptr,
    &activates, &count);
if (FAILED(hr) || count == 0) {
    // No decoder.  On older Windows the user may need to install
    // "HEVC Video Extensions" from the Store.
    return false;
}

IMFTransform* transform = nullptr;
hr = activates[0]->ActivateObject(IID_PPV_ARGS(&transform));
for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
CoTaskMemFree(activates);
if (FAILED(hr)) return false;
```

`MFT_ENUM_FLAG_SYNCMFT` only picks synchronous MFTs.  Async MFTs require
event-driven `ProcessInput`/`ProcessOutput` which is harder to get right
in a single-threaded render loop.  HW decoders on Win10+ all expose a
sync interface.

If you do hit an async MFT you can unlock it explicitly:

```cpp
IMFAttributes* attrs = nullptr;
transform->GetAttributes(&attrs);
UINT32 is_async = FALSE;
attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
if (is_async) {
    attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
}
attrs->Release();
```

### 2c. Wire the D3D11 device into the decoder

This is the one line that flips the decoder from software to D3D11VA:

```cpp
hr = transform->ProcessMessage(
    MFT_MESSAGE_SET_D3D_MANAGER,
    reinterpret_cast<ULONG_PTR>(device_manager));
// Failure here = software fallback; not always fatal but kills perf.
```

Plus low-latency mode (skip B-frame reordering, no display-time queue):

```cpp
IMFAttributes* attrs = nullptr;
if (SUCCEEDED(transform->GetAttributes(&attrs)) && attrs) {
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    attrs->Release();
}
```

### 2d. Configure input/output media types

```cpp
// Input — codec type + nominal resolution (decoder updates from bitstream).
IMFMediaType* input_type = nullptr;
MFCreateMediaType(&input_type);
input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
input_type->SetGUID(MF_MT_SUBTYPE, subtype);
MFSetAttributeSize(input_type, MF_MT_FRAME_SIZE, 3840, 2160);
MFSetAttributeRatio(input_type, MF_MT_FRAME_RATE, 60, 1);
MFSetAttributeRatio(input_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
hr = transform->SetInputType(0, input_type, 0);
input_type->Release();

// Output — enumerate and pick NV12 (8-bit) or P010 (10-bit HDR).
for (DWORD i = 0; ; ++i) {
    IMFMediaType* out_type = nullptr;
    HRESULT en = transform->GetOutputAvailableType(0, i, &out_type);
    if (FAILED(en)) break;
    GUID st = {};
    out_type->GetGUID(MF_MT_SUBTYPE, &st);
    if (st == MFVideoFormat_NV12 || st == MFVideoFormat_P010 || i == 0) {
        hr = transform->SetOutputType(0, out_type, 0);
        out_type->Release();
        if (SUCCEEDED(hr)) break;
    } else {
        out_type->Release();
    }
}
```

The output enumeration only returns the formats the decoder can give to
the device manager — for D3D11VA that's NV12 (8-bit) and P010 (10-bit
HDR).  CPU-mappable formats (RGB32, etc.) won't appear at all.

## Stage 3 — Feeding bytes

```cpp
bool decode(const uint8_t* data, size_t len, uint64_t pts_100ns)
{
    // ProcessInput consumes a full IMFSample.  Wrap the NAL bytes in
    // a single buffer; the decoder copies them into its own internal
    // queue so we can free our buffer immediately after the call.
    IMFMediaBuffer* buf = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(len), &buf);
    if (FAILED(hr)) return false;

    BYTE* p = nullptr;
    DWORD max = 0, cur = 0;
    buf->Lock(&p, &max, &cur);
    memcpy(p, data, len);
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(len));

    IMFSample* sample = nullptr;
    MFCreateSample(&sample);
    sample->AddBuffer(buf);
    sample->SetSampleTime(static_cast<LONGLONG>(pts_100ns));
    sample->SetSampleDuration(0);

    hr = transform->ProcessInput(0, sample, 0);

    sample->Release();
    buf->Release();
    return SUCCEEDED(hr);
}
```

`ProcessInput` may return `MF_E_NOTACCEPTING` — you need to drain
`ProcessOutput` first.  In a streaming loop call drain after each input;
in a burst loop drain in a tight while.

## Stage 4 — Pulling decoded GPU textures out

**This is the zero-copy money shot.** Decoder gives you an `IMFSample`;
inside is `IMFDXGIBuffer` which holds the actual `ID3D11Texture2D*` plus
a subresource index (decoders use a texture array as their output pool).

```cpp
struct DecodedFrame {
    ComPtr<ID3D11Texture2D> texture;  // GPU-side; ComPtr keeps alive
    uint32_t subresource = 0;         // slice within the decoder pool
    uint32_t width  = 0;
    uint32_t height = 0;
    uint64_t pts    = 0;              // microseconds
};

void drain_output(std::queue<DecodedFrame>& out)
{
    while (true) {
        MFT_OUTPUT_STREAM_INFO info = {};
        transform->GetOutputStreamInfo(0, &info);

        // Decoder allocates its own output sample (D3D11VA pool).
        // We must NOT pre-create the sample for this path.
        MFT_OUTPUT_DATA_BUFFER output = {};
        output.dwStreamID = 0;
        output.pSample    = nullptr;   // decoder fills

        DWORD status = 0;
        HRESULT hr = transform->ProcessOutput(0, 1, &output, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // Resolution changed mid-stream.  Re-run configure_output()
            // and continue — don't drop the current output sample.
            reconfigure_output();
            if (output.pSample) output.pSample->Release();
            if (output.pEvents) output.pEvents->Release();
            continue;
        }
        if (FAILED(hr)) {
            if (output.pSample) output.pSample->Release();
            if (output.pEvents) output.pEvents->Release();
            break;
        }
        if (!output.pSample) {
            if (output.pEvents) output.pEvents->Release();
            continue;
        }

        // Extract the D3D11 texture from the sample.  This is where
        // zero-copy actually happens — we keep the same GPU pixels
        // that came out of the decoder kernel.
        IMFMediaBuffer* media_buf = nullptr;
        hr = output.pSample->GetBufferByIndex(0, &media_buf);
        if (SUCCEEDED(hr)) {
            IMFDXGIBuffer* dxgi_buf = nullptr;
            hr = media_buf->QueryInterface(IID_PPV_ARGS(&dxgi_buf));
            if (SUCCEEDED(hr)) {
                ID3D11Texture2D* tex = nullptr;
                UINT subres = 0;
                dxgi_buf->GetResource(IID_PPV_ARGS(&tex));    // AddRefs
                dxgi_buf->GetSubresourceIndex(&subres);

                if (tex) {
                    D3D11_TEXTURE2D_DESC desc;
                    tex->GetDesc(&desc);
                    LONGLONG sample_time = 0;
                    output.pSample->GetSampleTime(&sample_time);

                    DecodedFrame f;
                    // Attach() takes the AddRef'd ref WITHOUT a second
                    // AddRef — exactly what we want.  Using `f.texture = tex`
                    // here would double-ref and leak one count per frame.
                    f.texture.Attach(tex);
                    f.subresource = subres;
                    f.width  = desc.Width;
                    f.height = desc.Height;
                    f.pts    = static_cast<uint64_t>(sample_time / 10);
                    out.push(std::move(f));
                }
                dxgi_buf->Release();
            }
            media_buf->Release();
        }
        output.pSample->Release();
        if (output.pEvents) output.pEvents->Release();
    }
}
```

Three critical details:

1. **Don't pre-allocate the sample.**  D3D11VA decoders allocate their
   own samples backed by a fixed texture-array pool; if you provide
   your own buffer you force a copy back to CPU.

2. **`GetResource` AddRefs, `Attach` consumes the ref.**  Using
   `ComPtr operator=` here would AddRef a second time and leak.

3. **`desc.Width`/`desc.Height` includes codec alignment padding.**
   On QSV-derived pools NV12 is rounded to 16-pixel boundaries.  Carry
   the *real* (pre-pad) content size separately if you need it (host
   advertises it via a side channel; we crop in the renderer below).

### 4a. Flush before feeding a new IDR

After packet loss, dump the decoder's reference pictures before feeding
the next keyframe — otherwise macOS VideoToolbox and some Windows MFTs
keep stale POC references and produce "Could not find ref with POC X"
errors on every subsequent P-frame:

```cpp
void flush() {
    if (transform) {
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    while (!output_frames.empty()) output_frames.pop();
}
```

## Stage 5 — D3D11 swap chain on a native window

```cpp
bool init_swapchain(ID3D11Device* device, HWND hwnd,
                    uint32_t client_w, uint32_t client_h,
                    bool is_hdr,
                    ComPtr<IDXGISwapChain1>& swapchain)
{
    ComPtr<IDXGIDevice> dxgi_device;
    device->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf()));

    ComPtr<IDXGIAdapter> adapter;
    dxgi_device->GetAdapter(adapter.GetAddressOf());

    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));

    DXGI_SWAP_CHAIN_DESC1 sc = {};
    sc.Width  = client_w;
    sc.Height = client_h;
    sc.Format = is_hdr ? DXGI_FORMAT_R10G10B10A2_UNORM
                       : DXGI_FORMAT_B8G8R8A8_UNORM;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount      = 2;
    sc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;   // lowest latency
    sc.Flags            = 0;

    HRESULT hr = factory->CreateSwapChainForHwnd(
        device, hwnd, &sc, nullptr, nullptr,
        swapchain.GetAddressOf());
    if (FAILED(hr)) return false;

    // HDR: signal the compositor.  Without this the OS clamps to SDR
    // and the 10-bit PQ values are mis-tonemapped.
    if (is_hdr) {
        ComPtr<IDXGISwapChain3> sc3;
        if (SUCCEEDED(swapchain.As(&sc3))) {
            sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        }
    }
    return true;
}
```

`DXGI_SWAP_EFFECT_FLIP_DISCARD` + `Present(0, 0)` (no vsync) gives the
minimum input-to-photon latency.  The two cosmetic tradeoffs are screen
tearing (vsync off) and required `BufferCount >= 2`.

For Qt host the HWND comes from `widget->winId()` after setting:

```cpp
setAttribute(Qt::WA_PaintOnScreen, true);     // Qt doesn't paint over us
setAttribute(Qt::WA_NativeWindow, true);      // ensure real HWND
setAttribute(Qt::WA_NoSystemBackground, true);
// + override paintEngine() to return nullptr to silence Qt warnings.
```

## Stage 6 — Pass 1: ID3D11VideoProcessor YUV→RGB

Why use `ID3D11VideoProcessor` instead of a custom shader:

* Dedicated video hardware on the GPU (separate from shader cores) —
  free vs. burning a draw call.
* Built-in color space conversion via `*ColorSpace1` API including the
  HDR10 BT.2020/PQ matrix — non-trivial to get right in HLSL.
* Decoder output texture might be in a format the shader can't sample
  directly (P010 is a 2-plane texture); the VP knows the format
  intimately.

### 6a. Create the VP and an intermediate render target

```cpp
ComPtr<ID3D11VideoDevice>             video_device;
ComPtr<ID3D11VideoContext>            video_context;
ComPtr<ID3D11VideoProcessorEnumerator> vp_enum;
ComPtr<ID3D11VideoProcessor>          vp;
ComPtr<ID3D11Texture2D>               intermediate;
ComPtr<ID3D11VideoProcessorOutputView> intermediate_vpov;
ComPtr<ID3D11ShaderResourceView>      intermediate_srv;

device->QueryInterface(IID_PPV_ARGS(video_device.GetAddressOf()));
context->QueryInterface(IID_PPV_ARGS(video_context.GetAddressOf()));

D3D11_VIDEO_PROCESSOR_CONTENT_DESC vpd = {};
vpd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
vpd.InputWidth   = frame_w;   // decoded texture dims (may include codec pad)
vpd.InputHeight  = frame_h;
vpd.OutputWidth  = frame_w;   // intermediate is frame-sized; we scale in pass 2
vpd.OutputHeight = frame_h;
vpd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
video_device->CreateVideoProcessorEnumerator(&vpd, vp_enum.GetAddressOf());
video_device->CreateVideoProcessor(vp_enum.Get(), 0, vp.GetAddressOf());

// Intermediate texture — RGB target for VP, sampler source for shader.
D3D11_TEXTURE2D_DESC td = {};
td.Width  = frame_w;
td.Height = frame_h;
td.MipLevels = 1;
td.ArraySize = 1;
td.Format    = is_hdr ? DXGI_FORMAT_R10G10B10A2_UNORM
                      : DXGI_FORMAT_B8G8R8A8_UNORM;
td.SampleDesc.Count = 1;
td.Usage     = D3D11_USAGE_DEFAULT;
td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
device->CreateTexture2D(&td, nullptr, intermediate.GetAddressOf());

D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
srvd.Format = td.Format;
srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
srvd.Texture2D.MipLevels = 1;
device->CreateShaderResourceView(intermediate.Get(), &srvd,
                                  intermediate_srv.GetAddressOf());

D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
video_device->CreateVideoProcessorOutputView(
    intermediate.Get(), vp_enum.Get(), &ovd,
    intermediate_vpov.GetAddressOf());
```

### 6b. Color space — the hardest knob to get right

```cpp
// Prefer VideoContext1 — adds the HDR10 / BT.2020 enums.
ComPtr<ID3D11VideoContext1> vc1;
video_context.As(&vc1);

if (vc1) {
    if (is_hdr) {
        // Input: 10-bit YCbCr, BT.2020 primaries, PQ transfer,
        //        studio (16-235) range, chroma siting "left".
        vc1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0,
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020);
        // Output: 10-bit RGB, BT.2020, PQ, full range.  This must
        // match the swap chain's SetColorSpace1 call above.
        vc1->VideoProcessorSetOutputColorSpace1(vp.Get(),
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    } else {
        // SDR: full-range BT.709 (most desktop content).  Use STUDIO
        // variant if your decoder gives you 16-235 range.
        vc1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0,
            DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709);
        vc1->VideoProcessorSetOutputColorSpace1(vp.Get(),
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }
} else {
    // Legacy fallback — only SDR.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs = {};
    in_cs.YCbCr_Matrix  = 1;  // BT.709
    in_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    video_context->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &in_cs);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs = {};
    out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    video_context->VideoProcessorSetOutputColorSpace(vp.Get(), &out_cs);
}
```

### 6c. Per-frame VP draw

```cpp
bool vp_blit(ID3D11Texture2D* decoded, uint32_t subres,
             uint32_t crop_w, uint32_t crop_h)
{
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.ArraySlice = subres;

    ComPtr<ID3D11VideoProcessorInputView> input_view;
    HRESULT hr = video_device->CreateVideoProcessorInputView(
        decoded, vp_enum.Get(), &ivd, input_view.GetAddressOf());
    if (FAILED(hr)) return false;

    // Crop the codec padding via source rect + dest rect.
    RECT rect = { 0, 0, (LONG)crop_w, (LONG)crop_h };
    video_context->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &rect);
    video_context->VideoProcessorSetStreamDestRect  (vp.Get(), 0, TRUE, &rect);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();

    return SUCCEEDED(video_context->VideoProcessorBlt(
        vp.Get(), intermediate_vpov.Get(), 0, 1, &stream));
}
```

The VP runs on the dedicated video hardware (fixed-function YUV→RGB,
chroma upsample, color matrix). On a discrete GPU this takes ~0.2 ms
for 4K NV12.

## Stage 7 — Pass 2: HLSL bicubic shader scaling

Why a second pass instead of letting the VP scale:

* VP scaling on consumer GPUs is bilinear with poor edge handling —
  visible aliasing on remote-desktop text.
* HLSL bicubic costs ~0.1-0.2 ms on any modern GPU.
* Lets you do aspect-fit / letterboxing with sub-pixel placement
  trivially via the viewport.

### 7a. Shaders

Full-screen triangle from `SV_VertexID` — no vertex buffer at all:

```hlsl
// Vertex shader
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
```

```hlsl
// Pixel shader — 16-tap Mitchell-Netravali bicubic (B=1/3, C=1/3).
//
// Catmull-Rom (B=0, C=0.5) is sharper but has visible ringing halos on
// high-contrast edges — bad for remote-desktop text.  Mitchell has
// smaller negative lobes and is the classic "subjectively best" choice
// for photographic content with sharp edges.
Texture2D    tex  : register(t0);
SamplerState samp : register(s0);

// cb.xy = crop pixel size (real content region — may be smaller than texture)
// cb.zw = 1 / texture size (texel stride; texture may have codec padding)
cbuffer CB : register(b0) { float4 cb; };

float mitchell(float x) {
    x = abs(x);
    const float B = 1.0/3.0, C = 1.0/3.0;
    if (x < 1.0)
        return ((12 - 9*B - 6*C)*x*x*x + (-18 + 12*B + 6*C)*x*x + (6 - 2*B)) / 6;
    else if (x < 2.0)
        return ((-B - 6*C)*x*x*x + (6*B + 30*C)*x*x + (-12*B - 48*C)*x + (8*B + 24*C)) / 6;
    return 0;
}

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float4 main(VSOut i) : SV_Target {
    float2 src_px = i.uv * cb.xy;          // pixel coords in the crop region
    float2 base   = floor(src_px - 0.5) + 0.5;
    float2 f      = src_px - base;

    float4 sum = 0;
    float  wsum = 0;
    [unroll] for (int dy = -1; dy <= 2; ++dy) {
        [unroll] for (int dx = -1; dx <= 2; ++dx) {
            float2 sp = base + float2(dx, dy);
            float w = mitchell(dx - f.x) * mitchell(dy - f.y);
            // Sample texel center; cb.zw = 1/texture_size keeps UVs valid
            // even when the decoded texture has trailing padding rows.
            float4 s = tex.SampleLevel(samp, sp * cb.zw, 0);
            sum  += s * w;
            wsum += w;
        }
    }
    return sum / wsum;
}
```

Compile once at startup with `D3DCompile`, save the bytecode for next
launch if you want faster startup.

### 7b. Per-frame draw

```cpp
bool present_pass2(uint32_t crop_w,  uint32_t crop_h,
                   uint32_t frame_w, uint32_t frame_h,
                   uint32_t win_w,   uint32_t win_h)
{
    ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT hr = swapchain->GetBuffer(0, IID_PPV_ARGS(back_buffer.GetAddressOf()));
    if (FAILED(hr)) return false;

    ComPtr<ID3D11RenderTargetView> rtv;
    device->CreateRenderTargetView(back_buffer.Get(), nullptr,
                                    rtv.GetAddressOf());

    // Aspect-fit (letterbox).
    const double frame_aspect  = (double)crop_w / crop_h;
    const double window_aspect = (double)win_w  / win_h;
    LONG dst_w, dst_h, dst_x, dst_y;
    if (frame_aspect > window_aspect) {
        dst_w = win_w;
        dst_h = (LONG)(win_w / frame_aspect);
        dst_x = 0;
        dst_y = (win_h - dst_h) / 2;
    } else {
        dst_h = win_h;
        dst_w = (LONG)(win_h * frame_aspect);
        dst_x = (win_w - dst_w) / 2;
        dst_y = 0;
    }

    // Clear so letterbox bars are black.
    const float clear[4] = { 0, 0, 0, 1 };
    context->ClearRenderTargetView(rtv.Get(), clear);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = (FLOAT)dst_x; vp.TopLeftY = (FLOAT)dst_y;
    vp.Width    = (FLOAT)dst_w; vp.Height   = (FLOAT)dst_h;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    context->RSSetViewports(1, &vp);

    ID3D11RenderTargetView* rtvs[] = { rtv.Get() };
    context->OMSetRenderTargets(1, rtvs, nullptr);

    // Update cbuffer.
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(context->Map(cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float cb[4] = {
            (float)crop_w,  (float)crop_h,
            1.0f/(float)frame_w, 1.0f/(float)frame_h
        };
        memcpy(m.pData, cb, sizeof(cb));
        context->Unmap(cbuffer, 0);
    }

    context->IASetInputLayout(nullptr);
    context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs, nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer*             cbs[]  = { cbuffer };
    ID3D11ShaderResourceView* srvs[] = { intermediate_srv.Get() };
    ID3D11SamplerState*       sams[] = { sampler };
    context->PSSetConstantBuffers(0, 1, cbs);
    context->PSSetShaderResources(0, 1, srvs);
    context->PSSetSamplers(0, 1, sams);

    context->Draw(3, 0);   // full-screen triangle, no IB / VB

    // Unbind so the next frame's VP write to intermediate isn't blocked.
    ID3D11ShaderResourceView* null_srv[] = { nullptr };
    context->PSSetShaderResources(0, 1, null_srv);
    ID3D11RenderTargetView*   null_rtv[] = { nullptr };
    context->OMSetRenderTargets(1, null_rtv, nullptr);

    return SUCCEEDED(swapchain->Present(0, 0));   // no vsync = lowest latency
}
```

The sampler is a single linear-wrap pulled from
`device->CreateSamplerState(&{D3D11_FILTER_MIN_MAG_MIP_LINEAR, ...})`.

## Stage 8 — Window resize

Two things change on resize: the swap-chain back buffer dims, and the
window-side viewport math.  The decoded-frame-side intermediate texture
stays the same (its size is the decoder texture size, not the window).

```cpp
bool resize(uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0) return false;
    // Release the back buffer reference if you have one cached.
    HRESULT hr = swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) return false;
    window_w = w; window_h = h;
    return true;
}
```

If you cached the `ID3D11RenderTargetView` from the back buffer (we
don't in the snippet above — we re-create per frame), you must
release it before `ResizeBuffers` or it returns
`DXGI_ERROR_INVALID_CALL`.

For Qt: convert logical → physical pixels via `devicePixelRatio()`
before sizing the swap chain (high-DPI displays):

```cpp
qreal dpr = widget->devicePixelRatio();
uint32_t physical_w = static_cast<uint32_t>(widget->width()  * dpr);
uint32_t physical_h = static_cast<uint32_t>(widget->height() * dpr);
```

## Stage 9 — Decoder reset / packet loss

After heavy packet loss, request a new IDR from the host, and before
feeding it:

```cpp
transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
while (!output_frames.empty()) output_frames.pop();
```

Without the flush the decoder's reference picture buffer (DPB) holds
stale POC values from the previous GOP and complains "Could not find
ref with POC X" on every P-frame after the IDR.

## Stage 10 — Robustness notes from real-world testing

* **SEH around ProcessOutput.**  Buggy decoder MFTs (Intel iGPU under
  thermal throttle, NVidia driver mid-update) can throw `EXCEPTION_*`
  from inside `ProcessOutput`.  Wrap the call in
  `__try`/`__except (EXCEPTION_EXECUTE_HANDLER)` and mark the decoder
  dead on hit; user can reconnect to get a fresh MFT.

* **`MF_E_TRANSFORM_STREAM_CHANGE`** mid-stream is normal — host
  changed resolution.  Re-run `configure_output()` and keep going.
  Do not destroy the decoder.

* **`MF_E_TRANSFORM_NEED_MORE_INPUT`** is normal between frames —
  break out of the drain loop and feed the next sample.

* **HEVC needs the codec extension on Win10.**  HEVC MFT isn't shipped
  by default; user installs "HEVC Video Extensions" from MS Store.
  H.264 is always there.

* **DPB explosion on long streams.**  Some decoders leak references
  to old samples internally — periodic `MFT_MESSAGE_COMMAND_FLUSH` (e.g.
  per keyframe boundary) keeps memory bounded.

* **`Present(0, 0)` vs vsync.**  No-vsync is the lowest input-to-photon
  latency.  If you see tearing complaints, use `Present(1, 0)` —
  trades ~8 ms latency for sync to display refresh.

* **HDR detection.**  Decode the bitstream side data
  (`AVMasteringDisplayMetadata`, `AVContentLightMetadata` via FFmpeg —
  or VUI parameters from the SPS — for VTB you get `pixelFormat ==
  kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange`).  If color
  primaries == BT.2020 + transfer == SMPTE 2084 (PQ), treat as HDR.

---

# Stage 11 — Interactive QML overlay (parameter panels, sliders, dropdowns)

The base pipeline owns the HWND outright — `WA_PaintOnScreen +
paintEngine() = nullptr` blocks Qt's compositor entirely.  A child
`QQuickWidget` won't paint, a child QML scene won't paint, even a
child `QLabel` won't paint.  That's why DeskBeam's existing HUD had
to be a separate top-level frameless tool window — cheap to ship, but
a separate HWND lags behind the parent on rapid moves/resizes and
complicates input.

The right architecture for a real overlay (settings panel, sliders,
dropdowns, menus) is to render the QML scene off-screen into a D3D11
texture and composite it inside the existing Pass 2 shader:

```
┌─ Decoded ID3D11Texture2D (NV12/P010 from MF) ───┐
│   │                                              │
│   ▼ VideoProcessorBlt                            │
│ intermediate (RGB)                               │
│   │                                              │
└───┼──────────────────────────────────────────────┘
    │
    │           ┌─ QML scene (offscreen) ───────────┐
    │           │ QQuickRenderControl renders the   │
    │           │ QML graph into a D3D11 RT texture │
    │           │ (RGBA8, premultiplied alpha)      │
    │           │   │                               │
    │           └───┼───────────────────────────────┘
    │               │
    ▼               ▼
┌────────────────────────────────────────┐
│ Pass 2 pixel shader (bicubic + COMP)   │
│  sample video   (intermediate, SRV0)   │
│  sample overlay (QML tex,     SRV1)    │
│  out = overlay.rgb + video.rgb*(1-a)   │
└──────────────────────────────────────┬─┘
                                       │
                                       ▼
                                 swap chain
```

Same HWND, same `Present`, pixel-perfect alpha blend, no compositor
mismatch.  The price is a chunky `QQuickRenderControl` integration
plus input event forwarding.  Recipe below.

## 11a. Share the D3D11 device with Qt RHI

Qt 6's default RHI backend on Windows is D3D11 (since 6.5).  We tell
QQuickWindow to use *our* device instead of letting it create its own:

```cpp
#include <QQuickGraphicsDevice>
#include <QQuickRenderTarget>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QSGRendererInterface>

// Process-wide setting.  Call BEFORE constructing any QQuickWindow.
QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
```

Sharing the device makes the QML render target a sibling resource our
Pass 2 shader samples with zero handle marshaling.  Without sharing,
you'd need keyed-mutex texture sharing or OS-level shared handles
across two devices — both kill latency.

## 11b. Build the offscreen QML scene

`QQuickRenderControl` runs Qt Quick without an OS window.  We give it
a `QQuickWindow` instance to host the scene graph (it's never shown)
and a render-target texture sized to our window client area.

```cpp
class QmlOverlay {
public:
    bool init(ID3D11Device* device, ID3D11DeviceContext* ctx,
              const QUrl& qmlEntry, QSize size);
    void resize(QSize size);
    bool render();                          // true if frame produced
    ID3D11ShaderResourceView* srv() const { return rt_srv_.Get(); }

    // Input forwarding — returns true if QML "consumed" the event
    // (interactive item under the cursor / focus item ate the key).
    // Caller suppresses forwarding to the remote host on true.
    bool sendMouse(QPoint posInWindow, QMouseEvent* e);
    bool sendWheel(QPoint posInWindow, QWheelEvent* e);
    bool sendKey  (QKeyEvent* e);

private:
    void rebuildRenderTarget(QSize size);

    QQuickRenderControl* rc_     = nullptr;
    QQuickWindow*        qwin_   = nullptr;
    QQmlEngine*          engine_ = nullptr;
    QQmlComponent*       comp_   = nullptr;
    QQuickItem*          root_   = nullptr;

    ComPtr<ID3D11Device>           device_;
    ComPtr<ID3D11DeviceContext>    ctx_;
    ComPtr<ID3D11Texture2D>        rt_tex_;     // QML's render target
    ComPtr<ID3D11ShaderResourceView> rt_srv_;   // Pass 2 samples this
    QSize size_;
    bool  dirty_ = true;
};

bool QmlOverlay::init(ID3D11Device* device, ID3D11DeviceContext* ctx,
                     const QUrl& qmlEntry, QSize size)
{
    device_ = device;
    ctx_    = ctx;
    size_   = size;

    rc_   = new QQuickRenderControl();
    qwin_ = new QQuickWindow(rc_);
    qwin_->setColor(Qt::transparent);    // empty regions render as (0,0,0,0)

    // Hand our D3D11 device to the QQuickWindow's RHI scene graph.
    qwin_->setGraphicsDevice(
        QQuickGraphicsDevice::fromDeviceAndContext(device_.Get(), ctx_.Get()));

    rebuildRenderTarget(size);

    // initialize() boots the RHI scene graph.  MUST come AFTER
    // setGraphicsDevice / setRenderTarget — otherwise Qt creates its
    // own device and our SRV points at a foreign resource.
    if (!rc_->initialize()) {
        qWarning("QQuickRenderControl::initialize failed");
        return false;
    }

    // Load QML.
    engine_ = new QQmlEngine();
    comp_   = new QQmlComponent(engine_, qmlEntry);
    if (comp_->isError()) {
        for (const auto& e : comp_->errors()) qWarning() << "QML:" << e.toString();
        return false;
    }
    QObject* obj = comp_->create();
    root_ = qobject_cast<QQuickItem*>(obj);
    if (!root_) { qWarning("QML root is not a QQuickItem"); return false; }

    root_->setParentItem(qwin_->contentItem());
    qwin_->contentItem()->setSize(size_);
    root_->setSize(size_);

    // Dirty triggers — re-render only when the QML scene actually changed.
    QObject::connect(rc_, &QQuickRenderControl::renderRequested,
                     [this] { dirty_ = true; });
    QObject::connect(rc_, &QQuickRenderControl::sceneChanged,
                     [this] { dirty_ = true; });
    return true;
}

void QmlOverlay::rebuildRenderTarget(QSize size)
{
    rt_srv_.Reset();
    rt_tex_.Reset();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width  = size.width();
    td.Height = size.height();
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;   // RGBA + alpha
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    device_->CreateTexture2D(&td, nullptr, rt_tex_.GetAddressOf());

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(rt_tex_.Get(), &sd, rt_srv_.GetAddressOf());

    // Tell Qt RHI this is where to draw.  Qt 6.5+ API.
    qwin_->setRenderTarget(QQuickRenderTarget::fromNativeTexture(
        { rt_tex_.Get(), 0 }, size));
    qwin_->resize(size);
    size_ = size;
    dirty_ = true;
}

void QmlOverlay::resize(QSize size)
{
    if (size == size_) return;
    rebuildRenderTarget(size);
    qwin_->contentItem()->setSize(size);
    if (root_) root_->setSize(size);
}
```

Non-obvious bits:

* `setGraphicsDevice` MUST come before `initialize()`, otherwise Qt
  builds its own D3D11 device and the SRV we hand to Pass 2 lives on
  a foreign device — sampling fails silently or crashes deep in RHI.

* `QQuickWindow::setColor(Qt::transparent)` is what lets the `lerp`
  in the shader keep video visible where no QML element is drawn —
  empty regions pass through as `(0,0,0,0)`.

* `QQuickRenderTarget::fromNativeTexture` ties the QML scene's pixels
  to *our* `ID3D11Texture2D`.  Qt RHI writes premultiplied alpha into
  it by default; the composite shader below assumes that.

## 11c. Per-frame render cycle

Drive the QML scene from the same tick that drives Pass 1/Pass 2.  In
DeskBeam's `ViewLoopState::iter()` it lives between Pass 1 (VP blit)
and Pass 2 (shader composite):

```cpp
bool QmlOverlay::render()
{
    if (!dirty_) return false;   // nothing changed — last texture is still valid
    rc_->beginFrame();            // Qt 6 explicit frame boundary
    rc_->polishItems();           // run QML layout / animation tick
    rc_->sync();                  // GUI-side scene → render-side
    rc_->render();                // actually draws into rt_tex_
    rc_->endFrame();
    dirty_ = false;
    return true;
}
```

Two-thread vs single-thread: `QQuickRenderControl` was designed for
two-thread use (sync on GUI, render on render thread).  **Single-thread
is fine** as long as `beginFrame/.../endFrame` runs on the thread
that owns the QQuickWindow.  In DeskBeam the view loop runs on the
main GUI thread (since Phase A.2) so just inline.

Don't render when `dirty_ == false`: QML scenes are mostly static
unless animated.  Saves ~0.5-1 ms per frame.

## 11d. Pass 2 shader v2 — composite QML over video

Single draw call, two SRVs, alpha blend in shader (no D3D blend state
needed; math is cleaner and easier to extend with global opacity / dim).

```cpp
// C++ side — add overlay SRV to the SRV array.
ID3D11ShaderResourceView* srvs[] = {
    intermediate_srv.Get(),    // t0 — video
    overlay.srv(),             // t1 — QML
};
context->PSSetShaderResources(0, 2, srvs);
```

```hlsl
// Pixel shader, Pass 2 v2.
Texture2D    tex_video   : register(t0);
Texture2D    tex_overlay : register(t1);
SamplerState samp        : register(s0);
cbuffer CB : register(b0) { float4 cb; };

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

// mitchell() helper from Stage 7 ...

float4 main(VSOut i) : SV_Target {
    // Bicubic upsample of video (unchanged from Stage 7).
    float2 src_px = i.uv * cb.xy;
    float2 base   = floor(src_px - 0.5) + 0.5;
    float2 f      = src_px - base;
    float4 video = 0;  float wsum = 0;
    [unroll] for (int dy = -1; dy <= 2; ++dy) {
        [unroll] for (int dx = -1; dx <= 2; ++dx) {
            float2 sp = base + float2(dx, dy);
            float  w  = mitchell(dx - f.x) * mitchell(dy - f.y);
            video += tex_video.SampleLevel(samp, sp * cb.zw, 0) * w;
            wsum  += w;
        }
    }
    video /= wsum;

    // QML overlay is rendered at swap-chain resolution, so i.uv maps 1:1.
    // Linear sample is fine (text rendering inside QML already AA'd).
    float4 overlay = tex_overlay.Sample(samp, i.uv);

    // Premultiplied alpha (Qt RHI default).
    //   out = overlay.rgb + video.rgb * (1 - overlay.a)
    return float4(overlay.rgb + video.rgb * (1.0 - overlay.a), 1.0);
}
```

If Qt RHI in your version writes straight (non-premultiplied) alpha,
swap the last line for `lerp(video.rgb, overlay.rgb, overlay.a)`.
Sanity test: drop a `Rectangle { color: "#80ff0000" }` (50% red) in
QML — premultiplied path renders dim red over the stream; straight
alpha renders bright red blended.  Adjust shader for whichever you see.

## 11e. Input forwarding (the fiddly part)

The HWND only sees events through StreamWindow's Qt event handlers.
We need to:

1. Decide who consumes each event — QML overlay or the remote host.
2. Map StreamWindow coords to QML scene coords (1:1 because the QML
   render target is the same size as the swap chain back buffer).
3. Synthesise a Qt event targeted at the offscreen `QQuickWindow`.

```cpp
bool QmlOverlay::sendMouse(QPoint posInWindow, QMouseEvent* e)
{
    if (!qwin_) return false;

    // Hit test: is there an interactive QML item under this point?
    // childAt() returns nullptr for empty regions or disabled items.
    QQuickItem* hit = qwin_->contentItem()->childAt(posInWindow.x(),
                                                    posInWindow.y());
    if (!hit || !hit->isVisible() || !hit->isEnabled()) return false;

    // Build a fresh event in QML window coords.  Cannot re-use `e`
    // directly — its scenePos/screenPos refer to the StreamWindow, not
    // our offscreen QQuickWindow.
    QMouseEvent ev(e->type(),
                   QPointF(posInWindow), QPointF(posInWindow), QPointF(posInWindow),
                   e->button(), e->buttons(), e->modifiers());
    QCoreApplication::sendEvent(qwin_, &ev);
    return ev.isAccepted();
}

bool QmlOverlay::sendKey(QKeyEvent* e)
{
    if (!qwin_) return false;
    if (!qwin_->activeFocusItem()) return false;   // nothing wants keys
    QCoreApplication::sendEvent(qwin_, e);
    return e->isAccepted();
}
```

In StreamWindow:

```cpp
void StreamWindow::mousePressEvent(QMouseEvent* e) {
    // QML overlay first — if it handles, don't send to remote host.
    if (qml_overlay_ && qml_overlay_->sendMouse(e->pos(), e)) return;
    forward_to_host(e);   // existing relative / absolute mouse path
}
```

**Edge case: opening the menu.**  If the menu is triggered by a button
drawn in the remote stream pixels (not in QML), QML can't hit-test it
because it's not a QML item.  Two patterns:

* **Keybind toggle** — F11 or similar opens/closes the QML overlay.
  Simplest, most predictable.
* **Click-through region** — invisible `MouseArea {
  propagateComposedEvents: true }` outside the menu; clicks outside
  the menu propagate to the remote.  Feels more seamless but fiddlier.

Always track a `bool overlay_visible_` and skip the hit-test entirely
when the overlay is hidden — both for perf and to avoid stealing
clicks from fullscreen games on the remote.

## 11f. Resize handling

When StreamWindow resizes, both the swap chain back buffer AND the
QML render texture follow:

```cpp
void StreamWindow::resizeEvent(QResizeEvent* e) {
    QSize physical = e->size() * devicePixelRatio();
    renderer_.resize(physical.width(), physical.height());
    if (qml_overlay_) qml_overlay_->resize(physical);
    QWidget::resizeEvent(e);
}
```

`devicePixelRatio()` matters: QML needs to render at physical pixels
to keep text crisp on high-DPI displays.

If you want QML to position items in *logical* pixels regardless of
DPR, use `QQuickWindow::setEffectiveDevicePixelRatio()`.  Don't
render at logical size and upscale in Pass 2 — text quality dies.

## 11g. Skipping the composite when the overlay is empty

Cost when nothing's visible (no menu open, no HUD):

* `QmlOverlay::render()` no-ops because `dirty_` stays false after the
  last full-clear repaint.
* Pass 2 still does the second `Sample` + ALU — measurable but ~0.05 ms.

Cheap perf win: keep a `bool overlay_visible_` flag and at Pass 2
either (a) bind a 1×1 fully-transparent texture as `t1` when hidden,
or (b) switch shader permutations (no-overlay vs overlay variants).
(a) is one extra texture; (b) is one extra shader.  Pick by taste.

## 11h. Lifecycle and teardown

Order matters at shutdown — QQuickRenderControl accesses the
graphics device during destruction:

```cpp
QmlOverlay::~QmlOverlay() {
    delete root_;       root_   = nullptr;
    delete comp_;       comp_   = nullptr;
    delete engine_;     engine_ = nullptr;
    if (rc_) rc_->invalidate();   // tear down RHI scene graph
    delete qwin_;       qwin_   = nullptr;
    delete rc_;         rc_     = nullptr;
    // ID3D11 ComPtrs release after.  All good.
}
```

`rc_->invalidate()` must happen BEFORE the QQuickWindow is destroyed
AND BEFORE our D3D11 device dies, otherwise crashes deep in QRhi
during QML shader teardown.

## 11i. Known limitations / per-Qt-version gotchas

* **Qt version floor: 6.5** for `QQuickRenderTarget::fromNativeTexture`
  + `QQuickGraphicsDevice::fromDeviceAndContext` D3D11 path.  Earlier
  versions had ad-hoc APIs (`QQuickGraphicsConfiguration`, lower-level
  RHI access).  Always verify against your Qt docs.

* **Premultiplied vs straight alpha** — Qt RHI default has shifted
  between minor versions.  Use the dim-red rectangle test described
  in 11d.

* **HDR + QML mixing** — if the swap chain is `R10G10B10A2 + PQ`
  (HDR10) and QML is rendered as sRGB BGRA, you must re-encode the
  overlay through PQ before alpha-blending or it'll look washed out.
  Easiest fix: render QML in linear-light and apply PQ at the very
  end of the composite shader.  Out of scope here; PQ encoding lives
  in `SetColorSpace1(...G2084_NONE_P2020)` semantics.

* **Per-tick latency on overlay edits** — interactive feel needs the
  overlay to update the moment the user clicks.  If the render loop
  is paced to host video frames, a click delays its visual response
  by up to one frame interval.  Wire `QQuickRenderControl::renderRequested`
  to bump the `dirty_` flag and call `update()` on the StreamWindow
  — the loop already runs every 16 ms via the QTimer driver, so
  worst-case latency is ~32 ms.

## Reference: Qt docs entry points

* [`QQuickRenderControl`](https://doc.qt.io/qt-6/qquickrendercontrol.html)
* [`QQuickWindow::setGraphicsDevice`](https://doc.qt.io/qt-6/qquickwindow.html#setGraphicsDevice)
* [`QQuickGraphicsDevice::fromDeviceAndContext`](https://doc.qt.io/qt-6/qquickgraphicsdevice.html#fromDeviceAndContext)
* [`QQuickRenderTarget::fromNativeTexture`](https://doc.qt.io/qt-6/qquickrendertarget.html#fromNativeTexture)
* The Qt source ships a working sample at
  `examples/quickcontrols/rendercontrol/d3d11` — minimal but real
  D3D11 + QQuickRenderControl wiring; closest analog to this recipe.

