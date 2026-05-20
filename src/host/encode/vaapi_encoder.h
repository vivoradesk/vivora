#pragma once

#ifdef VIVORA_LINUX

#include "common/codec/video_codec.h"
#include <cstdint>
#include <memory>
#include <vector>

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace vivora::host {

// VAAPI hardware H.264/HEVC encoder driven through libavcodec's
// h264_vaapi / hevc_vaapi wrappers.  The encoder accepts NV12 frames in
// host (CPU) memory for now — they are uploaded to a VAAPI surface on
// each encode().  Stage 4 will replace the upload with DMA-BUF import
// for true zero-copy from the PipeWire capture side.
//
// Pipeline (current):
//   sw NV12 → AVHWFramesContext.av_hwframe_transfer_data → vaapi surface
//          → avcodec_send_frame → encoded NAL units via avcodec_receive_packet.
class VaapiEncoder {
public:
    struct Config {
        int  width  = 0;
        int  height = 0;
        int  fps    = 60;
        int  bitrate_bps = 0;
        VideoCodec codec = VideoCodec::HEVC;
        // VAAPI render node — pick the one tied to the active GPU.
        // Intel iGPU is normally renderD128, NVIDIA via VAAPI driver
        // (rare) would be renderD129.  Falls back to first available.
        const char* drm_node = "/dev/dri/renderD128";
    };

    struct Packet {
        std::vector<uint8_t> data;
        uint64_t pts_us = 0;
        bool     keyframe = false;
    };

    VaapiEncoder();
    ~VaapiEncoder();

    VaapiEncoder(const VaapiEncoder&) = delete;
    VaapiEncoder& operator=(const VaapiEncoder&) = delete;

    bool init(const Config& cfg);
    void shutdown();

    // Push a CPU-side NV12 frame (Y plane + interleaved UV).  Strides may
    // be larger than width / (width) respectively; rows are read at the
    // given pitch.  pts_us is the display-time timestamp.
    bool encode_nv12(const uint8_t* y_data, int y_stride,
                     const uint8_t* uv_data, int uv_stride,
                     uint64_t pts_us);

    // Push a CPU-side BGRx/BGRA frame; the encoder performs the
    // BGR→NV12 colour conversion internally via swscale before upload.
    // Convenience for the PipeWire SHM path which delivers BGRx.
    bool encode_bgrx(const uint8_t* bgrx_data, int stride, uint64_t pts_us);

    // Re-encode the last frame previously fed via encode_nv12/encode_bgrx.
    // The cached NV12 staging frame is re-uploaded to the VAAPI surface
    // and sent through the encoder again with a fresh PTS — used by the
    // host_loop static-screen heartbeat to keep the wire cadence steady
    // when capture is silent.  Cheap: ~1.5 bpp upload, no sws_scale, no
    // BGRx snapshot.  Returns false until the first real frame arrives.
    bool reencode_last(uint64_t pts_us);

    // Pull next encoded packet (one NAL unit access-unit at a time).
    // Returns false when the encoder has no output ready right now.
    bool get_packet(Packet& out);

    // Force an IDR on the next encoded frame.
    void request_idr() { idr_requested_ = true; }

    void set_bitrate(int bps);

    int width()  const { return cfg_.width; }
    int height() const { return cfg_.height; }

private:
    Config         cfg_{};
    AVBufferRef*   hw_device_ctx_ = nullptr;
    AVBufferRef*   hw_frames_ctx_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVFrame*       sw_frame_ = nullptr;   // NV12 staging
    AVFrame*       hw_frame_ = nullptr;   // VAAPI surface bound to ctx_
    AVPacket*      pkt_ = nullptr;
    SwsContext*    sws_bgrx_to_nv12_ = nullptr;
    bool           idr_requested_ = false;
    int64_t        frame_idx_ = 0;
    // True once a real frame has been written into sw_frame_ — guards
    // reencode_last() from sending an uninitialised surface.
    bool           sw_frame_has_data_ = false;
};

} // namespace vivora::host

#endif // VIVORA_LINUX
