// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <chrono>
#include "client/decode/ffmpeg_decoder.h"
#include "common/utils/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vivora::client {

namespace {
// VIV-82: libav's HEVC decoder conceals missing references ("Could not find ref
// with POC …") in GREY and does NOT reliably set decode_error_flags /
// AV_EF_EXPLODE, so those frames slipped past get_frame()'s reject checks and
// flashed grey under loss.  Intercept libav's own log stream: when it reports a
// missing-ref / concealment during a decode call, raise a flag the decode path
// checks so the frame is rejected (drop + IDR) instead of shown.  The callback
// runs synchronously on the decode thread inside send_packet/receive_frame.
std::atomic<bool> g_decode_error{false};

void ffmpeg_log_cb(void* avcl, int level, const char* fmt, va_list vl) {
    if (level <= AV_LOG_WARNING) {
        va_list vl2;
        va_copy(vl2, vl);
        char line[512];
        std::vsnprintf(line, sizeof(line), fmt, vl2);
        va_end(vl2);
        if (std::strstr(line, "Could not find ref") ||
            std::strstr(line, "concealing")         ||
            std::strstr(line, "Missing reference")) {
            g_decode_error.store(true, std::memory_order_relaxed);
        }
    }
    av_log_default_callback(avcl, level, fmt, vl);
}
} // namespace

FfmpegDecoder::~FfmpegDecoder() {
    if (sws_)            sws_freeContext(sws_);
    if (out_frame_)      av_frame_free(&out_frame_);
    if (sw_frame_)       av_frame_free(&sw_frame_);
    if (in_frame_)       av_frame_free(&in_frame_);
    if (pkt_)            av_packet_free(&pkt_);
    if (ctx_)            avcodec_free_context(&ctx_);
    if (hw_device_ctx_)  av_buffer_unref(&hw_device_ctx_);
}

// libavcodec calls this with the list of pixel formats it can output.
// We pin VAAPI when present so the decoder produces GPU surfaces; the
// fallback to AV_PIX_FMT_NONE forces libav back to its default selection
// if the HW path didn't initialise (defensive — usually doesn't trigger).
static AVPixelFormat get_hw_format_cb(AVCodecContext* /*ctx*/, const AVPixelFormat* fmts) {
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_VAAPI) return *p;
    }
    return AV_PIX_FMT_NONE;
}

