#ifdef DESKBEAM_LINUX

#include "host/encode/vaapi_encoder.h"
#include "common/utils/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <cstring>

namespace deskbeam::host {

namespace {
constexpr const char* TAG = "VAENC";
} // namespace

VaapiEncoder::VaapiEncoder() = default;

VaapiEncoder::~VaapiEncoder() {
    shutdown();
}

void VaapiEncoder::shutdown() {
    if (sws_bgrx_to_nv12_) { sws_freeContext(sws_bgrx_to_nv12_); sws_bgrx_to_nv12_ = nullptr; }
    if (pkt_)              { av_packet_free(&pkt_); }
    if (hw_frame_)         { av_frame_free(&hw_frame_); }
    if (sw_frame_)         { av_frame_free(&sw_frame_); }
    if (ctx_)              { avcodec_free_context(&ctx_); }
    if (hw_frames_ctx_)    { av_buffer_unref(&hw_frames_ctx_); }
    if (hw_device_ctx_)    { av_buffer_unref(&hw_device_ctx_); }
}

bool VaapiEncoder::init(const Config& cfg) {
    cfg_ = cfg;
    if (cfg_.width <= 0 || cfg_.height <= 0) {
        log::error(TAG, "init: bad dims %dx%d", cfg_.width, cfg_.height);
        return false;
    }
    if (cfg_.bitrate_bps <= 0) {
        // Sensible default if caller didn't set: ~bpp 0.1 at 60fps.
        cfg_.bitrate_bps = static_cast<int>(
            static_cast<int64_t>(cfg_.width) * cfg_.height * 60 / 10);
    }

    // 1. Open the VAAPI device.
    int rc = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VAAPI,
                                    cfg_.drm_node, nullptr, 0);
    if (rc < 0) {
        log::error(TAG, "av_hwdevice_ctx_create(%s) failed: %d", cfg_.drm_node, rc);
        return false;
    }
    log::info(TAG, "VAAPI device opened: %s (codec=%s, %dx%d @ %dfps)",
              cfg_.drm_node,
              cfg_.codec == VideoCodec::HEVC ? "hevc" : "h264",
              cfg_.width, cfg_.height, cfg_.fps);

    // 2. Build a hwframes pool — NV12 surfaces sized for the encoder.
    hw_frames_ctx_ = av_hwframe_ctx_alloc(hw_device_ctx_);
    if (!hw_frames_ctx_) { log::error(TAG, "hwframe_ctx_alloc failed"); return false; }
    auto* frames_ctx = reinterpret_cast<AVHWFramesContext*>(hw_frames_ctx_->data);
    frames_ctx->format    = AV_PIX_FMT_VAAPI;       // hw surface
    frames_ctx->sw_format = AV_PIX_FMT_NV12;        // backing pixel layout
    frames_ctx->width     = cfg_.width;
    frames_ctx->height    = cfg_.height;
    frames_ctx->initial_pool_size = 4;              // small ring is fine
    rc = av_hwframe_ctx_init(hw_frames_ctx_);
    if (rc < 0) { log::error(TAG, "hwframe_ctx_init failed: %d", rc); return false; }

    // 3. Pick the codec.
    const AVCodec* codec = nullptr;
    if (cfg_.codec == VideoCodec::HEVC) {
        codec = avcodec_find_encoder_by_name("hevc_vaapi");
    } else {
        codec = avcodec_find_encoder_by_name("h264_vaapi");
    }
    if (!codec) {
        log::error(TAG, "VAAPI %s encoder not found in this libavcodec build",
                   cfg_.codec == VideoCodec::HEVC ? "hevc" : "h264");
        return false;
    }

