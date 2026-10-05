// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef VIVORA_WINDOWS

#include "app/windows_view_platform.h"
#include <windows.h>
#include "common/utils/log.h"
#include <utility>

bool WindowsViewPlatform::init(int argc, char* argv[],
                                const char* host_ip, uint16_t port) {
    // Do NOT call CoInitializeEx here — QApplication sets the GUI thread
    // to STA (required for OLE drag-and-drop and native dialogs).  Forcing
    // MTA first breaks those subsystems.  The Media Foundation decoder
    // calls CoInitializeEx itself on its own thread if needed.

    // Reuse an existing QApplication when the GUI host has already
    // bootstrapped one in this process (in-process Connect from the
    // tray app).  CLI path still constructs its own.  Qt forbids two
    // QApplications per process; this check is what lets the host UI
    // and a view session coexist in the same vivora.exe.
    if (!QApplication::instance()) {
        app_ = std::make_unique<QApplication>(argc, argv);
    }
    window_ = std::make_unique<vivora::StreamWindow>();
    window_->setWindowTitle(QString("Vivora — %1:%2").arg(host_ip).arg(port));
    // Loopback session (viewing this same machine): relative-cursor mode
    // would hide/clip the user's own pointer — disable it (VIV-50).
    const QString hip = QString::fromUtf8(host_ip ? host_ip : "");
    window_->set_loopback(hip.startsWith("127.") || hip == "localhost");
    // Header subtitle for the in-stream menu (VIV-74).  Friendly device
    // names aren't plumbed to the view layer yet, so show the endpoint.
    window_->set_peer_label(QString("%1:%2").arg(host_ip).arg(port));
    window_->show();
    return true;
}

bool WindowsViewPlatform::init_decoder(vivora::VideoCodec codec) {
    if (decoder_ && codec == codec_) return true;
    decoder_.reset();   // mid-session codec switch (VIV-147): rebuild for the new one
    codec_ = codec;
    decoder_ = vivora::IVideoDecoder::create();
    if (!decoder_->init(codec)) {
        vivora::log::error("VIEW", "Failed to init decoder");
        decoder_.reset();
        return false;
    }
    return true;
}

void WindowsViewPlatform::set_input_callback(InputCallback cb) {
    window_->set_input_callback(std::move(cb));
}

bool WindowsViewPlatform::pump_events() {
    app_->processEvents();
    return !app_->closingDown() && window_->isVisible();
}

bool WindowsViewPlatform::decode(const uint8_t* data, size_t len,
                                  uint32_t /*timestamp*/, bool /*keyframe*/,
                                  uint16_t seq_no) {
    if (!decoder_) return false;
    bool ok = decoder_->decode(data, len, seq_no);
    if (!ok) vivora::log::warn("VIEW", "Decoder rejected frame seq=%u", seq_no);
    return ok;
}

int WindowsViewPlatform::render() {
    if (!decoder_) return 0;
    int count = 0;
    vivora::DecodedFrame decoded;
    while (decoder_->get_frame(decoded)) {
        // Monitor switch (VIV-50): decoded size changed mid-session → rebuild
        // the renderer for the new geometry (mirrors the threaded pipeline).
        if (renderer_ready_ && decoded.width > 0 && decoded.height > 0
            && (decoded.width != renderer_w_ || decoded.height != renderer_h_)) {
            vivora::log::info("VIEW", "Decoded size changed %ux%u -> %ux%u — reinit renderer",
                              renderer_w_, renderer_h_, decoded.width, decoded.height);
            renderer_ready_ = false;
        }
        if (!renderer_ready_ && decoded.width > 0 && decoded.height > 0 && decoded.texture) {
            D3D11_TEXTURE2D_DESC tex_desc = {};
            decoded.texture->GetDesc(&tex_desc);
            renderer_ready_ = window_->init_renderer(
                decoder_->get_device(), decoded.width, decoded.height, tex_desc.Format);
            if (renderer_ready_) {
                renderer_w_ = decoded.width;
                renderer_h_ = decoded.height;
                // If StreamInfo arrived before the first frame, apply the
                // real crop dims now that the renderer exists.  Otherwise
                // fall back to the decoded (possibly padded) dims so mouse
                // mapping still works until StreamInfo lands.
                if (pending_stream_w_ != 0 && pending_stream_h_ != 0) {
                    window_->set_stream_size(pending_stream_w_, pending_stream_h_);
                } else {
                    window_->set_stream_size(decoded.width, decoded.height);
                }
                vivora::log::info("VIEW", "Renderer started: %ux%u, format=%u",
                                    decoded.width, decoded.height, tex_desc.Format);
            }
        }
        if (renderer_ready_) {
            window_->render_frame(decoded.texture.Get(), decoded.subresource);
        }
        // decoded.texture ComPtr releases on scope exit / next loop iter.
        count++;
    }
    return count;
}

void WindowsViewPlatform::flush_decoder() { if (decoder_) decoder_->flush(); }

void WindowsViewPlatform::upload_cursor_shape(const vivora::protocol::CursorShapeMessage& shape) {
    if (window_) window_->upload_cursor_shape(shape);
}

void WindowsViewPlatform::update_cursor_position(const vivora::protocol::CursorPositionMessage& pos) {
    if (window_) window_->update_cursor_position(pos);
}

void WindowsViewPlatform::set_stream_size(uint32_t width, uint32_t height) {
    pending_stream_w_ = width;
    pending_stream_h_ = height;
    if (window_) window_->set_stream_size(width, height);
    // The threaded pipeline applies these right after its lazy renderer init
    // (StreamInfo may arrive before the first decoded frame) — VIV-84.
    if (pipeline_) pipeline_->set_pending_stream_size(width, height);
}

vivora::IVideoPipeline* WindowsViewPlatform::video_pipeline() {
    if (!window_) return nullptr;
    if (!pipeline_) {
        pipeline_ = std::make_unique<vivora::WindowsVideoPipeline>(window_.get());
        if (pending_stream_w_ && pending_stream_h_)
            pipeline_->set_pending_stream_size(pending_stream_w_, pending_stream_h_);
    }
    return pipeline_.get();
}

void WindowsViewPlatform::update_stats(const vivora::StatsView& stats) {
    if (window_) window_->update_stats(stats);
}

void WindowsViewPlatform::set_status(const char* text) {
    if (window_) window_->set_status(text ? text : "");
}

void WindowsViewPlatform::set_menu_actions(const vivora::MenuActions& actions) {
    if (window_) window_->set_menu_actions(actions);
}

void WindowsViewPlatform::set_monitor_list(
        const std::vector<vivora::protocol::MonitorDesc>& monitors) {
    if (window_) window_->set_monitor_list(monitors);
}

void WindowsViewPlatform::shutdown() {
    pipeline_.reset();   // before window_ — it holds a StreamWindow*
    window_.reset();
    app_.reset();
}

#endif // VIVORA_WINDOWS
