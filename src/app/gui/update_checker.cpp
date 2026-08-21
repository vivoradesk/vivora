// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/gui/update_checker.h"

#include "common/utils/log.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <array>

#ifndef VIVORA_VERSION
#define VIVORA_VERSION "0.0.0"
#endif

namespace vivora::gui {
namespace {

// Compare dotted numeric versions ("0.1.2").  Any "-suffix" (e.g. -beta1) is
// dropped; missing components count as 0.  Returns <0 if a<b, 0 if equal, >0
// if a>b.
int cmp_version(const QString& a, const QString& b) {
    const auto parse = [](const QString& v) {
        std::array<int, 3> out{0, 0, 0};
        const QStringList parts = v.section('-', 0, 0).split('.');
        for (int i = 0; i < 3 && i < parts.size(); ++i) out[i] = parts[i].toInt();
        return out;
    };
    const auto va = parse(a), vb = parse(b);
    for (int i = 0; i < 3; ++i)
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    return 0;
}

} // namespace

UpdateChecker::UpdateChecker(QObject* parent) : QObject(parent) {}

void UpdateChecker::check(const QString& url) {
    if (url.isEmpty()) return;
    QNetworkReply* reply = nam_.get(QNetworkRequest{QUrl(url)});
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200) {
            log::info("Update", "version check skipped: status=%d (%s)", status,
                      reply->errorString().toUtf8().constData());
            return;
        }
        const QJsonObject obj =
            QJsonDocument::fromJson(reply->readAll()).object();
        const QString latest = obj.value("latest").toString();
        if (latest.isEmpty()) return;

        const QString current = QStringLiteral(VIVORA_VERSION);
        if (cmp_version(current, latest) < 0) {
            log::info("Update", "newer version available: %s (have %s)",
                      latest.toUtf8().constData(), current.toUtf8().constData());
            emit updateAvailable(latest, obj.value("url").toString(),
                                 obj.value("notes").toString());
        } else {
            log::info("Update", "up to date (%s, latest %s)",
                      current.toUtf8().constData(), latest.toUtf8().constData());
            emit upToDate();
        }
    });
}

} // namespace vivora::gui
