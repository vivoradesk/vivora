#ifdef DESKBEAM_WINDOWS

#include "app/gui/view_session.h"

#include "common/utils/log.h"

namespace deskbeam::gui {

ViewSession::ViewSession(QObject* parent) : QObject(parent) {
    // ~60Hz drive (16ms).  ViewLoopState::iter() has its own internal
    // 1ms idle-sleep; the QTimer paces us for the common case where the
    // network and the decoder both have something to do.
    tick_.setInterval(16);
    connect(&tick_, &QTimer::timeout, this, &ViewSession::onTick);
}

ViewSession::~ViewSession() {
    // Make sure the loop tears down before the platform / window go.
    stop_flag_.store(true, std::memory_order_release);
    tick_.stop();
    loop_.reset();
    platform_.reset();
}

bool ViewSession::start(const GuiViewConfig& cfg) {
    cfg_ = cfg;

    // Build the platform.  It will create a StreamWindow (QWidget) on
    // this (main) thread, reusing the existing QApplication that the
    // host GUI bootstrapped.
    host_ip_storage_ = cfg_.host_ip.empty() ? std::string("0.0.0.0") : cfg_.host_ip;
    platform_ = std::make_unique<WindowsViewPlatform>();
    // argc/argv aren't used in the in-process path (QApplication exists).
    static int dummy_argc = 0;
    static char* dummy_argv[] = { nullptr };
    if (!platform_->init(dummy_argc, dummy_argv,
                         host_ip_storage_.c_str(), cfg_.port)) {
        log::error("ViewSession", "WindowsViewPlatform::init failed");
        return false;
    }

    loop_cfg_.host_ip            = host_ip_storage_.c_str();
    loop_cfg_.port               = cfg_.port;
    loop_cfg_.stun_server        = cfg_.stun_server.empty()        ? nullptr : cfg_.stun_server.c_str();
    loop_cfg_.host_key_hex       = cfg_.host_key_hex.empty()       ? nullptr : cfg_.host_key_hex.c_str();
    loop_cfg_.rendezvous_server  = cfg_.rendezvous_server.empty()  ? nullptr : cfg_.rendezvous_server.c_str();
    loop_cfg_.peer_pubkey_hex    = cfg_.peer_pubkey_hex.empty()    ? nullptr : cfg_.peer_pubkey_hex.c_str();
    loop_cfg_.relay_server       = cfg_.relay_server.empty()       ? nullptr : cfg_.relay_server.c_str();
    loop_cfg_.relay_session_hex  = cfg_.relay_session_hex.empty()  ? nullptr : cfg_.relay_session_hex.c_str();
    loop_cfg_.license_file       = cfg_.license_file.empty()       ? nullptr : cfg_.license_file.c_str();
    loop_cfg_.stop_flag          = &stop_flag_;

    loop_ = std::make_unique<ViewLoopState>();
    if (!loop_->init(*platform_, loop_cfg_)) {
        log::error("ViewSession", "ViewLoopState::init failed (rc=%d)", loop_->exit_code());
        loop_.reset();
        platform_.reset();
        return false;
    }

    tick_.start();
    return true;
}

void ViewSession::stop() {
    stop_flag_.store(true, std::memory_order_release);
    // Don't stop the timer here — let onTick() observe the flag, exit
    // the loop cleanly, and emit finished().  Avoids races where the
    // user clicks stop while iter() is still mid-frame on the decoder.
}

void ViewSession::onTick() {
    if (!loop_) return;
    const bool keep_going = loop_->iter();
    if (!keep_going) {
        tick_.stop();
        loop_.reset();
        platform_.reset();
        if (!finished_emitted_) {
            finished_emitted_ = true;
            emit finished();
        }
    }
}

} // namespace deskbeam::gui

#endif // DESKBEAM_WINDOWS
