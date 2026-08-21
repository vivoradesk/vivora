// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

namespace vivora::gui {

// Fetches a small version manifest (version.json) and reports whether a build
// newer than this one is available.  Stage 1 of the update mechanism (VIV-69):
// notify only — no download / apply.  The manifest shape:
//   { "latest": "0.1.1", "min_supported": "0.1.0",
//     "url": "https://…/releases", "notes": "…" }
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject* parent = nullptr);

    // GET <url> (e.g. https://vivora.dev/version.json) and compare "latest" to
    // the built-in VIVORA_VERSION.  Emits at most one signal; silent on any
    // network/parse error (an update notice must never block startup).
    void check(const QString& url);

signals:
    void updateAvailable(const QString& latest, const QString& url,
                         const QString& notes);
    void upToDate();

private:
    QNetworkAccessManager nam_;
};

} // namespace vivora::gui
