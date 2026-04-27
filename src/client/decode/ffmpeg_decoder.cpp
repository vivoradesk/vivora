#include "client/decode/ffmpeg_decoder.h"
#include "common/utils/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

#include <cstring>

namespace deskbeam::client {

FfmpegDecoder::~FfmpegDecoder() {
    if (sws_)       sws_freeContext(sws_);
    if (out_frame_) av_frame_free(&out_frame_);
    if (in_frame_)  av_frame_free(&in_frame_);
    if (pkt_)       av_packet_free(&pkt_);
    if (ctx_)       avcodec_free_context(&ctx_);
}

bool FfmpegDecoder::init(VideoCodec codec) {
    codec_ = codec;
    AVCodecID codec_id = AV_CODEC_ID_NONE;
    switch (codec) {
        case VideoCodec::H264: codec_id = AV_CODEC_ID_H264; break;
        case VideoCodec::HEVC: codec_id = AV_CODEC_ID_HEVC; break;
    }
    const AVCodec* dec = avcodec_find_decoder(codec_id);
    if (!dec) {
        log::error("FFDec", "No decoder for codec id %d", static_cast<int>(codec_id));
        return false;
    }

    ctx_ = avcodec_alloc_context3(dec);
    if (!ctx_) return false;

    // Slice-only threading.  FF_THREAD_FRAME buffers N frames before
    // emitting, adding (thread_count - 1) frames of decode latency —
    // 4-8 frames at auto count, i.e. 67-133 ms at 60 fps, very visible
    // in interactive use.  Slice threading parallelises within a frame
    // when the encoder produces multi-slice output and is a no-op
    // otherwise, so no harm leaving it on.  Throughput suffers vs frame
    // threading; the right long-term answer is L4 VAAPI HW decode.
    ctx_->thread_count = 0;
    ctx_->thread_type  = FF_THREAD_SLICE;
    // No flags — earlier experiments with AV_CODEC_FLAG_LOW_DELAY shrank
    // the DPB to a single reference, which caused legitimate forward
    // refs to be evicted and produced "Could not find ref with POC X"
    // for frames that DeskBeam's encoder still wanted to use.  Modern
    // libav low-delay behavior is good enough by default for streams
    // without B-frames.

    if (avcodec_open2(ctx_, dec, nullptr) < 0) {
        log::error("FFDec", "avcodec_open2 failed");
        avcodec_free_context(&ctx_);
        return false;
    }

    pkt_       = av_packet_alloc();
    in_frame_  = av_frame_alloc();
    out_frame_ = av_frame_alloc();
    if (!pkt_ || !in_frame_ || !out_frame_) return false;

    log::info("FFDec", "Opened SW %s decoder",
              codec == VideoCodec::HEVC ? "HEVC" : "H.264");
    return true;
}

bool FfmpegDecoder::decode(const uint8_t* data, size_t len, uint64_t pts,
                           bool keyframe) {
    if (!ctx_ || !pkt_ || !data || len == 0) return false;

    // Once the decoder has reported a corrupt frame (missing ref, parse
    // error, EAGAIN), refuse all further input until the caller resets
    // us via reinit() at the next keyframe.  This is the project-wide
    // rule: any visible artifact must be replaced with a clean drop +
    // IDR cycle, never a half-decoded frame.
    if (corrupt_) return false;

    // One-shot diagnostic: walk Annex-B start codes and print every NAL
    // unit type in the first keyframe and first two P-frames.  HEVC NAL
    // types: 32=VPS, 33=SPS, 34=PPS, 35=AUD, 19/20=IDR, 21=CRA, 0..9=slice
    // categories.  This is the surest way to tell IDR vs CRA at random
    // access and to spot the case where macOS prepends RASL/RADL frames
    // to a CRA, since those force libav to look for pre-CRA refs.
    static int dumped_kf = 0;
    static int dumped_pf = 0;
    auto dump_nals = [data, len](const char* tag) {
        char buf[256] = {0};
        int  off = 0;
        size_t i = 0;
        while (i + 4 < len && off < 230) {
            if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
                int nut = (data[i+4] >> 1) & 0x3F;
                int n = std::snprintf(buf + off, sizeof(buf) - off, "%d ", nut);
                if (n > 0) off += n;
                i += 5;
            } else {
                ++i;
            }
        }
        log::info("FFDec", "%s len=%zu NALs: %s", tag, len, buf);
    };
    if (keyframe && dumped_kf < 1) {
        ++dumped_kf;
        dump_nals("KF");
    } else if (!keyframe && dumped_pf < 2 && dumped_kf > 0) {
        ++dumped_pf;
        dump_nals("PF");
    }

    pkt_->data  = const_cast<uint8_t*>(data);
    pkt_->size  = static_cast<int>(len);
    pkt_->pts   = static_cast<int64_t>(pts);
    pkt_->flags = keyframe ? AV_PKT_FLAG_KEY : 0;

    int rc = avcodec_send_packet(ctx_, pkt_);
    pkt_->data  = nullptr;
    pkt_->size  = 0;
    pkt_->flags = 0;
    if (rc < 0) {
        // ANY send error — including EAGAIN — taints the decoder.  EAGAIN
        // means libav's input queue is full (we're CPU-bound and the
        // decode thread can't keep up); silently dropping the packet
        // would leave subsequent P-frames referring to the dropped one
        // and produce visible glitches.  Mark corrupt so the upper layer
        // forces an IDR + reinit instead.
        corrupt_ = true;
        return false;
    }
    return true;
}

