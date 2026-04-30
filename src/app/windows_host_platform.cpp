#ifdef DESKBEAM_WINDOWS

#include "app/windows_host_platform.h"
#include <windows.h>
#include "common/codec/bitrate_controller.h"
#include "common/utils/log.h"
#include <chrono>
#include <utility>

bool WindowsHostPlatform::init(uint32_t manual_bitrate_bps,
                               deskbeam::EncoderKind kind,
                               deskbeam::VideoCodec codec) {
    capture_ = deskbeam::IScreenCapture::create();
    dxgi_ = dynamic_cast<deskbeam::DxgiCapture*>(capture_.get());
    if (!capture_ || !capture_->init(0)) {
        deskbeam::log::error("HOST", "Failed to init capture");
        return false;
    }
    auto res = capture_->get_resolution();
    deskbeam::log::info("HOST", "Capture: %ux%u", res.width, res.height);

    uint32_t bitrate = manual_bitrate_bps;
    if (bitrate == 0)
        bitrate = deskbeam::codec::default_bitrate_for(res.width, res.height, 60);

    encoder_ = deskbeam::IVideoEncoder::create(kind);
    if (!encoder_) {
        deskbeam::log::error("HOST", "No matching video encoder available");
        return false;
    }
    deskbeam::EncoderConfig cfg;
    cfg.width = res.width;
    cfg.height = res.height;
    cfg.fps = 60;
    cfg.bitrate_bps = bitrate;
    // idr_period serves as AMF GOP_SIZE — auto-IDR cadence when no client
    // request comes in. NVENC/QSV ignore it (intra refresh). With the
    // crypto-decrypt zero-payload bug fixed (2026-04-29), client IdrRequest
    // packets actually reach the host now, so recovery latency is ~50-100ms
    // via request_idr() instead of having to wait for the next GOP boundary.
    // Auto-IDR is now just a deep safety net for the worst case of total
    // bidirectional loss — keep it long to minimise the steady-state
    // 100KB-IDR trickle on the wire. 1800 = 30s @ 60fps.
    cfg.idr_period = 1800;
    cfg.codec = codec;
    if (dxgi_) cfg.input_format = dxgi_->get_capture_format();

    if (!encoder_->init(cfg, dxgi_ ? dxgi_->get_device() : nullptr)) {
        deskbeam::log::error("HOST", "Failed to init encoder");
        return false;
    }

    // Force initial mouse movement for DXGI.
    INPUT mi = {};
    mi.type = INPUT_MOUSE;
    mi.mi.dwFlags = MOUSEEVENTF_MOVE;
    mi.mi.dx = 1;
    SendInput(1, &mi, sizeof(INPUT));

    return true;
}

uint32_t WindowsHostPlatform::capture_width()  const { return capture_->get_resolution().width; }
uint32_t WindowsHostPlatform::capture_height() const { return capture_->get_resolution().height; }

void WindowsHostPlatform::set_bitrate(uint32_t bps) { encoder_->set_bitrate(bps); }
void WindowsHostPlatform::request_idr() { encoder_->request_idr(); }
deskbeam::VideoCodec WindowsHostPlatform::actual_codec() const {
    return encoder_ ? encoder_->get_config().codec : deskbeam::VideoCodec::HEVC;
}

bool WindowsHostPlatform::capture_and_encode(uint64_t& pts_us,
                                              bool& content_changed,
                                              bool force) {
    deskbeam::CapturedFrame frame;
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
                    deskbeam::log::warn("HOST", "Heartbeat staging texture alloc failed");
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
    deskbeam::CursorShape s;
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
    deskbeam::EncodedPacket pkt;
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

#endif // DESKBEAM_WINDOWS
