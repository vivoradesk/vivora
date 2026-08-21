// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "app/clipboard_bridge.h"

#include <QByteArray>
#include <QObject>
#include <QTimer>

#include <memory>

namespace vivora::gui {

// GUI-thread half of the VIV-22 clipboard sync.  Watches the system
// clipboard through QClipboard (which abstracts the CF_UNICODETEXT /
// NSPasteboard / X11-selection zoo and hands us Unicode text we normalise
// to UTF-8) and exchanges updates with the session loop through a
// ClipboardBridge:
//
//   local copy  → QClipboard::dataChanged → push_outbound(bridge)
//   remote copy → poll timer take_inbound(bridge) → QClipboard::setText
//
// Both the host side (AppController while sharing) and the client side
// (ViewSession while viewing) own one of these; only the bridge peer
// differs (host worker loop vs view loop).
//
// Echo-loop prevention: writing the clipboard from a received message makes
// the OS fire dataChanged again.  We remember the last applied/sent payload
// and drop any change whose content matches it (plus an `applying_` flag as
// belt-and-braces), so remote → local application never re-broadcasts and
// repeated identical copies don't spam the wire.
//
// Known limitation (accepted): on Linux/Wayland, QClipboard change signals
// may only fire while a window of this app has focus — clipboard changes
// made while Vivora is fully unfocused can be missed until the next focus.
class ClipboardSync : public QObject {
    Q_OBJECT

public:
    explicit ClipboardSync(std::shared_ptr<ClipboardBridge> bridge,
                           QObject* parent = nullptr);

private slots:
    void onClipboardChanged();
    void onPollTick();

private:
    // How often we look for a remote clipboard parked in the bridge.
    // 100ms keeps paste-after-copy latency imperceptible at zero
    // meaningful CPU cost (one mutex-protected pointer check).
    static constexpr int POLL_INTERVAL_MS = 100;

    std::shared_ptr<ClipboardBridge> bridge_;
    QTimer     poll_;
    QByteArray last_payload_;    // last UTF-8 text applied or sent
    bool       applying_ = false;  // set while we write the clipboard ourselves
};

} // namespace vivora::gui
