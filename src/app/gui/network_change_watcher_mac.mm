// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// macOS wake observer for NetworkChangeWatcher (VIV-57).
//
// Wake-from-sleep often re-associates WiFi to the same SSID without a
// reachability transition, yet the DHCP lease / NAT binding may have
// changed while asleep — so the rendezvous registration is stale even
// though QNetworkInformation stays quiet.  Mirrors the existing
// NSWorkspaceDidWakeNotification subscription MacScreenCapture uses to
// restart SCStream (VIV-10), but lives in the GUI layer so capture code
// stays decoupled from session/rendezvous concerns.
//
// Built without ARC (project-wide): the observer token returned by
// addObserverForName: is autoreleased, so we retain it here and release
// it after removeObserver:.

#include "app/gui/network_change_watcher.h"

#import <AppKit/AppKit.h>

namespace vivora::gui {

void* mac_register_wake_observer(NetworkChangeWatcher* watcher) {
    id observer = [[[NSWorkspace sharedWorkspace] notificationCenter]
        addObserverForName:NSWorkspaceDidWakeNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification* /*note*/) {
        // notifyEvent is thread-safe (queued hop to the watcher's thread).
        watcher->notifyEvent(QStringLiteral("system wake"));
    }];
    return (void*)[observer retain];
}

void mac_unregister_wake_observer(void* observer) {
    if (!observer) return;
    id obs = (id)observer;
    [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:obs];
    [obs release];
}

} // namespace vivora::gui
