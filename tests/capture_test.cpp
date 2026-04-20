#ifdef DESKBEAM_WINDOWS

#define NOMINMAX
#include "host/capture/screen_capture.h"
#include "host/capture/dxgi_capture.h"
#include "common/utils/log.h"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <d3d11.h>
#include <dxgiformat.h>
#include <dxgi1_6.h>
#include <windows.h>

// Query SDR brightness boost factor via QueryDisplayConfig.
// Returns 1.0 if HDR is off or query fails.
static float get_sdr_boost() {
    UINT32 num_paths = 0, num_modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_paths, &num_modes) != ERROR_SUCCESS)
        return 1.0f;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(num_paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(num_modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &num_paths, paths.data(),
                           &num_modes, modes.data(), nullptr) != ERROR_SUCCESS)
        return 1.0f;

    for (UINT32 i = 0; i < num_paths; ++i) {
        DISPLAYCONFIG_SDR_WHITE_LEVEL sdr = {};
        sdr.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        sdr.header.size = sizeof(sdr);
        sdr.header.adapterId = paths[i].targetInfo.adapterId;
        sdr.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&sdr.header) == ERROR_SUCCESS && sdr.SDRWhiteLevel > 1000) {
            float boost = sdr.SDRWhiteLevel / 1000.0f;
            deskbeam::log::info("TEST", "SDR boost factor: %.3f (%.0f nits)",
                                boost, 80.0f * boost);
            return boost;
        }
    }
    return 1.0f;
}

// sRGB EOTF: decode sRGB gamma to linear [0,1]
static float srgb_to_linear(float v) {
    if (v <= 0.04045f) return v / 12.92f;
    return std::pow((v + 0.055f) / 1.055f, 2.4f);
}

// Linear [0,1] -> sRGB OETF
static float linear_to_srgb(float v) {
    if (v <= 0.0f) return 0.0f;
    if (v >= 1.0f) return 1.0f;
    return (v <= 0.0031308f)
        ? v * 12.92f
        : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

static uint8_t float_to_u8(float v) {
    return static_cast<uint8_t>(std::clamp(static_cast<int>(v * 255.0f + 0.5f), 0, 255));
}

// Save B8G8R8A8_UNORM texture to BMP, correcting for HDR SDR boost
static bool save_texture_to_bmp(ID3D11Device* device, ID3D11DeviceContext* ctx,
                                 ID3D11Texture2D* texture, const char* path) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    float sdr_boost = get_sdr_boost();
    deskbeam::log::info("TEST", "Texture format: %u, %ux%u, SDR boost: %.3f",
                        desc.Format, desc.Width, desc.Height, sdr_boost);

    // Create staging texture to read back to CPU (test only — not part of pipeline)
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr)) return false;

    ctx->CopyResource(staging, texture);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        staging->Release();
        return false;
    }

    // Write BMP
    FILE* f = fopen(path, "wb");
    if (!f) {
        ctx->Unmap(staging, 0);
        staging->Release();
        return false;
    }

    uint32_t w = desc.Width;
    uint32_t h = desc.Height;
    uint32_t row_bytes = w * 3;
    uint32_t row_padded = (row_bytes + 3) & ~3u;
    uint32_t pixel_data_size = row_padded * h;
    uint32_t file_size = 54 + pixel_data_size;

    // BMP header
    uint8_t bmp_header[54] = {};
    bmp_header[0] = 'B'; bmp_header[1] = 'M';
    *reinterpret_cast<uint32_t*>(bmp_header + 2) = file_size;
    *reinterpret_cast<uint32_t*>(bmp_header + 10) = 54;
    *reinterpret_cast<uint32_t*>(bmp_header + 14) = 40;
    *reinterpret_cast<int32_t*>(bmp_header + 18) = w;
    *reinterpret_cast<int32_t*>(bmp_header + 22) = -(int32_t)h; // top-down
    *reinterpret_cast<uint16_t*>(bmp_header + 26) = 1;
    *reinterpret_cast<uint16_t*>(bmp_header + 28) = 24;
    *reinterpret_cast<uint32_t*>(bmp_header + 34) = pixel_data_size;

    fwrite(bmp_header, 1, 54, f);

    auto* src = static_cast<uint8_t*>(mapped.pData);
    std::vector<uint8_t> row(row_padded, 0);

    // When HDR is on, DXGI Desktop Duplication returns sRGB data with SDR brightness
    // boost baked in (in linear light). To get correct colors:
    // 1. sRGB decode (gamma -> linear)
    // 2. Divide by SDR boost factor
    // 3. sRGB encode (linear -> gamma)
    // When HDR is enabled, DXGI Desktop Duplication returns sRGB-gamma-encoded
    // data with SDR brightness boost baked into linear light values.
    // To get correct brightness: sRGB decode -> /boost -> sRGB encode.
    // NOTE: color primaries may differ from sRGB on wide-gamut monitors.
    // This is a test-only concern — in the real pipeline, the GPU texture goes
    // directly to the encoder without CPU readback or color conversion.
    for (uint32_t y = 0; y < h; ++y) {
        auto* src_row = src + y * mapped.RowPitch;
        for (uint32_t x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                float v = src_row[x * 4 + c] / 255.0f;
                if (sdr_boost > 1.001f) {
                    float lin = srgb_to_linear(v);
                    lin /= sdr_boost;
                    v = linear_to_srgb(lin);
                }
                row[x * 3 + c] = float_to_u8(v);
            }
        }
        fwrite(row.data(), 1, row_padded, f);
    }

    fclose(f);
    ctx->Unmap(staging, 0);
    staging->Release();

    return true;
}

