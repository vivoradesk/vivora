#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

namespace vivora::gui {

// Watches OS-level network-change events (interface up/down, WiFi->Ethernet,
// VPN connect/disconnect, wake-from-sleep) and emits a single debounced
// networkChanged() so the host can immediately re-register with the
// rendezvous server instead of waiting out the 30 s keepalive (VIV-57).
//
// Primary source is QNetworkInformation (NLM on Windows, SCNetworkReachability
// on macOS, NetworkManager on Linux) — one cross-platform code path.  On
// static Qt builds the backend plugin must be linked AND imported (see
// Q_IMPORT_PLUGIN in gui_main.cpp, same pattern as the Schannel TLS backend);
// if no backend loads we log a warning and stay inert — the host's 30 s
// rendezvous keepalive still recovers, just slower.
//
// macOS additionally funnels NSWorkspaceDidWakeNotification through
// notifyEvent() (network_change_watcher_mac.mm): reachability often doesn't
// fire on wake when the WiFi re-associates to the same network, yet the NAT
// binding / DHCP lease may have changed while asleep.
//
// Debounce: network transitions fire event bursts (link down, link up, DHCP,
// DNS...).  Each event restarts a 1 s single-shot timer, so exactly one
// networkChanged() fires ~1 s after the burst settles.
class NetworkChangeWatcher : public QObject {
    Q_OBJECT

public:
    explicit NetworkChangeWatcher(QObject* parent = nullptr);
    ~NetworkChangeWatcher() override;

    // True when a QNetworkInformation backend loaded and change events
    // will actually be delivered.
    bool backendAvailable() const { return backend_available_; }

    // Feed an externally detected event (e.g. macOS system wake) through
    // the same debounce.  Safe to call from any thread — bounces to the
    // watcher's thread via a queued invocation.
    void notifyEvent(const QString& reason);

signals:
    // One coalesced network change.  `reason` describes the last trigger
    // in the burst (for info-level logging at the receiver).
    void networkChanged(const QString& reason);

private:
    void onEvent(const QString& reason);

    QTimer  debounce_;
    QString pending_reason_;
    bool    backend_available_ = false;
#ifdef VIVORA_MACOS
    void*   wake_observer_ = nullptr;   // NSObjectProtocol token (retained)
#endif
};

} // namespace vivora::gui
