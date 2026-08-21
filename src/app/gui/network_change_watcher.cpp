// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/gui/network_change_watcher.h"

#include "common/utils/log.h"

#include <QMetaObject>
#include <QNetworkInformation>

namespace vivora::gui {

namespace {

constexpr const char* TAG = "NetChange";

// Network transitions (WiFi toggle, VPN up, wake) fire bursts of events —
// link down, link up, DHCP bound, DNS updated.  One second of silence after
// the last event is enough for the new route to be usable, and keeps us to
// a single rendezvous re-register per transition.
constexpr int DEBOUNCE_MS = 1000;

const char* reachability_name(QNetworkInformation::Reachability r) {
    switch (r) {
    case QNetworkInformation::Reachability::Online:       return "online";
    case QNetworkInformation::Reachability::Site:         return "site";
    case QNetworkInformation::Reachability::Local:        return "local";
    case QNetworkInformation::Reachability::Disconnected: return "disconnected";
    case QNetworkInformation::Reachability::Unknown:      return "unknown";
    default:                                               return "other";
    }
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
const char* transport_name(QNetworkInformation::TransportMedium m) {
    switch (m) {
    case QNetworkInformation::TransportMedium::Ethernet:  return "ethernet";
    case QNetworkInformation::TransportMedium::Cellular:  return "cellular";
    case QNetworkInformation::TransportMedium::WiFi:      return "wifi";
    case QNetworkInformation::TransportMedium::Bluetooth: return "bluetooth";
    case QNetworkInformation::TransportMedium::Unknown:   return "unknown";
    default:                                              return "other";
    }
}
#endif

} // namespace

#ifdef VIVORA_MACOS
// Implemented in network_change_watcher_mac.mm — NSWorkspaceDidWake observer.
void* mac_register_wake_observer(NetworkChangeWatcher* watcher);
void  mac_unregister_wake_observer(void* observer);
#endif

NetworkChangeWatcher::NetworkChangeWatcher(QObject* parent) : QObject(parent) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(DEBOUNCE_MS);
    connect(&debounce_, &QTimer::timeout, this, [this] {
        emit networkChanged(pending_reason_);
    });

#ifdef VIVORA_MACOS
    // Wake observer is independent of the QNetworkInformation backend —
    // register it first so wake-driven refresh works even if the
    // reachability backend fails to load.
    wake_observer_ = mac_register_wake_observer(this);
#endif

    // Static Qt registers no networkinformation backend unless its plugin
    // is linked and imported (Q_IMPORT_PLUGIN in gui_main.cpp — same
    // pattern as the Schannel TLS backend).  Missing backend is
    // non-fatal: the 30 s rendezvous keepalive still recovers, we just
    // lose the fast (<2 s) path.
    if (!QNetworkInformation::loadDefaultBackend()
        || !QNetworkInformation::instance()) {
        log::warn(TAG, "No QNetworkInformation backend available — "
                       "network-change rendezvous refresh disabled "
                       "(30s keepalive still active)");
        return;
    }
    auto* ni = QNetworkInformation::instance();
    backend_available_ = true;
    log::info(TAG, "Network-change watcher active (backend: %s)",
              ni->backendName().toUtf8().constData());

    // Reachability catches interface up/down, WiFi toggle, cable pull.
    connect(ni, &QNetworkInformation::reachabilityChanged, this,
            [this](QNetworkInformation::Reachability r) {
        onEvent(QStringLiteral("reachability -> %1")
                    .arg(QLatin1String(reachability_name(r))));
    });

#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    // Transport medium catches WiFi->Ethernet where reachability stays
    // Online throughout but the local address (and thus the NAT binding
    // the rendezvous knows) changed.
    connect(ni, &QNetworkInformation::transportMediumChanged, this,
            [this](QNetworkInformation::TransportMedium m) {
        onEvent(QStringLiteral("transport -> %1")
                    .arg(QLatin1String(transport_name(m))));
    });
#endif
}

NetworkChangeWatcher::~NetworkChangeWatcher() {
#ifdef VIVORA_MACOS
    mac_unregister_wake_observer(wake_observer_);
    wake_observer_ = nullptr;
#endif
}

void NetworkChangeWatcher::notifyEvent(const QString& reason) {
    // May be called from a non-Qt thread (macOS notification queue) —
    // hop to the watcher's thread before touching the QTimer.
    QMetaObject::invokeMethod(this, [this, reason] { onEvent(reason); },
                              Qt::QueuedConnection);
}

void NetworkChangeWatcher::onEvent(const QString& reason) {
    log::info(TAG, "Network event: %s (debouncing %d ms)",
              reason.toUtf8().constData(), DEBOUNCE_MS);
    pending_reason_ = reason;
    debounce_.start();   // restart — coalesce the burst into one signal
}

} // namespace vivora::gui