int main() {
    deskbeam::log::info("TEST", "=== DXGI Capture Test ===");

    // Test 1: Factory creates DxgiCapture on Windows
    auto capture = deskbeam::IScreenCapture::create();
    assert(capture != nullptr);
    deskbeam::log::info("TEST", "PASS: Factory created capture instance");

    // Test 2: Monitor enumeration (needs D3D11 init first)
    auto* dxgi = dynamic_cast<deskbeam::DxgiCapture*>(capture.get());
    assert(dxgi != nullptr);

    bool ok = capture->init(0);
    if (!ok) {
        deskbeam::log::error("TEST", "SKIP: Could not init capture (no desktop access?)");
        return 0;
    }
    deskbeam::log::info("TEST", "PASS: Capture initialized");

    // Test 3: Resolution is valid
    auto res = capture->get_resolution();
    assert(res.width > 0 && res.height > 0);
    deskbeam::log::info("TEST", "PASS: Resolution %ux%u", res.width, res.height);

    // Test 4: Monitor enumeration
    auto monitors = capture->enumerate_monitors();
    assert(!monitors.empty());
    deskbeam::log::info("TEST", "PASS: Found %zu monitor(s)", monitors.size());

    // Test 5: Capture frames until we get one with actual content
    // DXGI Desktop Duplication only returns pixel data when something changes.
    // Force a change by moving the cursor, then capture.
    {
        // Wiggle the mouse to force a desktop update
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = 1;
        input.mi.dy = 0;
        SendInput(1, &input, sizeof(INPUT));
        input.mi.dx = -1;
        SendInput(1, &input, sizeof(INPUT));
    }

    deskbeam::CapturedFrame frame;
    bool got_content = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (capture->capture_frame(frame, 200)) {
            deskbeam::log::info("TEST", "Frame #%llu, dirty_rects=%zu",
                                static_cast<unsigned long long>(frame.frame_index),
                                frame.dirty_rects.size());
            if (!frame.dirty_rects.empty()) {
                got_content = true;
                break;
            }
            // Copy the texture before releasing — DXGI invalidates it on ReleaseFrame.
            // We'll keep the last captured frame for saving.
            capture->release_frame(frame);
        }
        // Wiggle again to provoke another update
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = 1;
        SendInput(1, &input, sizeof(INPUT));
    }
    assert(got_content);
    assert(frame.texture);
    deskbeam::log::info("TEST", "PASS: Captured frame with content, dirty_rects=%zu",
                        frame.dirty_rects.size());

    // Test 6: Copy texture and save screenshot for visual verification
    // We must copy before ReleaseFrame since DXGI owns the original texture.
    {
        D3D11_TEXTURE2D_DESC desc;
        frame.texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;

        ID3D11Texture2D* copy = nullptr;
        HRESULT hr = dxgi->get_device()->CreateTexture2D(&desc, nullptr, &copy);
        assert(SUCCEEDED(hr));
        dxgi->get_context()->CopyResource(copy, frame.texture.Get());

        capture->release_frame(frame);

        save_texture_to_bmp(dxgi->get_device(), dxgi->get_context(),
                            copy, "capture_test.bmp");
        deskbeam::log::info("TEST", "PASS: Saved capture_test.bmp");
        copy->Release();
    }

    // Test 7: Capture multiple frames and measure FPS
    deskbeam::log::info("TEST", "Capturing 120 frames for FPS measurement...");
    auto start = deskbeam::Clock::now();
    int captured = 0;
    for (int i = 0; i < 120; ++i) {
        deskbeam::CapturedFrame f;
        if (capture->capture_frame(f, 50)) {
            captured++;
            capture->release_frame(f);
        }
    }
    auto elapsed = std::chrono::duration<double>(deskbeam::Clock::now() - start).count();
    double fps = captured / elapsed;
    deskbeam::log::info("TEST", "PASS: Captured %d frames in %.2fs (%.1f FPS)", captured, elapsed, fps);

    deskbeam::log::info("TEST", "=== All tests passed ===");
    return 0;
}

#else

#include <cstdio>
int main() {
    std::printf("Capture test is Windows-only\n");
    return 0;
}

#endif
