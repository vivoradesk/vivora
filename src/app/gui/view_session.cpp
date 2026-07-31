#include "app/gui/view_session.h"

#ifdef VIVORA_WINDOWS
#include "app/windows_view_platform.h"
#endif
#ifdef VIVORA_MACOS
#include "app/mac_view_platform.h"
#endif
#ifdef VIVORA_LINUX
#include "app/linux_view_platform.h"
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
    // Join the connect worker if it's still running (only when the session is
    // destroyed mid-connect, e.g. app shutdown).  loop_->init() is not
    // interruptible, so this can block until the in-flight handshake resolves.
    if (connect_thread_.joinable()) connect_thread_.join();
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
#elif defined(VIVORA_LINUX)
    auto p = std::make_unique<LinuxViewPlatform>();
    // argc/argv aren't used in the in-process path (QApplication exists).
    static int dummy_argc = 0;
    static char* dummy_argv[] = { nullptr };
    if (!p->init(dummy_argc, dummy_argv,
                 host_ip_storage_.c_str(), cfg_.port)) {
        return false;
    }
    platform_ = std::move(p);
    return true;
#else
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
    // VIV-23: GUI asks the user before trusting a new / changed peer key.
    loop_cfg_.interactive_trust  = true;
    // Viewing caps (Settings → Viewing, 0 = none).
    loop_cfg_.view_fps_cap  = static_cast<uint16_t>(std::max(0, cfg_.view_fps_cap));
    loop_cfg_.view_max_kbps = static_cast<uint32_t>(std::max(0, cfg_.view_max_kbps));
    // VIV-54: auto-reconnect budget (the loop derives the banner peer label
    // from peer_pubkey_hex, so no separate label plumbing is needed).
    loop_cfg_.reconnect_timeout_ms = cfg_.reconnect_timeout_ms;

    // VIV-22: clipboard sync client-side.  ClipboardSync watches QClipboard
    // on this (GUI) thread; the loop drains/fills the bridge every iter().
    clipboardBridge_ = std::make_shared<vivora::ClipboardBridge>();
    clipboardSync_   = std::make_unique<ClipboardSync>(clipboardBridge_);
    loop_cfg_.clipboard = clipboardBridge_;

    loop_ = std::make_unique<ViewLoopState>();

#ifdef VIVORA_WINDOWS
    // Windows: run the blocking connect (DNS resolves + rendezvous lookup + hole
    // punch + Noise handshake) on a worker thread so the GUI event loop — the
    // launcher window and the tray — stays responsive during a cold connect,
    // where getaddrinfo can stall several seconds on a cold resolver.  Safe here
    // because D3D11/HWND tolerate the split: loop_->init() only STORES callbacks
    // on the platform and drives the socket, and the tick timer that reads
    // loop_/platform_ isn't started until finishConnect() runs back on the GUI
    // thread — no concurrent platform access.
    connect_thread_ = std::thread([this]() {
        const bool ok = loop_->init(*platform_, loop_cfg_);
        QMetaObject::invokeMethod(this, [this, ok]() { finishConnect(ok); },
                                  Qt::QueuedConnection);
    });
#else
    // macOS (AppKit) and Linux (X11) are NOT thread-safe for the platform work
    // loop_->init() ends up doing (Cocoa view / CoreVideo, X11), so connect on
    // the main thread — a worker there corrupts the view and crashes on
    // teardown.  finishConnect() is still deferred to the next event-loop turn
    // (queued) so the caller can finish start() and track us first, keeping the
    // success/trust/failure handling identical to the Windows path.
    const bool ok = loop_->init(*platform_, loop_cfg_);
    QMetaObject::invokeMethod(this, [this, ok]() { finishConnect(ok); },
                              Qt::QueuedConnection);
#endif
    return true;
}

void ViewSession::finishConnect(bool ok) {
    // The worker has posted us and is about to return — join it so the
    // std::thread is cleanly reaped (and never outlives this object).
    if (connect_thread_.joinable()) connect_thread_.join();

    if (ok) {
        everConnected_ = true;
        tick_.start();
        return;
    }

    // Connect failed.  VIV-23: capture a pending TOFU trust question before the
    // loop is destroyed, so AppController can show the dialog and re-dial.
    vivora::client::TrustPending tp;
    if (loop_ && loop_->trust_pending(tp)) {
        trustPending_  = true;
        trustMismatch_ = tp.mismatch;
        trustPeerCode_ = QString::fromStdString(tp.code);
        trustNewHex_   = QString::fromStdString(tp.pubkey_hex);
        trustOldHex_   = QString::fromStdString(tp.stored_hex);
        log::info("ViewSession", "Connect paused: trust decision needed for '%s'",
                  tp.code.c_str());
    } else {
        log::error("ViewSession", "ViewLoopState::init failed (rc=%d)",
                   loop_ ? loop_->exit_code() : -1);
    }
    loop_.reset();
    platform_.reset();
    clipboardSync_.reset();
    clipboardBridge_.reset();
    if (!finished_emitted_) {
        finished_emitted_ = true;
        emit finished();
    }
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
        // VIV-54: a live session can end mid-flight because the host's key
        // changed during an auto-reconnect (possible MITM).  Capture the
        // pending TOFU trust question before the loop is destroyed so
        // AppController can raise the same VIV-23 dialog it uses on connect.
        vivora::client::TrustPending tp;
        if (loop_->trust_pending(tp)) {
            trustPending_  = true;
            trustMismatch_ = tp.mismatch;
            trustPeerCode_ = QString::fromStdString(tp.code);
            trustNewHex_   = QString::fromStdString(tp.pubkey_hex);
            trustOldHex_   = QString::fromStdString(tp.stored_hex);
            log::info("ViewSession", "Reconnect stopped: host key changed for '%s'",
                      tp.code.c_str());
        }
        tick_.stop();
        loop_.reset();
        platform_.reset();
        clipboardSync_.reset();     // VIV-22: stop watching the clipboard
        clipboardBridge_.reset();
        if (!finished_emitted_) {
            finished_emitted_ = true;
            emit finished();
        }
    }
}

} // namespace vivora::gui