    // 4. Build the encoder context.
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) { log::error(TAG, "alloc_context3 failed"); return false; }
    ctx_->width        = cfg_.width;
    ctx_->height       = cfg_.height;
    ctx_->pix_fmt      = AV_PIX_FMT_VAAPI;
    ctx_->time_base    = AVRational{1, 1'000'000};   // microseconds
    ctx_->framerate    = AVRational{cfg_.fps, 1};
    ctx_->bit_rate     = cfg_.bitrate_bps;
    ctx_->rc_max_rate  = cfg_.bitrate_bps;
    ctx_->rc_min_rate  = cfg_.bitrate_bps;
    ctx_->rc_buffer_size = cfg_.bitrate_bps;          // 1s VBV — same shape as NVENC/QSV/AMF
    ctx_->gop_size     = cfg_.fps * 30;               // ~30s GOP, on-demand IDR via request_idr()
    ctx_->max_b_frames = 0;                           // no B-frames — kills latency
    ctx_->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_);
    if (!ctx_->hw_frames_ctx) { log::error(TAG, "buffer_ref failed"); return false; }

    // VAAPI driver-specific tuning.  These keys are recognised by both
    // hevc_vaapi and h264_vaapi.
    av_opt_set(ctx_->priv_data, "rc_mode", "CBR", 0);

    // low_power=1 is REQUIRED on Intel Gen11+ iGPUs (UHD Xe, Iris Xe,
    // Arc) for H.264 — the full-quality VAEntrypointEncSlice was
    // dropped, leaving only VAEntrypointEncSliceLP.  Without this
    // flag ffmpeg silently falls back to software encode and we get
    // 100-300ms per frame instead of 5-10.  Verify with
    //   vainfo --device /dev/dri/renderD128 | grep H264
    // — if you only see ":  VAEntrypointEncSliceLP" then LP is mandatory.
    //
    // HEVC LP on Intel iHD used to crash on some resolutions but appears
    // stable on driver >= 22.3 (vainfo shows VAProfileHEVCMain EncSliceLP).
    // If we see regressions we'll need a driver-version gate.
    av_opt_set_int(ctx_->priv_data, "low_power", 1, 0);

    rc = avcodec_open2(ctx_, codec, nullptr);
    if (rc < 0) {
        char err[128]{}; av_strerror(rc, err, sizeof(err));
        log::error(TAG, "avcodec_open2 failed: %s", err);
        return false;
    }

    // 5. Allocate persistent sw / hw / pkt frames.
    sw_frame_ = av_frame_alloc();
    sw_frame_->format = AV_PIX_FMT_NV12;
    sw_frame_->width  = cfg_.width;
    sw_frame_->height = cfg_.height;
    if (av_frame_get_buffer(sw_frame_, 32) < 0) {
        log::error(TAG, "sw_frame buffer alloc failed");
        return false;
    }

    hw_frame_ = av_frame_alloc();
    if (av_hwframe_get_buffer(hw_frames_ctx_, hw_frame_, 0) < 0) {
        log::error(TAG, "hwframe_get_buffer failed");
        return false;
    }

    pkt_ = av_packet_alloc();

    log::info(TAG, "VAAPI %s encoder ready: %dx%d @ %d fps, %d kbps, node=%s",
              cfg_.codec == VideoCodec::HEVC ? "HEVC" : "H.264",
              cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrate_bps / 1000,
              cfg_.drm_node);
    return true;
}

bool VaapiEncoder::encode_nv12(const uint8_t* y_data, int y_stride,
                               const uint8_t* uv_data, int uv_stride,
                               uint64_t pts_us)
{
    if (!ctx_) return false;

    // Copy Y plane row-by-row (source stride may differ from sw_frame's
    // linesize, especially when capture stride == width*1).
    const int W = cfg_.width;
    const int H = cfg_.height;
    for (int row = 0; row < H; ++row) {
        std::memcpy(sw_frame_->data[0] + row * sw_frame_->linesize[0],
                    y_data + row * y_stride, W);
    }
    for (int row = 0; row < H / 2; ++row) {
        std::memcpy(sw_frame_->data[1] + row * sw_frame_->linesize[1],
                    uv_data + row * uv_stride, W);
    }

    // Upload sw → hw (vaCopyImage under the hood).
    int rc = av_hwframe_transfer_data(hw_frame_, sw_frame_, 0);
    if (rc < 0) { log::error(TAG, "hwframe_transfer_data failed: %d", rc); return false; }

    // VAAPI's vaapi_encode core asserts display_order == encode_order
    // which only holds when the user-supplied PTS strictly increases
    // monotonically.  Wall-clock-derived microsecond PTS sometimes goes
    // backwards by tiny amounts under heartbeat re-encode timing — use
    // a simple frame counter at time_base 1/1_000_000 as the PTS the
    // encoder sees, and remember the real pts to put on the output pkt.
    (void)pts_us;
    hw_frame_->pts = ++frame_idx_;
    if (idr_requested_) {
        hw_frame_->pict_type = AV_PICTURE_TYPE_I;
        idr_requested_ = false;
    } else {
        hw_frame_->pict_type = AV_PICTURE_TYPE_NONE;
    }

    rc = avcodec_send_frame(ctx_, hw_frame_);
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        char err[128]{}; av_strerror(rc, err, sizeof(err));
        log::warn(TAG, "send_frame: %s", err);
        return false;
    }
    sw_frame_has_data_ = true;
    return true;
}

