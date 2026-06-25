#ifdef VIVORA_WINDOWS

#include "app/windows_host_platform.h"
#include <windows.h>
#include "common/codec/bitrate_controller.h"
#include "common/utils/log.h"
#include <chrono>
#include <cstdlib>
#include <utility>

bool WindowsHostPlatform::init(uint32_t manual_bitrate_bps,
                               vivora::EncoderKind kind,
                               vivora::VideoCodec codec) {
    capture_ = vivora::IScreenCapture::create();
    dxgi_ = dynamic_cast<vivora::DxgiCapture*>(capture_.get());
    if (!capture_ || !capture_->init(0)) {
        vivora::log::error("HOST", "Failed to init capture");
        return false;
    }
    auto res = capture_->get_resolution();
    vivora::log::info("HOST", "Capture: %ux%u", res.width, res.height);

    // HDR carriage: H.264 is 8-bit only, but DXGI hands us FP16 surfaces
    // when the source display is HDR.  AMF "falls back" to 8-bit by
    // CopyResource'ing FP16 bytes into a BGRA surface — which just
    // bit-reinterprets the FP16 channel pairs as BGRA pixels and
    // produces green garbage on the wire.  Promote to HEVC Main10
    // whenever capture is FP16 so HDR survives end-to-end; the client
    // already negotiates the host's actual codec via the handshake, so
    // it picks HEVC automatically.
    auto effective_codec = codec;
    if (effective_codec == vivora::VideoCodec::H264
        && dxgi_ && dxgi_->get_capture_format() == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        vivora::log::warn("HOST",
            "HDR capture (FP16) detected — promoting requested H.264 to "
            "HEVC Main10 so 10-bit colour survives the encode");
        effective_codec = vivora::VideoCodec::HEVC;
    }

    // Save the encoder config — start_encoder() rebuilds the encoder
    // from these whenever a viewer attaches.  Bitrate auto-derives from
    // resolution if the user didn't pin one.
    saved_kind_      = kind;
    saved_codec_     = effective_codec;
    live_bitrate_bps_ = manual_bitrate_bps != 0
        ? manual_bitrate_bps
        : vivora::codec::default_bitrate_for(res.width, res.height, 60);

    // Force initial mouse movement so DXGI produces its first frame
    // immediately (otherwise the duplication blocks until the user
    // happens to wiggle the mouse).  Cheap.
    INPUT mi = {};
    mi.type = INPUT_MOUSE;
    mi.mi.dwFlags = MOUSEEVENTF_MOVE;
    mi.mi.dx = 1;
    SendInput(1, &mi, sizeof(INPUT));

    return true;
}

bool WindowsHostPlatform::start_encoder() {
    if (encoder_) return true;   // already running

    encoder_ = vivora::IVideoEncoder::create(saved_kind_);
    if (!encoder_) {
        vivora::log::error("HOST", "No matching video encoder available");
        return false;
    }
    auto res = capture_->get_resolution();
    vivora::EncoderConfig cfg;
    cfg.width       = res.width;
    cfg.height      = res.height;
    cfg.fps         = 60;
    cfg.bitrate_bps = live_bitrate_bps_;
    // idr_period serves as AMF GOP_SIZE — auto-IDR cadence when no
    // client request comes in. NVENC/QSV ignore it (intra refresh).
    // Auto-IDR is now just a deep safety net; client IdrRequest fires
    // recovery in 50-100ms.  1800 = 30s @ 60fps keeps the 100KB-IDR
    // trickle off the wire.
    cfg.idr_period  = 1800;
    cfg.codec       = saved_codec_;
    // Multi-slice output (VIV-82): localizes burst loss and lets the decoder
    // parallelize.  Opt-in via VIVORA_SLICES while we validate; default 1.
    if (const char* s = std::getenv("VIVORA_SLICES")) {
        int n = std::atoi(s);
        if (n > 1) cfg.num_slices = static_cast<uint32_t>(n);
    }
    if (dxgi_) cfg.input_format = dxgi_->get_capture_format();

    if (!encoder_->init(cfg, dxgi_ ? dxgi_->get_device() : nullptr)) {
        vivora::log::error("HOST", "Failed to init encoder");
        encoder_.reset();
        return false;
    }
    vivora::log::info("HOST", "Encoder started (%s, %u kbps)",
                        saved_codec_ == vivora::VideoCodec::HEVC ? "hevc" : "h264",
                        live_bitrate_bps_ / 1000);
    return true;
}

void WindowsHostPlatform::stop_encoder() {
    if (!encoder_) return;
    encoder_.reset();
    // Drop the staged frame too — its content is from a previous session
    // and may not match the next encoder's input format if config changes.
    staging_valid_ = false;
    vivora::log::info("HOST", "Encoder stopped (no clients attached)");
}

uint32_t WindowsHostPlatform::capture_width()  const { return capture_->get_resolution().width; }
uint32_t WindowsHostPlatform::capture_height() const { return capture_->get_resolution().height; }