void FfmpegDecoder::flush() {
    if (ctx_) avcodec_flush_buffers(ctx_);
}

bool FfmpegDecoder::reinit() {
    corrupt_ = false;
    if (ctx_) avcodec_free_context(&ctx_);
    AVCodecID codec_id = (codec_ == VideoCodec::H264)
                           ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
    const AVCodec* dec = avcodec_find_decoder(codec_id);
    if (!dec) return false;
    ctx_ = avcodec_alloc_context3(dec);
    if (!ctx_) return false;
    ctx_->thread_count = 0;
    ctx_->thread_type  = FF_THREAD_SLICE;
    if (avcodec_open2(ctx_, dec, nullptr) < 0) {
        log::error("FFDec", "reinit: avcodec_open2 failed");
        avcodec_free_context(&ctx_);
        return false;
    }
    return true;
}

bool FfmpegDecoder::ensure_sws(int src_format, int width, int height) {
    if (sws_ && sws_src_format_ == src_format &&
        sws_width_ == width && sws_height_ == height) {
        return true;
    }
    if (sws_) {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    sws_ = sws_getContext(width, height, static_cast<AVPixelFormat>(src_format),
                          width, height, AV_PIX_FMT_YUV420P,
                          SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) {
        log::error("FFDec", "sws_getContext failed for fmt=%d %dx%d",
                   src_format, width, height);
        return false;
    }
    sws_src_format_ = src_format;
    sws_width_      = width;
    sws_height_     = height;

    // Resize the destination frame to match.
    av_frame_unref(out_frame_);
    out_frame_->format = AV_PIX_FMT_YUV420P;
    out_frame_->width  = width;
    out_frame_->height = height;
    if (av_frame_get_buffer(out_frame_, 32) < 0) {
        log::error("FFDec", "av_frame_get_buffer failed");
        return false;
    }
    return true;
}

bool FfmpegDecoder::get_frame(YuvFrame& out) {
    if (!ctx_ || !in_frame_ || corrupt_) return false;
    int rc = avcodec_receive_frame(ctx_, in_frame_);
    if (rc != 0) {
        // EAGAIN = no frame yet; EOF = stream ended; both expected.
        return false;
    }
    // Reject frames that libav decoded with missing references — those
    // visibly disintegrate as gray placeholder regions.  Force an IDR
    // cycle instead.
    if (in_frame_->flags & AV_FRAME_FLAG_CORRUPT) {
        av_frame_unref(in_frame_);
        corrupt_ = true;
        return false;
    }

    // Log colorspace metadata once per stream — tells us if host is
    // sending HDR (BT.2020 + PQ) which our BT.709 shader can't handle
    // correctly without tonemapping.
    static bool logged_color = false;
    if (!logged_color) {
        logged_color = true;
        log::info("FFDec",
                  "Decoded fmt=%d colorspace=%d range=%d primaries=%d trc=%d",
                  in_frame_->format,
                  in_frame_->colorspace,
                  in_frame_->color_range,
                  in_frame_->color_primaries,
                  in_frame_->color_trc);
        // HDR detection: BT.2020 primaries (9) or non-linear-luma (10), or
        // SMPTE 2084 PQ transfer (16), or HLG transfer (18).  Either flag
        // is enough to switch the renderer to the HDR shader path.
        is_hdr_ =
            (in_frame_->color_primaries == AVCOL_PRI_BT2020) ||
            (in_frame_->color_trc == AVCOL_TRC_SMPTE2084)    ||
            (in_frame_->color_trc == AVCOL_TRC_ARIB_STD_B67);
        if (is_hdr_) log::info("FFDec", "HDR stream detected — using BT.2020+PQ shader path");
    }

    AVFrame* picked = in_frame_;
    if (in_frame_->format != AV_PIX_FMT_YUV420P) {
        // 10-bit HDR HEVC, NV12, YUV422P, etc. — convert to plain YUV420P
        // 8-bit so the renderer's three-plane shader is the only shape it
        // ever has to handle.  L4 (VAAPI) will skip this entirely.
        if (!ensure_sws(in_frame_->format, in_frame_->width, in_frame_->height)) {
            av_frame_unref(in_frame_);
            return false;
        }
        sws_scale(sws_,
                  in_frame_->data, in_frame_->linesize,
                  0, in_frame_->height,
                  out_frame_->data, out_frame_->linesize);
        out_frame_->pts = in_frame_->pts;
        picked = out_frame_;
    }

    out.width  = static_cast<uint32_t>(picked->width);
    out.height = static_cast<uint32_t>(picked->height);
    out.stride[0] = picked->linesize[0];
    out.stride[1] = picked->linesize[1];
    out.stride[2] = picked->linesize[2];
    out.plane[0] = picked->data[0];
    out.plane[1] = picked->data[1];
    out.plane[2] = picked->data[2];
    out.pts = static_cast<uint64_t>(picked->pts);

    // Note: in_frame_ keeps the ref until next receive_frame; out_frame_
    // is preallocated.  Either way the planes stay valid until the next
    // `decode` / `get_frame` — caller MUST upload before then.
    return true;
}

} // namespace deskbeam::client