bool VaapiEncoder::encode_bgrx(const uint8_t* bgrx_data, int stride, uint64_t pts_us) {
    if (!sws_bgrx_to_nv12_) {
        // SWS_POINT (nearest neighbour) is the cheapest scaler and is
        // visually indistinguishable for screen content vs SWS_BILINEAR
        // on a no-resize colour-format conversion. SWS_BILINEAR cost
        // BGRx→NV12 1920x1200 ~25ms on iGPU which capped wire to ~40fps.
        sws_bgrx_to_nv12_ = sws_getContext(
            cfg_.width, cfg_.height, AV_PIX_FMT_BGR0,
            cfg_.width, cfg_.height, AV_PIX_FMT_NV12,
            SWS_POINT, nullptr, nullptr, nullptr);
        if (!sws_bgrx_to_nv12_) {
            log::error(TAG, "sws_getContext BGRx→NV12 failed");
            return false;
        }
    }
    const uint8_t* src_planes[4]   = { bgrx_data, nullptr, nullptr, nullptr };
    int            src_strides[4]  = { stride,    0,       0,       0       };
    sws_scale(sws_bgrx_to_nv12_, src_planes, src_strides,
              0, cfg_.height,
              sw_frame_->data, sw_frame_->linesize);

    int rc = av_hwframe_transfer_data(hw_frame_, sw_frame_, 0);
    if (rc < 0) { log::error(TAG, "hwframe_transfer_data failed: %d", rc); return false; }

    (void)pts_us;
    hw_frame_->pts = ++frame_idx_;
    if (idr_requested_) {
        hw_frame_->pict_type = AV_PICTURE_TYPE_I;
        idr_requested_ = false;
    } else {
        hw_frame_->pict_type = AV_PICTURE_TYPE_NONE;
    }
    rc = avcodec_send_frame(ctx_, hw_frame_);
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        char err[128]{}; av_strerror(rc, err, sizeof(err));
        log::warn(TAG, "send_frame (bgrx): %s", err);
        return false;
    }
    sw_frame_has_data_ = true;
    return true;
}

bool VaapiEncoder::reencode_last(uint64_t pts_us) {
    if (!ctx_ || !sw_frame_has_data_) return false;
    // sw_frame_ still holds the last NV12 from encode_nv12 / encode_bgrx —
    // re-upload it as a fresh hw frame and feed the encoder again. ~1.5
    // bpp upload (no sws_scale, no per-row memcpy beyond the upload),
    // cheap enough to fire every min_frame_interval on a static screen.
    int rc = av_hwframe_transfer_data(hw_frame_, sw_frame_, 0);
    if (rc < 0) { log::warn(TAG, "hwframe_transfer_data (heartbeat) failed: %d", rc); return false; }

    (void)pts_us;
    hw_frame_->pts = ++frame_idx_;
    if (idr_requested_) {
        hw_frame_->pict_type = AV_PICTURE_TYPE_I;
        idr_requested_ = false;
    } else {
        hw_frame_->pict_type = AV_PICTURE_TYPE_NONE;
    }
    rc = avcodec_send_frame(ctx_, hw_frame_);
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        char err[128]{}; av_strerror(rc, err, sizeof(err));
        log::warn(TAG, "send_frame (heartbeat): %s", err);
        return false;
    }
    return true;
}

bool VaapiEncoder::get_packet(Packet& out) {
    if (!ctx_ || !pkt_) return false;
    int rc = avcodec_receive_packet(ctx_, pkt_);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return false;
    if (rc < 0) {
        char err[128]{}; av_strerror(rc, err, sizeof(err));
        log::warn(TAG, "receive_packet: %s", err);
        return false;
    }
    out.data.assign(pkt_->data, pkt_->data + pkt_->size);
    out.pts_us   = static_cast<uint64_t>(pkt_->pts);
    out.keyframe = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
    av_packet_unref(pkt_);
    return true;
}

void VaapiEncoder::set_bitrate(int bps) {
    if (!ctx_) return;
    ctx_->bit_rate    = bps;
    ctx_->rc_max_rate = bps;
    ctx_->rc_min_rate = bps;
    cfg_.bitrate_bps  = bps;
    // Note: many VAAPI encoders ignore mid-stream bit_rate changes; you
    // may need a full reinit to honour aggressive bitrate ramps.
}

} // namespace deskbeam::host

#endif // DESKBEAM_LINUX
