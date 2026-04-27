#ifdef DESKBEAM_LINUX

#include "app/linux_view_platform.h"
#include "common/utils/log.h"

#include <QString>
#include <QSurfaceFormat>

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

    app_ = std::make_unique<QApplication>(argc, argv);

    window_ = std::make_unique<QMainWindow>();
    window_->setWindowTitle(QString("DeskBeam — %1:%2").arg(host_ip).arg(port));
    view_ = new deskbeam::client::QtGlVideoView(window_.get());
    window_->setCentralWidget(view_);
    window_->resize(1280, 720);
    window_->show();
    return true;
}

void LinuxViewPlatform::set_input_callback(InputCallback cb) {
    if (view_) view_->set_input_callback(std::move(cb));
}

bool LinuxViewPlatform::pump_events() {
    app_->processEvents();
    return !app_->closingDown() && window_->isVisible();
}

bool LinuxViewPlatform::init_decoder(deskbeam::VideoCodec codec) {
    if (decoder_) return true;
    decoder_ = std::make_unique<deskbeam::client::FfmpegDecoder>();
    if (!decoder_->init(codec)) {
        deskbeam::log::error("VIEW", "Failed to init FFmpeg decoder");
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
    if (!ok) deskbeam::log::warn("VIEW", "Decoder rejected frame seq=%u", seq_no);
    return ok;
}

int LinuxViewPlatform::render() {
    if (!decoder_ || !view_) return 0;
    // Propagate HDR flag from decoder — set after first frame's color
    // metadata is read.  Cheap enough to refresh every render iteration;
    // the setter is a trivial bool store.
    view_->set_hdr(decoder_->is_hdr());
    int count = 0;
    deskbeam::client::FfmpegDecoder::YuvFrame f;
    while (decoder_->get_frame(f)) {
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

#endif // DESKBEAM_LINUX
