#pragma once

#ifdef VIVORA_MACOS

#include "common/utils/types.h"
#include "common/codec/video_codec.h"
#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <vector>

namespace vivora::host {

struct MacEncoderConfig {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    uint32_t bitrate_bps = 15'000'000;
    uint32_t idr_period = 120;   // max keyframe interval, frames
    bool hdr = false;            // HEVC Main10 + BT.2020/PQ (HEVC only; H.264 is always SDR 8-bit)
    // Negotiated wire codec. HEVC is the default; H.264 is the fallback for
    // viewers whose decoder can't init HEVC (VIV-7). H.264 is SDR 8-bit only,
    // so `hdr` is ignored when codec == H264.
    vivora::VideoCodec codec = vivora::VideoCodec::HEVC;
};

struct MacEncodedPacket {
    std::vector<uint8_t> data;   // Annex-B; on keyframes VPS/SPS/PPS (HEVC) or SPS/PPS (H.264) are prepended
    uint64_t pts = 0;            // microseconds
    bool keyframe = false;
};

// Hardware video encoder via VideoToolbox (HEVC default, H.264 fallback).
class MacVideoToolboxEncoder {
public:
    MacVideoToolboxEncoder();
    ~MacVideoToolboxEncoder();

    MacVideoToolboxEncoder(const MacVideoToolboxEncoder&) = delete;
    MacVideoToolboxEncoder& operator=(const MacVideoToolboxEncoder&) = delete;

    bool init(const MacEncoderConfig& config);
    void shutdown();

    // Submit a CVPixelBuffer for encoding. Takes ownership of the reference
    // (releases after the encoder is done with it). Returns false on error.
    bool encode(CVPixelBufferRef frame, uint64_t pts_us);

    // Pull the next encoded packet from the output queue, if any.
    bool get_packet(MacEncodedPacket& out);

    // Request a keyframe on the next encode() call.
    void request_idr() { idr_pending_ = true; }

    void set_bitrate(uint32_t bitrate_bps);

    const MacEncoderConfig& config() const { return config_; }

    // Opaque Pimpl — public so that the VT output callback (a C function
    // in the .mm file) can reinterpret the refcon parameter.
    struct Impl;

private:
    Impl* impl_ = nullptr;
    MacEncoderConfig config_{};
    bool idr_pending_ = false;
};

} // namespace vivora::host

#endif // VIVORA_MACOS
