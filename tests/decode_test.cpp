// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef VIVORA_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include "client/decode/video_decoder.h"
#include "common/utils/log.h"
#include <cstdio>
#include <vector>

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    vivora::log::info("TEST", "=== Decode Test ===");

    auto decoder = vivora::IVideoDecoder::create();
    if (!decoder->init(vivora::VideoCodec::HEVC)) {
        vivora::log::error("TEST", "Failed to init decoder");
        return 1;
    }

    // Read a .h265 file from encode_test or transport_test
    FILE* f = fopen("transport_test.h265", "rb");
    if (!f) f = fopen("encode_test.h265", "rb");
    if (!f) {
        vivora::log::error("TEST", "No .h265 test file found");
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> data(fsize);
    fread(data.data(), 1, fsize, f);
    fclose(f);

    vivora::log::info("TEST", "Read %ld bytes from .h265 file", fsize);

    // Feed the whole file as one big chunk (MF should handle NAL parsing)
    vivora::log::info("TEST", "Feeding to decoder...");
    bool ok = decoder->decode(data.data(), data.size(), 0);
    vivora::log::info("TEST", "decode() returned %s", ok ? "true" : "false");

    // Try to get output
    vivora::DecodedFrame frame;
    int decoded = 0;
    while (decoder->get_frame(frame)) {
        decoded++;
        vivora::log::info("TEST", "Decoded frame %d: %ux%u", decoded, frame.width, frame.height);
    }

    vivora::log::info("TEST", "Total decoded frames: %d", decoded);
    vivora::log::info("TEST", "=== %s ===", decoded > 0 ? "PASS" : "NEEDS MORE DATA");

    return 0;
}

#else
#include <cstdio>
int main() { printf("Windows only\n"); return 0; }
#endif