bool FfmpegDecoder::init(VideoCodec codec) {
    // Install our log interceptor once per process (VIV-82 grey-frame catch).
    static bool log_cb_installed = false;
    if (!log_cb_installed) { av_log_set_callback(ffmpeg_log_cb); log_cb_installed = true; }

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

    // Try VAAPI HW decode first, unless VIVORA_NO_HWDEC forces software.
    // The env toggle is both a diagnostic (isolate flaky HW decode from
    // network loss) and a field fallback for GPUs whose VAAPI HEVC path is
    // unreliable at high resolution (VIV-79).  Otherwise falls through to SW
    // on any failure — missing GPU, no driver, missing kernel module, lack
    // of permission on /dev/dri/renderD*, etc.  Logged either way so it's
    // obvious which path is active in the field.
    sw_forced_ = std::getenv("VIVORA_NO_HWDEC") != nullptr;
    int hw_rc = use_sw() ? -1
              : av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VAAPI,
                                       nullptr, nullptr, 0);
    if (hw_rc == 0 && hw_device_ctx_) {
        ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
        ctx_->get_format    = get_hw_format_cb;
        hw_decode_          = true;
    } else {
        if (use_sw())
            log::info("FFDec", "VIVORA_NO_HWDEC set — forcing software decode");
        else
            log::warn("FFDec", "VAAPI device init failed (rc=%d) — using SW decode", hw_rc);
        if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
    }

    // Slice-only threading on the SW fallback.  Frame threading buffers
    // N frames before emit (visible 67-133 ms latency at 60 fps); slice
    // threading is zero-latency and parallelises within a frame when
    // the encoder produces multi-slice output.  HW decode ignores both.
    ctx_->thread_count = 0;
    ctx_->thread_type  = FF_THREAD_SLICE;
    // No-artifact (VIV-82): make the decoder ERROR on missing references /
    // broken bitstream ("Could not find ref with POC …") instead of silently
    // concealing the damage in grey.  Under packet loss the SW HEVC decoder
    // would otherwise emit grey-filled frames with decode_error_flags UNSET, so
    // get_frame()'s reject check missed them.  With EXPLODE they surface as a
    // decode error → get_frame drops → drop-to-keyframe + IDR (a brief freeze,
    // never grey).
    ctx_->err_recognition = AV_EF_EXPLODE;

    if (avcodec_open2(ctx_, dec, nullptr) < 0) {
        log::error("FFDec", "avcodec_open2 failed");
        avcodec_free_context(&ctx_);
        if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
        return false;
    }

    pkt_       = av_packet_alloc();
    in_frame_  = av_frame_alloc();
    sw_frame_  = av_frame_alloc();
    out_frame_ = av_frame_alloc();
    if (!pkt_ || !in_frame_ || !sw_frame_ || !out_frame_) return false;

    log::info("FFDec", "Opened %s %s decoder",
              hw_decode_ ? "VAAPI HW" : "SW",
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

    g_decode_error.store(false, std::memory_order_relaxed);
    int rc = avcodec_send_packet(ctx_, pkt_);
    pkt_->data  = nullptr;
    pkt_->size  = 0;
    pkt_->flags = 0;
    // libav logged a missing-ref/concealment while decoding this packet — the
    // output would be grey.  Reject it → the upper layer drops + IDRs (VIV-82).
    if (rc >= 0 && g_decode_error.load(std::memory_order_relaxed)) {
        // libav concealed a missing ref this decode → the output would be grey.
        corrupt_ = true;
        note_hw_failure();
        return false;
    }
    if (rc < 0) {
        // ANY send error — including EAGAIN — taints the decoder.  EAGAIN
        // means libav's input queue is full (we're CPU-bound and the
        // decode thread can't keep up); silently dropping the packet
        // would leave subsequent P-frames referring to the dropped one
        // and produce visible glitches.  Mark corrupt so the upper layer
        // forces an IDR + reinit instead.
        corrupt_ = true;
        note_hw_failure();
        return false;
    }
    return true;
}

void FfmpegDecoder::flush() {
    if (ctx_) avcodec_flush_buffers(ctx_);
}

const char* FfmpegDecoder::backend_name() const {
    if (hw_decode_) return codec_ == VideoCodec::HEVC ? "VAAPI HEVC" : "VAAPI H.264";
    return codec_ == VideoCodec::HEVC ? "SW HEVC" : "SW H.264";
}

