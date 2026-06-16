#include "app/gui/cloud_client.h"

#include "common/utils/log.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>

namespace vivora::gui {

CloudClient::CloudClient(QObject* parent) : QObject(parent) {
    // Diagnostic: confirm a TLS backend is actually available before any HTTPS.
    log::info("Cloud", "TLS: supportsSsl=%d active='%s' available=[%s] lib='%s'",
              QSslSocket::supportsSsl() ? 1 : 0,
              QSslSocket::activeBackend().toUtf8().constData(),
              QSslSocket::availableBackends().join(',').toUtf8().constData(),
              QSslSocket::sslLibraryVersionString().toUtf8().constData());
}

void CloudClient::setBaseUrl(const QString& url) {
    baseUrl_ = url;
    while (baseUrl_.endsWith('/')) baseUrl_.chop(1);
}

void CloudClient::setToken(const QString& token) { token_ = token; }

void CloudClient::signup(const QString& email, const QString& password) {
    postAuth("/signup", email, password);
}

void CloudClient::login(const QString& email, const QString& password) {
    postAuth("/login", email, password);
}

void CloudClient::postAuth(const QString& path, const QString& email,
                           const QString& password) {
    if (baseUrl_.isEmpty()) {
        emit authFailed("No account server configured");
        return;
    }
    QJsonObject body;
    body["email"] = email;
    body["password"] = password;

    QNetworkRequest req{QUrl(baseUrl_ + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    log::info("Cloud", "POST %s", (baseUrl_ + path).toUtf8().constData());
    QNetworkReply* reply = nam_.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        log::info("Cloud", "auth reply: status=%d netErr='%s'", status,
                  reply->errorString().toUtf8().constData());
        const QByteArray data = reply->readAll();
        const QJsonObject obj = QJsonDocument::fromJson(data).object();
        if (status == 200 || status == 201) {
            emit authSucceeded(obj.value("token").toString(),
                               obj.value("user_id").toString(),
                               obj.value("email").toString());
        } else {
            QString msg = obj.value("error").toString();
            if (msg.isEmpty()) {
                msg = status > 0 ? QString("server error (%1)").arg(status)
                                 : reply->errorString();
            }
            emit authFailed(msg);
        }
    });
}

void CloudClient::fetchLicense() {
    if (token_.isEmpty()) {
        emit licenseError("Not signed in");
        return;
    }
    if (baseUrl_.isEmpty()) {
        emit licenseError("No account server configured");
        return;
    }
    QNetworkRequest req{QUrl(baseUrl_ + "/license")};
    req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());

    QNetworkReply* reply = nam_.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        if (status == 200 && data.size() == 95) {
            emit licenseFetched(data);
        } else if (status == 404) {
            emit licenseUnavailable();
        } else {
            log::warn("Cloud", "fetchLicense failed: status=%d size=%lld",
                      status, static_cast<long long>(data.size()));
            emit licenseError(status > 0 ? QString("server error (%1)").arg(status)
                                         : reply->errorString());
        }
    });
}

} // namespace vivora::gui
