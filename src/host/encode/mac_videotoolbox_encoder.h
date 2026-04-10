#pragma once

#ifdef DESKBEAM_MACOS

#include "common/utils/types.h"
#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <vector>

namespace deskbeam::host {

struct MacEncoderConfig {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    uint32_t bitrate_bps = 15'000'000;
    uint32_t idr_period = 120;   // max keyframe interval, frames
    bool hdr = false;            // HEVC Main10 + BT.2020/PQ
};

struct MacEncodedPacket {
    std::vector<uint8_t> data;   // HEVC Annex-B (VPS/SPS/PPS prepended on keyframes)
    uint64_t pts = 0;            // microseconds
    bool keyframe = false;
};

// HEVC hardware encoder via VideoToolbox.
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

} // namespace deskbeam::host

#endif // DESKBEAM_MACOS
