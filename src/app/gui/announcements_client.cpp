// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/gui/announcements_client.h"

#include "common/utils/log.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace vivora::gui {

AnnouncementsClient::AnnouncementsClient(QObject* parent) : QObject(parent) {}

void AnnouncementsClient::fetch(const QString& url) {
    if (url.isEmpty()) return;
    QNetworkReply* reply = nam_.get(QNetworkRequest{QUrl(url)});
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200) {
            log::info("Announce", "fetch skipped: status=%d", status);
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonArray arr = obj.value("announcements").toArray();
        QVariantList out;
        for (const auto& v : arr) out.append(v.toObject().toVariantMap());
        emit fetched(out);
    });
}

} // namespace vivora::gui
