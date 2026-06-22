#include "app/gui/polls_client.h"

#include "common/utils/log.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace vivora::gui {

PollsClient::PollsClient(QObject* parent) : QObject(parent) {}

void PollsClient::setBaseUrl(const QString& url) {
    baseUrl_ = url;
    while (baseUrl_.endsWith('/')) baseUrl_.chop(1);
}

void PollsClient::setToken(const QString& token) { token_ = token; }

void PollsClient::fetch() {
    if (baseUrl_.isEmpty()) return;
    QNetworkReply* reply = nam_.get(QNetworkRequest{QUrl(baseUrl_ + "/polls")});
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        QVariantList out;
        for (const auto& v : obj.value("polls").toArray()) out.append(v.toObject().toVariantMap());
        emit fetched(out);
    });
}

void PollsClient::respond(const QString& id, const QVariantMap& body) {
    if (baseUrl_.isEmpty()) return;
    QNetworkRequest req{QUrl(baseUrl_ + "/polls/" + id + "/respond")};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!token_.isEmpty()) req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
    const QByteArray payload =
        QJsonDocument(QJsonObject::fromVariantMap(body)).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = nam_.post(req, payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, id]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = (status == 204 || status == 200);
        if (!ok) log::warn("Polls", "respond %s failed: status=%d", id.toUtf8().constData(), status);
        emit responded(id, ok);
    });
}

void PollsClient::fetchResults(const QString& id) {
    if (baseUrl_.isEmpty()) return;
    QNetworkReply* reply = nam_.get(QNetworkRequest{QUrl(baseUrl_ + "/polls/" + id + "/results")});
    connect(reply, &QNetworkReply::finished, this, [this, reply, id]() {
        reply->deleteLater();
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        emit results(id, obj.toVariantMap());
    });
}

} // namespace vivora::gui
