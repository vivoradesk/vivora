#include "app/gui/view_session.h"

#ifdef VIVORA_WINDOWS
#include "app/windows_view_platform.h"
#endif
#ifdef VIVORA_MACOS
#include "app/mac_view_platform.h"
#endif

#include "common/utils/log.h"

namespace vivora::gui {

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

bool ViewSession::init_platform() {
#ifdef VIVORA_WINDOWS
    auto p = std::make_unique<WindowsViewPlatform>();
    // argc/argv aren't used in the in-process path (QApplication exists).
    static int dummy_argc = 0;
    static char* dummy_argv[] = { nullptr };
    if (!p->init(dummy_argc, dummy_argv,
                 host_ip_storage_.c_str(), cfg_.port)) {
        return false;
    }
    platform_ = std::move(p);
    return true;
#elif defined(VIVORA_MACOS)
    auto p = std::make_unique<MacViewPlatform>();
    if (!p->init(host_ip_storage_.c_str(), cfg_.port)) {
        return false;
    }
    platform_ = std::move(p);
    return true;
#else
    // Linux GUI not wired yet — see src/app/CMakeLists.txt comment.
    log::error("ViewSession", "GUI not built for this platform");
    return false;
#endif
}

bool ViewSession::start(const GuiViewConfig& cfg) {
    cfg_ = cfg;

    // Build the platform.  It will create its window (Windows QWidget or
    // macOS NSWindow) on this (main) thread, reusing the existing
    // QApplication that the GUI shell bootstrapped.
    host_ip_storage_ = cfg_.host_ip.empty() ? std::string("0.0.0.0") : cfg_.host_ip;
    if (!init_platform()) {
        log::error("ViewSession", "platform init failed");
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

} // namespace vivora::gui
