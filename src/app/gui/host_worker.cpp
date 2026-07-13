#include "app/gui/host_worker.h"

#include "app/host_loop.h"
#include "common/utils/log.h"

#ifdef VIVORA_WINDOWS
#include "app/windows_host_platform.h"
#endif
#ifdef VIVORA_MACOS
#include "app/mac_host_platform.h"
#endif
#ifdef VIVORA_LINUX
#include "app/linux_host_platform.h"
#endif

namespace vivora::gui {

HostWorker::HostWorker(QObject* parent) : QObject(parent) {
    // The worker QObject lives on the GUI thread; the platform + loop run
    // on the QThread we manage ourselves.  We don't moveToThread the
    // worker itself because we want signals emitted from the worker
    // thread to cross to the GUI thread via queued connections.
}

HostWorker::~HostWorker() {
    stop();
}

void HostWorker::start(const HostWorkerConfig& cfg) {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        log::warn("HostWorker", "start() called while already running");
        return;
    }
    cfg_ = cfg;
    stop_flag_.store(false, std::memory_order_relaxed);
    client_count_.store(0, std::memory_order_relaxed);
    state_.store(0, std::memory_order_relaxed);

    // Build a fresh QThread for each session — keeping a long-lived
    // thread that switches between sessions creates ordering hazards
    // around platform_ lifetime.  Cost of thread create/destroy is in
    // single-digit ms which is irrelevant compared to encoder init.
    QObject::connect(&thread_, &QThread::started, this, [this] {
        runOnWorkerThread();
    }, Qt::DirectConnection);
    QObject::connect(&thread_, &QThread::finished, this, [this] {
        running_.store(false, std::memory_order_release);
        emit stopped();
    });
    thread_.start();
}

void HostWorker::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    stop_flag_.store(true, std::memory_order_release);
    thread_.quit();        // no-op — we don't run an event loop, but cheap.
    thread_.wait();        // blocks until runOnWorkerThread returns.
    // Drop the QThread::started/finished connections so a subsequent
    // start() can re-bind cleanly.
    thread_.disconnect();
}

void HostWorker::runOnWorkerThread() {
    // Build the platform here so all D3D11/DXGI/encoder objects are
    // created and destroyed on the same thread that drives the loop.
#ifdef VIVORA_WINDOWS
    auto* p = new WindowsHostPlatform();
    if (!p->init(cfg_.manual_bitrate_bps, cfg_.encoder_kind, cfg_.codec)) {
        delete p;
        emit initFailed("Windows host platform init failed (DXGI/encoder unavailable)");
        return;
    }
    platform_.reset(p);
#endif
#ifdef VIVORA_MACOS
    auto* p = new MacHostPlatform();
    if (!p->init(cfg_.display_index, cfg_.manual_bitrate_bps, cfg_.codec)) {
        delete p;
        emit initFailed("macOS host platform init failed");
        return;
    }
    platform_.reset(p);
#endif
#ifdef VIVORA_LINUX
    auto* p = new LinuxHostPlatform();
    if (!p->init(cfg_.manual_bitrate_bps, cfg_.codec, cfg_.encoder_kind)) {
        delete p;
        emit initFailed("Linux host platform init failed");
        return;
    }
    platform_.reset(p);
#endif

    vivora::HostLoopConfig lcfg;
    lcfg.port               = cfg_.port;
    lcfg.manual_bitrate_bps = cfg_.manual_bitrate_bps;
    lcfg.encoder_kind       = cfg_.encoder_kind;
    lcfg.codec              = cfg_.codec;
    lcfg.stun_server        = cfg_.stun_server.empty()        ? nullptr : cfg_.stun_server.c_str();
    lcfg.rendezvous_server  = cfg_.rendezvous_server.empty()  ? nullptr : cfg_.rendezvous_server.c_str();
    lcfg.relay_server       = cfg_.relay_server.empty()       ? nullptr : cfg_.relay_server.c_str();
    lcfg.relay_session_hex  = cfg_.relay_session_hex.empty()  ? nullptr : cfg_.relay_session_hex.c_str();
    lcfg.license_file       = cfg_.license_file.empty()       ? nullptr : cfg_.license_file.c_str();
    lcfg.stop_flag          = &stop_flag_;
    lcfg.client_count_out   = &client_count_;
    lcfg.state_out          = &state_;
    lcfg.idle_timeout_min   = cfg_.idle_timeout_min;
    lcfg.idle_warning_sec   = cfg_.idle_warning_sec;
    lcfg.approval_gate      = cfg_.approval_gate;
    lcfg.clipboard          = cfg_.clipboard;   // VIV-22
    lcfg.rendezvous_refresh_flag = &rendezvous_refresh_flag_;
    // Bounce the idle warning through a queued connection so the toast
    // is raised on the GUI thread.  Lambda capture is fine — the worker
    // thread outlives the run_host_loop call.
    lcfg.on_idle_warning    = [this](int seconds_until_disconnect) {
        // Marshal onto the GUI thread before emitting the signal so
        // the AppController's slot (which raises a QSystemTrayIcon
        // toast) runs in the right Qt thread context.
        QMetaObject::invokeMethod(this, [this, seconds_until_disconnect] {
            emit idleWarning(seconds_until_disconnect);
        }, Qt::QueuedConnection);
    };

    vivora::run_host_loop(*platform_, lcfg);

    // Loop exited (stop requested or unrecoverable error).  Tear down
    // the platform on this thread before signalling stopped().
    platform_.reset();
}

} // namespace vivora::gui
