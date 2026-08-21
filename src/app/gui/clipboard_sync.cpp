// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/gui/clipboard_sync.h"

#include "common/utils/log.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QString>

#include <cstring>

namespace vivora::gui {

ClipboardSync::ClipboardSync(std::shared_ptr<ClipboardBridge> bridge,
                             QObject* parent)
    : QObject(parent), bridge_(std::move(bridge)) {
    if (QClipboard* cb = QGuiApplication::clipboard()) {
        connect(cb, &QClipboard::dataChanged,
                this, &ClipboardSync::onClipboardChanged);
    }
    connect(&poll_, &QTimer::timeout, this, &ClipboardSync::onPollTick);
    poll_.start(POLL_INTERVAL_MS);
}

void ClipboardSync::onClipboardChanged() {
    if (!bridge_) return;
    QClipboard* cb = QGuiApplication::clipboard();
    if (!cb) return;

    // Text only for VIV-22.  QClipboard::text() is empty for both an empty
    // clipboard and a non-text payload (image, files) — neither is sent.
    const QString text = cb->text(QClipboard::Clipboard);
    if (text.isEmpty()) return;

    // Normalise to UTF-8 for the wire.
    const QByteArray utf8 = text.toUtf8();

    // Echo suppression: the change we caused ourselves (applying a remote
    // clipboard) or a copy of identical content must not (re-)broadcast.
    if (utf8 == last_payload_) {
        applying_ = false;
        return;
    }
    if (applying_) {
        // Our own setText fired but the content already differs (user copied
        // during the round-trip) — treat the new content as a fresh local copy.
        applying_ = false;
    }

    if (static_cast<size_t>(utf8.size()) > protocol::ClipboardMessage::MAX_PAYLOAD) {
        log::debug("ClipboardSync", "Clipboard text too large to sync (%lld > %zu bytes)",
                   static_cast<long long>(utf8.size()),
                   protocol::ClipboardMessage::MAX_PAYLOAD);
        return;
    }

    last_payload_ = utf8;

    protocol::ClipboardMessage msg;
    msg.mime = protocol::ClipboardMessage::text_mime();
    msg.data.assign(utf8.constData(), utf8.constData() + utf8.size());
    bridge_->push_outbound(std::move(msg));
}

void ClipboardSync::onPollTick() {
    if (!bridge_) return;
    protocol::ClipboardMessage msg;
    if (!bridge_->take_inbound(msg)) return;

    // Text only — anything else is a future ticket (files VIV-39, images TBD).
    if (msg.mime.rfind("text/plain", 0) != 0) return;
    if (msg.data.empty()) return;

    const QByteArray utf8(reinterpret_cast<const char*>(msg.data.data()),
                          static_cast<int>(msg.data.size()));
    if (utf8 == last_payload_) return;   // already have this content

    QClipboard* cb = QGuiApplication::clipboard();
    if (!cb) return;

    // Remember what we're about to apply BEFORE setText — on some platforms
    // dataChanged fires synchronously from inside setText.
    last_payload_ = utf8;
    applying_     = true;
    cb->setText(QString::fromUtf8(utf8), QClipboard::Clipboard);
    log::info("ClipboardSync", "Applied remote clipboard (%d bytes)", utf8.size());
}

} // namespace vivora::gui