bool FfmpegDecoder::reinit() {
    corrupt_ = false;
    // Drop any buffer refs we still hold against the old codec / its
    // hwframes context BEFORE freeing the context — otherwise the
    // refcount-zero free of the underlying VAAPI surfaces races with
    // their internal pool teardown and segfaults on the next
    // avcodec_receive_frame against the freshly-opened ctx.
    if (in_frame_) av_frame_unref(in_frame_);
    if (sw_frame_) av_frame_unref(sw_frame_);
    if (out_frame_) av_frame_unref(out_frame_);
    // sws operates on the old src format; rebuild lazily on first frame.
    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; sws_src_format_ = -1; }
    if (ctx_) avcodec_free_context(&ctx_);
    AVCodecID codec_id = (codec_ == VideoCodec::H264)
                           ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
    const AVCodec* dec = avcodec_find_decoder(codec_id);
    if (!dec) return false;
    ctx_ = avcodec_alloc_context3(dec);
    if (!ctx_) return false;
    // Re-attach HW only if we haven't given up on it (VIV-80).  After
    // fallback latches we reopen in software so the broken HW path is
    // bypassed for the rest of the session.
    if (hw_device_ctx_ && !use_sw()) {
        ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
        ctx_->get_format    = get_hw_format_cb;
        hw_decode_          = true;
    } else {
        hw_decode_          = false;
    }
    ctx_->thread_count = 0;
    ctx_->thread_type  = FF_THREAD_SLICE;
    ctx_->err_recognition = AV_EF_EXPLODE;  // reject broken frames, never grey (VIV-82)
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
    // Reject frames that libav couldn't decode cleanly.  Two distinct
    // signals, and the gap between them is the "corrupted/frozen picture for
    // the first few seconds" bug (VIV-77):
    //   * AV_FRAME_FLAG_CORRUPT — set per the codec's err_recognition policy.
    //   * decode_error_flags    — non-zero when the HW accelerator failed or
    //     references were missing ("Could not find ref with POC …",
    //     "hardware accelerator failed to decode picture").  VAAPI surfaces
    //     these here WITHOUT the corrupt flag, so they used to slip through
    //     and render as garbage/frozen output until the next IDR.
    // Either way the picture is broken — drop it and force an IDR cycle
    // (project no-artifact rule), rather than displaying a half-decoded frame.
    // NOTE: VIV-82 tried DISPLAYING decode_error_flags frames (to dodge VAAPI
    // false positives that were churning IDRs) — but under real loss on a flaky
    // VAAPI it tears badly AND it broke the HW→SW give-up streak.  So we stay
    // strict; a genuinely flaky HW path falls back to software via the streak.
    if ((in_frame_->flags & AV_FRAME_FLAG_CORRUPT) ||
        in_frame_->decode_error_flags != 0 ||
        g_decode_error.load(std::memory_order_relaxed)) {  // libav logged conceal
        av_frame_unref(in_frame_);
        corrupt_ = true;
        note_hw_failure();
        return false;
    }

    // VAAPI path: surface lives on the GPU.  Readback to a separate
    // sw_frame_ so the canonical receive target (in_frame_) keeps the
    // shape libavcodec expects and we don't fight its frame lifecycle.
    // After this branch `picked_frame` points either at in_frame_ (SW
    // path) or sw_frame_ (HW path).  Both are valid until the next
    // get_frame() call.  L4b will replace the readback with DMA-BUF +
    // EGLImage zero-copy and skip this branch entirely.
    AVFrame* picked_frame = in_frame_;
    if (in_frame_->format == AV_PIX_FMT_VAAPI) {
        av_frame_unref(sw_frame_);
        if (av_hwframe_transfer_data(sw_frame_, in_frame_, 0) < 0) {
            log::error("FFDec", "hwframe transfer failed");
            av_frame_unref(in_frame_);
            corrupt_ = true;
            note_hw_failure();
            return false;
        }
        sw_frame_->color_primaries = in_frame_->color_primaries;
        sw_frame_->color_trc       = in_frame_->color_trc;
        sw_frame_->colorspace      = in_frame_->colorspace;
        sw_frame_->color_range     = in_frame_->color_range;
        sw_frame_->pts             = in_frame_->pts;
        av_frame_unref(in_frame_);  // release GPU surface ref now
        picked_frame = sw_frame_;
    }

    // Log colorspace metadata once per stream — tells us if host is
    // sending HDR (BT.2020 + PQ) which our BT.709 shader can't handle
    // correctly without tonemapping.
    if (!color_logged_) {
        color_logged_ = true;
        // VAAPI commonly leaves AVFrame's color_* at UNSPECIFIED (2),
        // even when the bitstream carries the right VUI params — the
        // driver just doesn't propagate them.  Fall back to ctx_, which
        // libav parses out of the SPS at codec-open time.
        AVColorPrimaries pri = in_frame_->color_primaries;
        AVColorTransferCharacteristic trc = in_frame_->color_trc;
        AVColorSpace cs = in_frame_->colorspace;
        AVColorRange rng = in_frame_->color_range;
        if (pri == AVCOL_PRI_UNSPECIFIED) pri = ctx_->color_primaries;
        if (trc == AVCOL_TRC_UNSPECIFIED) trc = ctx_->color_trc;
        if (cs  == AVCOL_SPC_UNSPECIFIED) cs  = ctx_->colorspace;
        if (rng == AVCOL_RANGE_UNSPECIFIED) rng = ctx_->color_range;
        log::info("FFDec",
                  "Decoded fmt=%d colorspace=%d range=%d primaries=%d trc=%d "
                  "(ctx primaries=%d trc=%d)",
                  in_frame_->format, cs, rng, pri, trc,
                  ctx_->color_primaries, ctx_->color_trc);
        is_hdr_ =
            (pri == AVCOL_PRI_BT2020)        ||
            (trc == AVCOL_TRC_SMPTE2084)     ||
            (trc == AVCOL_TRC_ARIB_STD_B67);
        if (is_hdr_) log::info("FFDec", "HDR stream detected — using BT.2020+PQ shader path");
    }

    // picked_frame from the HW branch above (or in_frame_ on SW path).
    if (picked_frame->format != AV_PIX_FMT_YUV420P) {
        // 10-bit HDR HEVC (sw), NV12 (vaapi readback), YUV422P, etc. —
        // convert to plain YUV420P 8-bit so the renderer only has to
        // handle one shape.  L4b will skip this for HW-decoded content.
        if (!ensure_sws(picked_frame->format,
                        picked_frame->width, picked_frame->height)) {
            return false;
        }
        sws_scale(sws_,
                  picked_frame->data, picked_frame->linesize,
                  0, picked_frame->height,
                  out_frame_->data, out_frame_->linesize);
        out_frame_->pts = picked_frame->pts;
        picked_frame = out_frame_;
    }

    out.width     = static_cast<uint32_t>(picked_frame->width);
    out.height    = static_cast<uint32_t>(picked_frame->height);
    out.stride[0] = picked_frame->linesize[0];
    out.stride[1] = picked_frame->linesize[1];
    out.stride[2] = picked_frame->linesize[2];
    out.plane[0]  = picked_frame->data[0];
    out.plane[1]  = picked_frame->data[1];
    out.plane[2]  = picked_frame->data[2];
    out.pts       = static_cast<uint64_t>(picked_frame->pts);

    // A clean frame made it all the way through — the HW path is healthy,
    // so clear the failure streak (only *consecutive* failures fall back).
    hw_fail_streak_ = 0;

    // The plane buffers stay valid until the next decode()/get_frame() —
    // caller MUST upload before then.  in_frame_, sw_frame_, out_frame_
    // are all preallocated and reused across iterations.
    return true;
}

