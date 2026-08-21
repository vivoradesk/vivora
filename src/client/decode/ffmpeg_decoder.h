// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "common/codec/video_codec.h"

#include <cstddef>
#include <cstdint>

extern "C" {
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwsContext;
struct AVBufferRef;
}

namespace vivora::client {

// Software H.264/HEVC decoder via libavcodec.  Output is always YUV420P
// 8-bit — `get_frame` runs sws_scale internally if the stream comes in
// 10-bit (HDR HEVC) or NV12 so the renderer only ever sees one format.
//
// L4 will introduce a VAAPI HW path that bypasses the sws step and hands
// out DRM PRIME fds for zero-copy GL upload; the public API stays the
// same so the renderer doesn't care which path produced the frame.
class FfmpegDecoder {
public:
    FfmpegDecoder() = default;
    ~FfmpegDecoder();
    FfmpegDecoder(const FfmpegDecoder&) = delete;
    FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;

    bool init(VideoCodec codec);

    // Re-create the underlying AVCodecContext from scratch.  Used on
    // keyframe boundaries instead of `avcodec_flush_buffers` because the
    // FFmpeg 4.4 HEVC decoder shipped with Ubuntu 22.04 leaks POC state
    // across flushes — symptoms are persistent "Could not find ref with
    // POC X" warnings and visible flicker even on clean IDR streams.
    bool reinit();

    // Submit a NAL-unit payload.  Returns true on accept; false on a
    // codec-level reject (caller should request IDR).  pts is opaque —
    // surfaced back through `get_frame` so the host loop can correlate
    // decoded frames with their wire seq numbers.  `keyframe` toggles
    // AV_PKT_FLAG_KEY which lets libavcodec recognise IDR/CRA boundaries
    // and reset its DPB cleanly.
    bool decode(const uint8_t* data, size_t len, uint64_t pts,
                bool keyframe);

    // Drop all buffered reference pictures.  Call after detected loss /
    // before feeding a new IDR.
    void flush();

    struct YuvFrame {
        uint32_t       width  = 0;
        uint32_t       height = 0;
        // Strides (bytes per row) for Y/U/V planes.
        int            stride[3] = {0, 0, 0};
        // Non-owning pointers into the decoder's internal frame buffer.
        // Valid only until the next `decode` or `get_frame` call —
        // upload to a GL texture immediately and don't retain.
        const uint8_t* plane[3]  = {nullptr, nullptr, nullptr};
        uint64_t       pts = 0;
    };

    // Pull a decoded frame.  Returns false when none is ready this iter.
    bool get_frame(YuvFrame& out);

    // True if the most recently decoded stream was HDR (BT.2020 + PQ).
    // Set after the first frame is decoded and color metadata is read.
    // Renderer branches on this to apply the right colorspace + EOTF.
    bool is_hdr() const { return is_hdr_; }

    // Human-readable backend label for the HUD.  "VAAPI HEVC" / "SW HEVC".
    const char* backend_name() const;

private:
    bool ensure_sws(int src_format, int width, int height);

    AVCodecContext* ctx_       = nullptr;
    AVPacket*       pkt_       = nullptr;
    AVFrame*        in_frame_  = nullptr;  // raw decoder output (HW or SW)
    AVFrame*        sw_frame_  = nullptr;  // CPU copy when in_frame_ is on GPU
    AVFrame*        out_frame_ = nullptr;  // sws_scale destination (YUV420P)
    SwsContext*     sws_       = nullptr;
    // VAAPI device — non-null when HW decode succeeded at init.
    AVBufferRef*    hw_device_ctx_ = nullptr;
    bool            hw_decode_ = false;
    int             sws_src_format_ = -1;
    int             sws_width_      = 0;
    int             sws_height_     = 0;
    VideoCodec      codec_     = VideoCodec::HEVC;  // remembered for reinit()
    // Latches to true on the first decode error or corrupt-flagged frame.
    // While set, decode() / get_frame() refuse all input/output so the
    // upper layer can drop everything cleanly until the next IDR-driven
    // reinit().  Project rule: never display a partially-broken frame.
    bool            corrupt_   = false;
    bool            is_hdr_    = false;
    // Per-instance guard for the one-time colorspace/HDR detection in
    // get_frame().  MUST be a member, not a function-local static: the GUI
    // creates a fresh FfmpegDecoder per ViewSession within one process, and a
    // process-lifetime static made every session after the first skip HDR
    // detection → HDR streams rendered SDR (washed out) on reconnect (VIV-78).
    bool            color_logged_ = false;

    // HW→SW auto-fallback (VIV-80).  Hardware decoders fail on perfectly
    // valid frames when the driver is missing/flaky (common on Linux without
    // the right VAAPI driver; also seen on Windows).  After a streak of
    // consecutive HW failures we give up on hardware and reopen in software
    // on the next reinit().  sw_forced_ is the manual VIVORA_NO_HWDEC
    // override; either path routes through use_sw().
    bool            sw_forced_      = false;  // VIVORA_NO_HWDEC
    bool            hw_gave_up_     = false;  // auto-fallback latched
    int             hw_fail_streak_ = 0;
    static constexpr int kHwFailGiveUp = 12;  // ~0.2 s at 60 fps of solid failure
    // Time-windowed fallback: a flaky HW decoder (some Intel VAAPI HEVC) fails
    // intermittently — a few frames every couple of seconds — which never trips
    // the consecutive streak above yet churns IDRs and craters the bitrate.
    // kHwFailWindowGiveUp failures within kHwFailWindowMs also give up (VIV-82).
    int64_t         hw_win_start_ms_ = 0;
    int             hw_win_fails_    = 0;
    static constexpr int     kHwFailWindowGiveUp = 4;
    static constexpr int64_t kHwFailWindowMs     = 15000;
    bool use_sw() const { return sw_forced_ || hw_gave_up_; }
    void note_hw_failure();
};

} // namespace vivora::client
