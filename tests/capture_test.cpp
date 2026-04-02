#ifdef DESKBEAM_WINDOWS

#include "host/capture/screen_capture.h"
#include "host/capture/dxgi_capture.h"
#include "common/utils/log.h"
#include <cassert>
#include <cstdio>
#include <d3d11.h>
#include <windows.h>

// Save captured frame to BMP for visual verification
static bool save_texture_to_bmp(ID3D11Device* device, ID3D11DeviceContext* ctx,
                                 ID3D11Texture2D* texture, const char* path) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

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
    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    *reinterpret_cast<uint32_t*>(header + 2) = file_size;
    *reinterpret_cast<uint32_t*>(header + 10) = 54;
    *reinterpret_cast<uint32_t*>(header + 14) = 40;
    *reinterpret_cast<int32_t*>(header + 18) = w;
    *reinterpret_cast<int32_t*>(header + 22) = -(int32_t)h; // top-down
    *reinterpret_cast<uint16_t*>(header + 26) = 1;
    *reinterpret_cast<uint16_t*>(header + 28) = 24;
    *reinterpret_cast<uint32_t*>(header + 34) = pixel_data_size;

    fwrite(header, 1, 54, f);

    // Write pixels (BGRA -> BGR)
    auto* src = static_cast<uint8_t*>(mapped.pData);
    std::vector<uint8_t> row(row_padded, 0);
    for (uint32_t y = 0; y < h; ++y) {
        auto* src_row = src + y * mapped.RowPitch;
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 3 + 0] = src_row[x * 4 + 0]; // B
            row[x * 3 + 1] = src_row[x * 4 + 1]; // G
            row[x * 3 + 2] = src_row[x * 4 + 2]; // R
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
    assert(frame.texture != nullptr);
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
        dxgi->get_context()->CopyResource(copy, frame.texture);

        capture->release_frame(frame);

        if (save_texture_to_bmp(dxgi->get_device(), dxgi->get_context(),
                                copy, "capture_test.bmp")) {
            deskbeam::log::info("TEST", "PASS: Saved capture_test.bmp");
        } else {
            deskbeam::log::warn("TEST", "WARN: Could not save BMP (non-fatal)");
        }
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
