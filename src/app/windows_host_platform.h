// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#ifdef VIVORA_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "app/host_platform.h"
#include "host/capture/screen_capture.h"
#include "host/capture/dxgi_capture.h"
#include "host/encode/video_encoder.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <memory>
#include <vector>

class WindowsHostPlatform : public vivora::HostPlatform {
public:
    // `stream_fps` is the VIV-67 framerate cap — it becomes the encoder's
    // declared rate (and thereby the intra-refresh period: one full
    // refresh per second).  Defaulted so the legacy CLI keeps compiling.
    bool init(uint32_t manual_bitrate_bps,
              vivora::EncoderKind kind,
              vivora::VideoCodec codec,
              uint16_t stream_fps = 60,
              // Display to capture at startup.  --display N was documented but
              // never reached DXGI on Windows: capture always came up on
              // output 0 and the flag silently did nothing (VIV-147).
              uint32_t display_index = 0);

    uint32_t capture_width()  const override;
    uint32_t capture_height() const override;

    void set_bitrate(uint32_t bps) override;
    void request_idr() override;
    vivora::VideoCodec actual_codec() const override;
    bool set_codec(vivora::VideoCodec codec) override;

    std::vector<vivora::protocol::MonitorDesc> list_monitors() override;
    bool select_monitor(uint32_t index, bool seed_cursor = true) override;
    bool supports_monitor_switch() const override { return true; }
    bool poll_display_change() override;
    bool refresh_capture() override;
    int32_t input_origin_x() const override;
    int32_t input_origin_y() const override;

    bool capture_and_encode(uint64_t& pts_us,
                            bool& content_changed,
                            bool force) override;
    double last_capture_ms() const override { return last_capture_ms_; }
    bool re_encode_last(uint64_t pts_us) override;
    bool get_encoded_packet(EncodedPacketView& out) override;

    bool get_cursor_state(CursorState& out) override;
    bool take_cursor_shape(CursorShapeView& out) override;

    // Phase B+ encoder lifecycle.  init() does only capture setup;
    // start_encoder builds the actual NVENC/AMF/QSV session, stop_encoder
    // tears it down.  Capture (DXGI Duplicate1 handle + staging_tex_)
    // stays alive across the gap so reconnect skips its setup cost.
    bool start_encoder() override;
    void stop_encoder() override;

private:
    // Recompute the auto bitrate for the current capture resolution.  No-op
    // when --bitrate pinned one.
    void rederive_bitrate();

    // The codec that can actually carry the current capture surface: `want`,
    // unless it is 8-bit H.264 and DXGI is handing us FP16.
    vivora::VideoCodec codec_for_capture(vivora::VideoCodec want) const;

    // Tear down + rebuild the encoder for the capture's current geometry and
    // pixel format, and latch geometry_changed_ for host_loop.
    void rebuild_for_geometry();
    // Set when the capture geometry/format moved and the encoder was rebuilt;
    // consumed by refresh_capture() so host_loop re-sends StreamInfo, re-arms
    // input mapping and forces a keyframe.
    bool geometry_changed_ = false;

    // Capture-only wall time of the last frame; see HostPlatform.
    double last_capture_ms_ = 0.0;

    std::unique_ptr<vivora::IScreenCapture> capture_;
    vivora::DxgiCapture* dxgi_ = nullptr;
    std::unique_ptr<vivora::IVideoEncoder> encoder_;
    std::vector<uint8_t> pkt_buf_;

    // Saved encoder config so start_encoder() can rebuild after a
    // stop_encoder() teardown.  Bitrate is mutable across the session
    // (BitrateController updates it on RTT/loss changes) — we keep a
    // running copy here that set_bitrate() updates whether or not the
    // encoder is currently live.
    vivora::EncoderKind saved_kind_  = vivora::EncoderKind::Auto;
    vivora::VideoCodec  saved_codec_ = vivora::VideoCodec::HEVC;
    uint32_t              live_bitrate_bps_ = 0;
    uint16_t              saved_fps_        = 60;   // VIV-67 framerate cap
    // Whether --bitrate pinned the rate.  When it didn't, a resolution change
    // (monitor switch / mode change) re-derives the auto default for the new
    // size instead of carrying the old display's budget over.
    bool                  manual_bitrate_   = false;

    // Owned mirror of the most recently captured DXGI texture, fed to the
    // encoder by re_encode_last() when the screen is static and capture
    // emits nothing. Lazily allocated on the first successful capture.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_tex_;
    bool staging_valid_ = false;

    // Latest cursor position observed from DXGI (raw host pixels).
    int32_t last_cursor_x_ = 0;
    int32_t last_cursor_y_ = 0;
    bool    last_cursor_visible_ = false;
};

#endif // VIVORA_WINDOWS