void FfmpegDecoder::note_hw_failure() {
    // Count consecutive hardware-decode failures; after a sustained streak
    // give up on HW so the next reinit() reopens in software (VIV-80).  Only
    // meaningful while actually on the HW path and not already fallen back.
    if (!hw_decode_ || hw_gave_up_) return;
    // (a) solid consecutive failure — fast path.
    if (++hw_fail_streak_ >= kHwFailGiveUp) {
        hw_gave_up_ = true;
        log::warn("FFDec",
                  "hardware decode failed %d frames running — falling back to "
                  "software decode for this session", hw_fail_streak_);
        return;
    }
    // (b) intermittent-but-persistent failure — windowed path.  The streak
    // resets on every clean frame, so a flaky decoder that errors once every
    // few seconds never hits (a); this catches it.
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - hw_win_start_ms_ > kHwFailWindowMs) {
        hw_win_start_ms_ = now;
        hw_win_fails_    = 0;
    }
    if (++hw_win_fails_ >= kHwFailWindowGiveUp) {
        hw_gave_up_ = true;
        log::warn("FFDec",
                  "hardware decode failed %d times in <%llds — flaky HW, "
                  "falling back to software decode for this session",
                  hw_win_fails_,
                  static_cast<long long>(kHwFailWindowMs / 1000));
    }
}

} // namespace vivora::client
