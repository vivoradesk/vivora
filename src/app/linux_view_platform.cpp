#ifdef VIVORA_LINUX

#include "app/linux_view_platform.h"
#include "common/utils/log.h"

#include <QString>
#include <QSurfaceFormat>

#include <cstdlib>
#include <cstring>
#include <utility>

bool LinuxViewPlatform::init(int argc, char* argv[],
                             const char* host_ip, uint16_t port) {
    // Force a desktop OpenGL Core 3.3 context — our YUV shader uses
    // `#version 330 core` and won't compile on the GLES profile that Qt
    // sometimes picks on systems with both available.  Mesa on every
    // distro we target supports 3.3 since the Gallium era.
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setVersion(3, 3);
    fmt.setSwapInterval(0);  // no vsync — we already pace via host loop
    QSurfaceFormat::setDefaultFormat(fmt);

    // Reuse the existing QApplication when the GUI shell already bootstrapped
    // one (in-process Connect from the tray/launcher).  Constructing a second
    // QApplication per process is fatal — in a Release build (Q_ASSERT
    // compiled out) it doesn't abort cleanly but corrupts Qt's global state
    // and the process dies silently, which looked like "the client just
    // closes on Connect".  CLI --view still builds its own.  Mirrors
    // WindowsViewPlatform.
    if (!QApplication::instance()) {
        app_ = std::make_unique<QApplication>(argc, argv);
    }

    window_ = std::make_unique<QMainWindow>();
    window_->setWindowTitle(QString("Vivora — %1:%2").arg(host_ip).arg(port));
    view_ = new vivora::client::QtGlVideoView(window_.get());
    window_->setCentralWidget(view_);
    // Without an explicit focus proxy, QMainWindow keeps keyboard focus and
    // the central widget's keyPressEvent never fires — so all key input
    // got swallowed before reaching the host. Make the GL view the focus
    // proxy so window-level activation routes keys straight to it.
    window_->setFocusProxy(view_);
    // Header subtitle for the in-stream menu (VIV-74) — endpoint for now.
    view_->set_peer_label(QString("%1:%2").arg(host_ip).arg(port));
    window_->resize(1280, 720);
    window_->show();
    view_->setFocus();

    // Threaded pipeline (VIV-81): when enabled, decode runs on its own thread
    // through this pipeline and presents into view_; the legacy decoder_ path
    // is unused.  The view loop drives submit/poll/present.
    const char* pl = std::getenv("VIVORA_PIPELINE");
    const bool want_legacy = pl && (std::strcmp(pl, "legacy") == 0 ||
                                    std::strcmp(pl, "inpoll") == 0);
    if (!want_legacy) {  // threaded decode is the default now (VIV-82)
        pipeline_ = std::make_unique<vivora::LinuxVideoPipeline>(view_);
        vivora::log::info("VIEW", "Linux threaded video pipeline enabled");
    }
    return true;
}

void LinuxViewPlatform::set_input_callback(InputCallback cb) {
    if (view_) view_->set_input_callback(std::move(cb));
}

bool LinuxViewPlatform::pump_events() {
    app_->processEvents();
    return !app_->closingDown() && window_->isVisible();
}

bool LinuxViewPlatform::init_decoder(vivora::VideoCodec codec) {
    if (decoder_) return true;
    decoder_ = std::make_unique<vivora::client::FfmpegDecoder>();
    if (!decoder_->init(codec)) {
        vivora::log::error("VIEW", "Failed to init FFmpeg decoder");
        decoder_.reset();
        return false;
    }
    return true;
}

bool LinuxViewPlatform::decode(const uint8_t* data, size_t len,
                               uint32_t /*timestamp*/, bool keyframe,
                               uint16_t seq_no) {
    if (!decoder_) return false;
    bool ok = decoder_->decode(data, len, seq_no, keyframe);
    if (!ok) vivora::log::warn("VIEW", "Decoder rejected frame seq=%u", seq_no);
    return ok;
}

int LinuxViewPlatform::render() {
    if (!decoder_ || !view_) return 0;
    int count = 0;
    vivora::client::FfmpegDecoder::YuvFrame f;
    while (decoder_->get_frame(f)) {
        // Propagate HDR flag AFTER get_frame — that's where the decoder reads
        // the colour metadata and latches is_hdr_.  Setting it before (as we
        // used to) meant the very first frame painted with last iteration's
        // flag → a brief SDR-coloured first frame on an HDR stream (VIV-78).
        view_->set_hdr(decoder_->is_hdr());
        // First frame: if StreamInfo already arrived, propagate dims;
        // otherwise fall back to decoded size so input mapping has
        // *something* sensible until StreamInfo lands.
        if (pending_stream_w_ != 0 && pending_stream_h_ != 0) {
            view_->set_stream_size(pending_stream_w_, pending_stream_h_);
            pending_stream_w_ = 0;
            pending_stream_h_ = 0;
        }
        view_->update_yuv(f.plane[0], f.stride[0],
                          f.plane[1], f.stride[1],
                          f.plane[2], f.stride[2],
                          f.width, f.height);
        ++count;
    }
    return count;
}

void LinuxViewPlatform::flush_decoder() {
    // Hard re-create — libav 4.4 (Ubuntu 22.04) HEVC's `flush_buffers`
    // leaks POC-MSB tracking across IDRs, producing "Duplicate POC" /
    // "Could not find ref" cascades.  Recreating the AVCodecContext from
    // scratch every IDR (~once / 2s) is cheap and reliable.
    if (decoder_) decoder_->reinit();
}

void LinuxViewPlatform::upload_cursor_shape(const vivora::protocol::CursorShapeMessage& shape) {
    if (view_) view_->upload_cursor_shape(shape);
}

void LinuxViewPlatform::update_cursor_position(const vivora::protocol::CursorPositionMessage& pos) {
    if (view_) view_->update_cursor_position(pos);
}

void LinuxViewPlatform::update_stats(const vivora::StatsView& stats) {
    // Override decoder label and HDR flag — both are decoder-side facts
    // not visible to the cross-platform view loop.
    vivora::StatsView local = stats;
    if (decoder_) {
        std::snprintf(local.decoder, sizeof(local.decoder), "%s",
                      decoder_->backend_name());
        local.hdr = decoder_->is_hdr();
    }
    if (view_) view_->update_stats(local);
}

void LinuxViewPlatform::set_status(const char* text) {
    if (view_) view_->set_status(QString::fromUtf8(text ? text : ""));
}

void LinuxViewPlatform::set_menu_actions(const vivora::MenuActions& actions) {
    if (view_) view_->set_menu_actions(actions);
}

void LinuxViewPlatform::set_stream_size(uint32_t width, uint32_t height) {
    if (view_) view_->set_stream_size(width, height);
    else {
        pending_stream_w_ = width;
        pending_stream_h_ = height;
    }
}

void LinuxViewPlatform::shutdown() {
    decoder_.reset();
    window_.reset();
    app_.reset();
}

#endif // VIVORA_LINUX
