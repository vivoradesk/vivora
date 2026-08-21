// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#ifdef VIVORA_LINUX

#include "common/codec/video_codec.h"
#include "host/encode/video_encoder.h"  // EncoderKind

#include <cstdint>
#include <memory>
#include <vector>

namespace vivora::host {

// Common interface for the Linux hardware encoders (VAAPI, NVENC).  The host
// platform holds one of these and feeds it CPU BGRx frames from the PipeWire
// SHM path; the concrete backend is chosen at runtime by
// create_linux_encoder() based on the requested EncoderKind and what
// hardware / driver is actually present.
class ILinuxEncoder {
public:
    struct Config {
        int  width  = 0;
        int  height = 0;
        int  fps    = 60;
        int  bitrate_bps = 0;
        VideoCodec codec = VideoCodec::HEVC;
        // VAAPI render node — pick the one tied to the active GPU.  Ignored
        // by the NVENC backend (it selects the NVIDIA device via CUDA).
        const char* drm_node = "/dev/dri/renderD128";
    };

    struct Packet {
        std::vector<uint8_t> data;
        uint64_t pts_us = 0;
        bool     keyframe = false;
    };

    virtual ~ILinuxEncoder() = default;

    virtual bool init(const Config& cfg) = 0;
    virtual void shutdown() = 0;

    // Push a CPU-side BGRx/BGRA frame; the backend converts to its native
    // input format internally.  This is the live PipeWire-SHM path.
    virtual bool encode_bgrx(const uint8_t* bgrx_data, int stride, uint64_t pts_us) = 0;

    // Re-encode the most recent frame with a fresh PTS — used by the
    // host_loop static-screen heartbeat to keep wire cadence steady.
    virtual bool reencode_last(uint64_t pts_us) = 0;

    virtual bool get_packet(Packet& out) = 0;
    virtual void request_idr() = 0;
    virtual void set_bitrate(int bps) = 0;

    virtual int width()  const = 0;
    virtual int height() const = 0;

    // Human-readable backend name for logs ("VAAPI", "NVENC").
    virtual const char* backend_name() const = 0;
};

// Pick and construct a Linux encoder.
//   kind == Nvenc → force NVENC; if unavailable, fall back to VAAPI.
//   kind == Auto  → probe NVENC first (discrete GPU, best latency); on any
//                   failure fall back to VAAPI.
//   any other     → VAAPI (the only other Linux backend).
// Returns nullptr only if the finally-selected backend fails to initialise.
std::unique_ptr<ILinuxEncoder> create_linux_encoder(EncoderKind kind,
                                                     const ILinuxEncoder::Config& cfg);

} // namespace vivora::host

#endif // VIVORA_LINUX
