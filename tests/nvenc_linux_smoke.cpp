// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// Standalone smoke test for the Linux NVENC encoder (VIV-8).  Not wired into
// ctest (needs an NVIDIA GPU + driver); compiled and run ad hoc on the box.
#include "host/encode/nvenc_linux_encoder.h"
#include <cstdio>
#include <vector>

using namespace vivora::host;

static bool run_codec(vivora::VideoCodec codec, const char* name) {
    std::printf("=== %s ===\n", name);
    NvencLinuxEncoder enc;
    ILinuxEncoder::Config cfg;
    cfg.width = 256; cfg.height = 256; cfg.fps = 60;
    cfg.bitrate_bps = 2'000'000; cfg.codec = codec;
    if (!enc.init(cfg)) { std::printf("  init FAILED\n"); return false; }

    // Synthetic BGRx gradient frame.
    std::vector<uint8_t> frame(256 * 256 * 4);
    int ok_packets = 0, key = 0;
    for (int f = 0; f < 5; ++f) {
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x) {
                uint8_t* p = &frame[(y * 256 + x) * 4];
                p[0] = (uint8_t)(x + f * 10);  // B
                p[1] = (uint8_t)(y + f * 10);  // G
                p[2] = (uint8_t)(x ^ y);       // R
                p[3] = 255;                    // X
            }
        if (!enc.encode_bgrx(frame.data(), 256 * 4, f * 16666)) {
            std::printf("  encode_bgrx FAILED on frame %d\n", f);
            return false;
        }
        ILinuxEncoder::Packet pkt;
        while (enc.get_packet(pkt)) {
            ++ok_packets;
            if (pkt.keyframe) ++key;
            std::printf("  frame %d -> %zu bytes %s\n", f, pkt.data.size(),
                        pkt.keyframe ? "[IDR]" : "");
        }
    }
    // Heartbeat re-encode of the last frame.
    enc.reencode_last(5 * 16666);
    ILinuxEncoder::Packet pkt;
    while (enc.get_packet(pkt)) {
        ++ok_packets;
        std::printf("  heartbeat -> %zu bytes\n", pkt.data.size());
    }
    std::printf("  %s: %d packets, %d keyframes\n", name, ok_packets, key);
    return ok_packets >= 5 && key >= 1;
}

int main() {
    std::printf("is_available: %s\n",
                NvencLinuxEncoder::is_available() ? "YES" : "NO");
    bool ok = true;
    ok &= run_codec(vivora::VideoCodec::HEVC, "HEVC");
    ok &= run_codec(vivora::VideoCodec::H264, "H264");
    std::printf("\n%s\n", ok ? "=== SMOKE PASS ===" : "=== SMOKE FAIL ===");
    return ok ? 0 : 1;
}
