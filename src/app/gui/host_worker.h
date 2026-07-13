#pragma once

#include "app/host_platform.h"
#include "host/encode/video_encoder.h"
#include "host/session/host_approval_gate.h"

#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace vivora::gui {

// Strongly-owning twin of HostLoopConfig.  HostLoopConfig holds raw
// `const char*` pointers that the loop dereferences on the worker thread
// — the controller (which lives on the GUI thread) needs durable storage
// for those strings, so we copy them into std::string here and the worker
// hands `c_str()` pointers into the loop config it builds.
struct HostWorkerConfig {
    uint16_t              port              = 9876;
    uint32_t              manual_bitrate_bps = 0;
    vivora::EncoderKind encoder_kind      = vivora::EncoderKind::Auto;
    vivora::VideoCodec  codec             = vivora::VideoCodec::HEVC;
    std::string           stun_server;
    std::string           rendezvous_server;
    std::string           relay_server;
    std::string           relay_session_hex;
    std::string           license_file;
    uint32_t              display_index     = 0;   // mac/linux only
    int                   idle_timeout_min  = 0;   // 0 disables
    int                   idle_warning_sec  = 30;
    // Optional approval gate (VIV-53).  When set, every new client
    // lands Pending and the gate's callback fires on the worker
    // thread so the GUI can raise the approval popup.
    std::shared_ptr<vivora::host::HostApprovalGate> approval_gate;
};

// Owns a HostPlatform and runs run_host_loop on its own QThread.  The GUI
// thread interacts with it only via start()/stop() and atomic counters
// polled with state() / clientCount().  All Qt signals are emitted across
// thread boundaries via queued connections.
class HostWorker : public QObject {
    Q_OBJECT

public:
    explicit HostWorker(QObject* parent = nullptr);
    ~HostWorker() override;

    // Spin up the platform and start the host loop on the worker thread.
    // No-op if already running.
    void start(const HostWorkerConfig& cfg);

    // Signal the loop to exit and join the worker thread.  Blocks until
    // the loop tears down (typically <50ms — one poll cycle plus encoder
    // shutdown).  Safe to call from the GUI thread.
    void stop();

    // Force the next poll iteration to re-register with rendezvous,
    // bypassing the 30 s pacing.  Wired to the GUI Refresh button so
    // the user can prod the registration if they suspect the server
    // forgot us, and to NetworkChangeWatcher (VIV-57) so a network
    // change updates the reflexive mapping within ~2 s instead of
    // waiting out the keepalive.  No-op if no rendezvous configured.
    void requestRendezvousRefresh() {
        rendezvous_refresh_flag_.store(true, std::memory_order_release);
    }

    bool running() const { return running_.load(std::memory_order_relaxed); }
    int  clientCount() const { return client_count_.load(std::memory_order_relaxed); }
    int  state() const       { return state_.load(std::memory_order_relaxed); }

signals:
    // Emitted once the loop has actually exited (after stop() or fatal
    // platform init failure).  The controller uses this to flip its
    // "sharing" property back to false.
    void stopped();
    // Platform init failed (no DXGI device, encoder unavailable, etc.).
    void initFailed(QString reason);
    // Idle warning fired — clients have been silent for idle_timeout_min
    // and we'll force-disconnect them in `seconds_until_disconnect`
    // seconds unless they send input first.  AppController surfaces
    // this as a tray toast.  Always delivered on the GUI thread (queued
    // connection across the QThread boundary).
    void idleWarning(int seconds_until_disconnect);

private:
    void runOnWorkerThread();

    QThread                          thread_;
    HostWorkerConfig                 cfg_;
    std::atomic<bool>                stop_flag_{false};
    std::atomic<int>                 client_count_{0};
    std::atomic<int>                 state_{0};
    std::atomic<bool>                running_{false};
    std::atomic<bool>                rendezvous_refresh_flag_{false};
    std::unique_ptr<HostPlatform>    platform_;   // lives on worker thread
};

} // namespace vivora::gui
