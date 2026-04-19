#ifdef DESKBEAM_WINDOWS

#include "app/windows_view_platform.h"
#include <windows.h>
#include <objbase.h>
#include "common/utils/log.h"
#include <utility>

bool WindowsViewPlatform::init(int argc, char* argv[],
                                const char* host_ip, uint16_t port) {
    // Initialize COM as MTA before Qt (which may set STA).
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Decoder creation deferred to init_decoder() — codec is only known
    // after the HELLO_ACK handshake.
    app_ = std::make_unique<QApplication>(argc, argv);
    window_ = std::make_unique<deskbeam::StreamWindow>();
    window_->setWindowTitle(QString("DeskBeam — %1:%2").arg(host_ip).arg(port));
    window_->show();
    return true;
}

bool WindowsViewPlatform::init_decoder(deskbeam::VideoCodec codec) {
    if (decoder_) return true;
    decoder_ = deskbeam::IVideoDecoder::create();
    if (!decoder_->init(codec)) {
        deskbeam::log::error("VIEW", "Failed to init decoder");
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
    if (!ok) deskbeam::log::warn("VIEW", "Decoder rejected frame seq=%u", seq_no);
    return ok;
}

int WindowsViewPlatform::render() {
    if (!decoder_) return 0;
    int count = 0;
    deskbeam::DecodedFrame decoded;
    while (decoder_->get_frame(decoded)) {
        if (!renderer_ready_ && decoded.width > 0 && decoded.height > 0 && decoded.texture) {
            D3D11_TEXTURE2D_DESC tex_desc = {};
            decoded.texture->GetDesc(&tex_desc);
            renderer_ready_ = window_->init_renderer(
                decoder_->get_device(), decoded.width, decoded.height, tex_desc.Format);
            if (renderer_ready_) {
                // If StreamInfo arrived before the first frame, apply the
                // real crop dims now that the renderer exists.  Otherwise
                // fall back to the decoded (possibly padded) dims so mouse
                // mapping still works until StreamInfo lands.
                if (pending_stream_w_ != 0 && pending_stream_h_ != 0) {
                    window_->set_stream_size(pending_stream_w_, pending_stream_h_);
                } else {
                    window_->set_stream_size(decoded.width, decoded.height);
                }
                deskbeam::log::info("VIEW", "Renderer started: %ux%u, format=%u",
                                    decoded.width, decoded.height, tex_desc.Format);
            }
        }
        if (renderer_ready_) {
            window_->render_frame(decoded.texture, decoded.subresource);
        }
        if (decoded.texture) decoded.texture->Release();
        count++;
    }
    return count;
}

void WindowsViewPlatform::flush_decoder() { if (decoder_) decoder_->flush(); }

void WindowsViewPlatform::upload_cursor_shape(const deskbeam::protocol::CursorShapeMessage& shape) {
    if (window_) window_->upload_cursor_shape(shape);
}

void WindowsViewPlatform::update_cursor_position(const deskbeam::protocol::CursorPositionMessage& pos) {
    if (window_) window_->update_cursor_position(pos);
}

void WindowsViewPlatform::set_stream_size(uint32_t width, uint32_t height) {
    pending_stream_w_ = width;
    pending_stream_h_ = height;
    if (window_) window_->set_stream_size(width, height);
}

void WindowsViewPlatform::shutdown() {
    window_.reset();
    app_.reset();
}

#endif // DESKBEAM_WINDOWS
