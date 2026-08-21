// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace vivora::gui {

// Talks to the polls API (VIV-71): fetch the feed, submit a response, and pull
// aggregate results.  Filtering (answered/target) is AppController's job.
// Silent on any network/parse error — a poll must never block startup.
class PollsClient : public QObject {
    Q_OBJECT
public:
    explicit PollsClient(QObject* parent = nullptr);

    void setBaseUrl(const QString& url);   // e.g. https://cloud.vivora.dev
    void setToken(const QString& token);   // optional bearer (clean dedup when logged in)

    void fetch();                                       // GET /polls
    void respond(const QString& id, const QVariantMap& body);  // POST /polls/{id}/respond
    void fetchResults(const QString& id);               // GET /polls/{id}/results

signals:
    void fetched(const QVariantList& polls);
    void responded(const QString& id, bool ok);
    void results(const QString& id, const QVariantMap& results);

private:
    QNetworkAccessManager nam_;
    QString baseUrl_;
    QString token_;
};

} // namespace vivora::gui
