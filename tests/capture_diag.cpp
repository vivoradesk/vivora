// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef VIVORA_WINDOWS

#define NOMINMAX
#include "host/capture/screen_capture.h"
#include "host/capture/dxgi_capture.h"
#include "common/utils/log.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>
#include "check.h"

using Microsoft::WRL::ComPtr;

int main() {
    vivora::log::info("DIAG", "=== DXGI Capture Diagnostics ===");

    auto capture = vivora::IScreenCapture::create();
    auto* dxgi = dynamic_cast<vivora::DxgiCapture*>(capture.get());
    CHECK(capture->init(0));

    auto* device = dxgi->get_device();
    auto* ctx = dxgi->get_context();

    // --- Output / monitor info ---
    ComPtr<IDXGIDevice> dxgi_dev;
    device->QueryInterface(IID_PPV_ARGS(&dxgi_dev));
    ComPtr<IDXGIAdapter> adapter;
    dxgi_dev->GetAdapter(&adapter);
    ComPtr<IDXGIOutput> output;
    adapter->EnumOutputs(0, &output);
    ComPtr<IDXGIOutput6> output6;
    output.As(&output6);

    if (output6) {
        DXGI_OUTPUT_DESC1 d;
        output6->GetDesc1(&d);
        vivora::log::info("DIAG", "ColorSpace: %u", d.ColorSpace);
        vivora::log::info("DIAG", "BitsPerColor: %u", d.BitsPerColor);
        vivora::log::info("DIAG", "RedPrimary: (%.4f, %.4f)", d.RedPrimary[0], d.RedPrimary[1]);
        vivora::log::info("DIAG", "GreenPrimary: (%.4f, %.4f)", d.GreenPrimary[0], d.GreenPrimary[1]);
        vivora::log::info("DIAG", "BluePrimary: (%.4f, %.4f)", d.BluePrimary[0], d.BluePrimary[1]);
        vivora::log::info("DIAG", "WhitePoint: (%.4f, %.4f)", d.WhitePoint[0], d.WhitePoint[1]);
        vivora::log::info("DIAG", "MinLuminance: %.2f nits", d.MinLuminance);
        vivora::log::info("DIAG", "MaxLuminance: %.2f nits", d.MaxLuminance);
        vivora::log::info("DIAG", "MaxFullFrameLuminance: %.2f nits", d.MaxFullFrameLuminance);
    }

    // --- Capture a frame and examine the texture ---
    // Wiggle mouse to ensure we get content
    INPUT mi = {};
    mi.type = INPUT_MOUSE;
    mi.mi.dwFlags = MOUSEEVENTF_MOVE;
    mi.mi.dx = 1;
    SendInput(1, &mi, sizeof(INPUT));
    mi.mi.dx = -1;
    SendInput(1, &mi, sizeof(INPUT));

    vivora::CapturedFrame frame;
    bool got = false;
    for (int i = 0; i < 30; ++i) {
        if (capture->capture_frame(frame, 200) && !frame.dirty_rects.empty()) {
            got = true;
            break;
        }
        if (frame.texture) capture->release_frame(frame);
        mi.mi.dx = 1;
        SendInput(1, &mi, sizeof(INPUT));
    }
    CHECK(got);

    D3D11_TEXTURE2D_DESC td;
    frame.texture->GetDesc(&td);
    vivora::log::info("DIAG", "Texture format: %u", td.Format);
    vivora::log::info("DIAG", "Texture size: %ux%u", td.Width, td.Height);

    // Map to CPU for sampling
    D3D11_TEXTURE2D_DESC staging_desc = td;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    device->CreateTexture2D(&staging_desc, nullptr, &staging);
    ctx->CopyResource(staging, frame.texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);

    auto* pixels = static_cast<uint8_t*>(mapped.pData);

    // Sample grid
    vivora::log::info("DIAG", "--- Pixel samples (BGRA) ---");
    uint32_t w = td.Width, h = td.Height;
    for (int yp = 5; yp <= 95; yp += 15) {
        for (int xp = 5; xp <= 95; xp += 30) {
            uint32_t x = w * xp / 100;
            uint32_t y = h * yp / 100;
            auto* p = pixels + y * mapped.RowPitch + x * 4;
            vivora::log::info("DIAG", "  (%u,%u) [%d%%,%d%%] B=%3u G=%3u R=%3u A=%3u",
                                x, y, xp, yp, p[0], p[1], p[2], p[3]);
        }
    }

    // Histogram: count values in buckets of 16
    int histogram[16] = {};
    uint64_t total_brightness = 0;
    uint32_t pixel_count = 0;
    for (uint32_t y = 0; y < h; y += 4) {
        auto* row = pixels + y * mapped.RowPitch;
        for (uint32_t x = 0; x < w; x += 4) {
            auto* p = row + x * 4;
            uint32_t lum = (p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000; // perceived brightness
            histogram[lum / 16]++;
            total_brightness += lum;
            pixel_count++;
        }
    }
    vivora::log::info("DIAG", "--- Brightness histogram ---");
    for (int i = 0; i < 16; ++i) {
        vivora::log::info("DIAG", "  [%3d-%3d]: %d", i*16, i*16+15, histogram[i]);
    }
    vivora::log::info("DIAG", "  Average brightness: %.1f / 255",
                        (double)total_brightness / pixel_count);

    ctx->Unmap(staging, 0);
    staging->Release();
    capture->release_frame(frame);

    vivora::log::info("DIAG", "=== Done ===");
    return check_report(nullptr);
}

#else
int main() { return 0; }
#endif