void WindowsHostPlatform::set_bitrate(uint32_t bps) {
    // Remember the value even when the encoder is torn down so the
    // next start_encoder() picks up the latest rate.
    live_bitrate_bps_ = bps;
    if (encoder_) encoder_->set_bitrate(bps);
}
void WindowsHostPlatform::request_idr() {
    if (encoder_) encoder_->request_idr();
}
vivora::VideoCodec WindowsHostPlatform::actual_codec() const {
    // Answer from saved config when encoder is torn down — host_loop
    // queries this before the first viewer attaches.
    return encoder_ ? encoder_->get_config().codec : saved_codec_;
}

bool WindowsHostPlatform::capture_and_encode(uint64_t& pts_us,
                                              bool& content_changed,
                                              bool force) {
    // Phase B+: lazy encoder.  host_loop's lazy gate normally prevents
    // us from getting here without an active encoder, but guard anyway
    // — a race between client disconnect + this tick would crash on
    // the encoder_->encode() call below otherwise.
    if (!encoder_) return false;

    vivora::CapturedFrame frame;
    // Non-blocking capture: if DXGI doesn't have a fresh frame, return
    // immediately so the host_loop can fire heartbeat / yield. A 16ms
    // timeout here halved the loop's effective rate on a static screen
    // (every iteration blocked the full frame interval), starving the
    // heartbeat down to ~25fps.
    if (!capture_->capture_frame(frame, 0))
        return false;

    // Stash cursor state before any early-return so get_cursor_state()
    // can report it even on content-unchanged ticks.
    last_cursor_x_ = frame.cursor.x;
    last_cursor_y_ = frame.cursor.y;
    last_cursor_visible_ = frame.cursor.visible;

    pts_us = std::chrono::duration_cast<std::chrono::microseconds>(
        frame.capture_time.time_since_epoch()).count();
    content_changed = frame.content_changed;

    if (!content_changed && !force) {
        capture_->release_frame(frame);
        return false;
    }

    // Mirror the DXGI texture into our own staging copy before letting
    // DXGI release it. re_encode_last() reuses this on idle ticks so the
    // wire stays at full frame-rate cadence even when the screen is static.
    if (dxgi_) {
        ID3D11Device* dev = dxgi_->get_device();
        ID3D11DeviceContext* ctx = dxgi_->get_context();
        if (dev && ctx && frame.texture) {
            if (!staging_tex_) {
                D3D11_TEXTURE2D_DESC desc = {};
                frame.texture->GetDesc(&desc);
                desc.Usage          = D3D11_USAGE_DEFAULT;
                desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags      = 0;
                if (FAILED(dev->CreateTexture2D(&desc, nullptr, staging_tex_.GetAddressOf()))) {
                    vivora::log::warn("HOST", "Heartbeat staging texture alloc failed");
                }
            }
            if (staging_tex_) {
                ctx->CopyResource(staging_tex_.Get(), frame.texture.Get());
                staging_valid_ = true;
            }
        }
    }

    if (!encoder_->encode(frame.texture.Get(), pts_us)) {
        capture_->release_frame(frame);
        return false;
    }
    capture_->release_frame(frame);
    return true;
}

bool WindowsHostPlatform::re_encode_last(uint64_t pts_us) {
    if (!staging_valid_ || !staging_tex_ || !encoder_) return false;
    return encoder_->encode_skip(staging_tex_.Get(), pts_us);
}

bool WindowsHostPlatform::get_cursor_state(CursorState& out) {
    if (!dxgi_) return false;
    const uint32_t w = capture_width();
    const uint32_t h = capture_height();
    if (w == 0 || h == 0) return false;
    float xn = static_cast<float>(last_cursor_x_) / static_cast<float>(w);
    float yn = static_cast<float>(last_cursor_y_) / static_cast<float>(h);
    if (xn < 0.f) xn = 0.f; else if (xn > 1.f) xn = 1.f;
    if (yn < 0.f) yn = 0.f; else if (yn > 1.f) yn = 1.f;
    out.x_norm   = xn;
    out.y_norm   = yn;
    out.visible  = last_cursor_visible_;
    out.shape_id = dxgi_->current_shape_id();
    return true;
}

bool WindowsHostPlatform::take_cursor_shape(CursorShapeView& out) {
    if (!dxgi_) return false;
    vivora::CursorShape s;
    if (!dxgi_->take_new_cursor_shape(s)) return false;
    out.id        = s.id;
    out.width     = s.width;
    out.height    = s.height;
    out.hotspot_x = s.hotspot_x;
    out.hotspot_y = s.hotspot_y;
    out.bgra      = std::move(s.bgra);
    return true;
}

bool WindowsHostPlatform::get_encoded_packet(EncodedPacketView& out) {
    if (!encoder_) return false;
    vivora::EncodedPacket pkt;
    if (!encoder_->get_packet(pkt))
        return false;
    pkt_buf_ = std::move(pkt.data);
    out.data      = pkt_buf_.data();
    out.len       = pkt_buf_.size();
    out.pts       = pkt.pts;
    out.keyframe  = pkt.keyframe;
    out.heartbeat = pkt.heartbeat;
    return true;
}

#endif // VIVORA_WINDOWS
