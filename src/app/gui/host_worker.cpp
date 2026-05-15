#include "app/gui/host_worker.h"

#include "app/host_loop.h"
#include "common/utils/log.h"

#ifdef DESKBEAM_WINDOWS
#include "app/windows_host_platform.h"
#endif
#ifdef DESKBEAM_MACOS
#include "app/mac_host_platform.h"
#endif
#ifdef DESKBEAM_LINUX
#include "app/linux_host_platform.h"
#endif

namespace deskbeam::gui {

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
#ifdef DESKBEAM_WINDOWS
    auto* p = new WindowsHostPlatform();
    if (!p->init(cfg_.manual_bitrate_bps, cfg_.encoder_kind, cfg_.codec)) {
        delete p;
        emit initFailed("Windows host platform init failed (DXGI/encoder unavailable)");
        return;
    }
    platform_.reset(p);
#endif
#ifdef DESKBEAM_MACOS
    auto* p = new MacHostPlatform();
    if (!p->init(cfg_.display_index, cfg_.manual_bitrate_bps, cfg_.codec)) {
        delete p;
        emit initFailed("macOS host platform init failed");
        return;
    }
    platform_.reset(p);
#endif
#ifdef DESKBEAM_LINUX
    auto* p = new LinuxHostPlatform();
    if (!p->init(cfg_.manual_bitrate_bps, cfg_.codec)) {
        delete p;
        emit initFailed("Linux host platform init failed");
        return;
    }
    platform_.reset(p);
#endif

    deskbeam::HostLoopConfig lcfg;
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

    deskbeam::run_host_loop(*platform_, lcfg);

    // Loop exited (stop requested or unrecoverable error).  Tear down
    // the platform on this thread before signalling stopped().
    platform_.reset();
}

} // namespace deskbeam::gui
