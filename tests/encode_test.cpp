#ifdef DESKBEAM_WINDOWS

#define NOMINMAX
#include "host/capture/screen_capture.h"
#include "host/capture/dxgi_capture.h"
#include "host/encode/video_encoder.h"
#include "common/utils/log.h"
#include "common/utils/metrics.h"
#include <cassert>
#include <cstdio>
#include <windows.h>

int main() {
    deskbeam::log::info("TEST", "=== Encode Test: DXGI Capture -> AMF HEVC 10-bit HDR ===");

    // Initialize capture
    auto capture = deskbeam::IScreenCapture::create();
    auto* dxgi = dynamic_cast<deskbeam::DxgiCapture*>(capture.get());
    assert(dxgi);

    bool ok = capture->init(0);
    if (!ok) {
        deskbeam::log::error("TEST", "Failed to init capture");
        return 1;
    }
    auto res = capture->get_resolution();
    deskbeam::log::info("TEST", "Capture: %ux%u, format=%s", res.width, res.height,
                        dxgi->get_capture_format() == 10 ? "FP16 HDR" : "BGRA SDR");

    // Initialize encoder
    auto encoder = deskbeam::IVideoEncoder::create();
    deskbeam::EncoderConfig cfg;
    cfg.width = res.width;
    cfg.height = res.height;
    cfg.fps = 60;
    cfg.bitrate_bps = 15'000'000;
    cfg.idr_period = 60;
    cfg.input_format = dxgi->get_capture_format();

    ok = encoder->init(cfg, dxgi->get_device());
    if (!ok) {
        deskbeam::log::error("TEST", "Failed to init encoder");
        return 1;
    }

    // Open output file for raw H.264 bitstream
    FILE* outfile = fopen("encode_test.h265", "wb");
    if (!outfile) {
        deskbeam::log::error("TEST", "Failed to open output file");
        return 1;
    }

    // Force mouse movement to ensure frames
    INPUT mi = {};
    mi.type = INPUT_MOUSE;
    mi.mi.dwFlags = MOUSEEVENTF_MOVE;
    mi.mi.dx = 1;
    SendInput(1, &mi, sizeof(INPUT));

    // Capture and encode 120 frames
    const int target_frames = 120;
    int encoded_frames = 0;
    uint64_t total_bytes = 0;
    int keyframes = 0;
    double total_encode_ms = 0.0;
    auto start = deskbeam::Clock::now();

    for (int attempt = 0; attempt < target_frames * 3 && encoded_frames < target_frames; ++attempt) {
        deskbeam::CapturedFrame frame;
        if (!capture->capture_frame(frame, 50)) continue;

        auto t0 = deskbeam::Clock::now();
        uint64_t pts = std::chrono::duration_cast<std::chrono::microseconds>(
            frame.capture_time.time_since_epoch()).count();

        ok = encoder->encode(frame.texture, pts);
        capture->release_frame(frame);

        if (!ok) {
            deskbeam::log::warn("TEST", "Encode failed for frame %d", encoded_frames);
            continue;
        }

        auto t1 = deskbeam::Clock::now();
        double enc_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Collect output packets
        deskbeam::EncodedPacket pkt;
        while (encoder->get_packet(pkt)) {
            fwrite(pkt.data.data(), 1, pkt.data.size(), outfile);
            total_bytes += pkt.data.size();
            if (pkt.keyframe) keyframes++;

            deskbeam::log::debug("TEST", "Frame %d: %zu bytes, %s, encode=%.1fms",
                                 encoded_frames, pkt.data.size(),
                                 pkt.keyframe ? "IDR" : "P",
                                 enc_ms);
            encoded_frames++;
        }

        total_encode_ms += enc_ms;

        // Wiggle mouse to keep getting frames
        if (attempt % 10 == 0) {
            mi.mi.dx = (attempt % 20 == 0) ? 1 : -1;
            SendInput(1, &mi, sizeof(INPUT));
        }
    }

    fclose(outfile);

    auto elapsed = std::chrono::duration<double>(deskbeam::Clock::now() - start).count();

    deskbeam::log::info("TEST", "=== Results ===");
    deskbeam::log::info("TEST", "Encoded frames: %d", encoded_frames);
    deskbeam::log::info("TEST", "Keyframes: %d", keyframes);
    deskbeam::log::info("TEST", "Total size: %llu KB", (unsigned long long)(total_bytes / 1024));
    deskbeam::log::info("TEST", "Average bitrate: %.1f Mbps",
                        (total_bytes * 8.0) / elapsed / 1'000'000);
    deskbeam::log::info("TEST", "Average encode time: %.2f ms",
                        total_encode_ms / std::max(encoded_frames, 1));
    deskbeam::log::info("TEST", "FPS: %.1f", encoded_frames / elapsed);
    deskbeam::log::info("TEST", "Total time: %.2f s", elapsed);
    deskbeam::log::info("TEST", "Output: encode_test.h265");
    deskbeam::log::info("TEST", "  Play with: ffplay encode_test.h265");

    if (encoded_frames > 0) {
        deskbeam::log::info("TEST", "=== PASS ===");
    } else {
        deskbeam::log::error("TEST", "=== FAIL: no frames encoded ===");
        return 1;
    }

    return 0;
}

#else
#include <cstdio>
int main() {
    std::printf("Encode test is Windows-only\n");
    return 0;
}
#endif
