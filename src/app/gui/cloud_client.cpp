// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/gui/cloud_client.h"

#include "common/utils/log.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QUrl>

#include <algorithm>

namespace vivora::gui {

CloudClient::CloudClient(QObject* parent) : QObject(parent) {
    // Diagnostic: confirm a TLS backend is actually available before any HTTPS.
    log::info("Cloud", "TLS: supportsSsl=%d active='%s' available=[%s] lib='%s'",
              QSslSocket::supportsSsl() ? 1 : 0,
              QSslSocket::activeBackend().toUtf8().constData(),
              QSslSocket::availableBackends().join(',').toUtf8().constData(),
              QSslSocket::sslLibraryVersionString().toUtf8().constData());

    // SSE reconnect timer: single-shot, re-armed by scheduleDeviceStreamReconnect.
    sseReconnect_.setSingleShot(true);
    connect(&sseReconnect_, &QTimer::timeout, this, [this] {
        if (sseWanted_) openDeviceStream();
    });
}

CloudClient::~CloudClient() {
    // Delegate to the safe teardown.  Do NOT inline `sseReply_->abort();
    // sseReply_->deleteLater();` here: abort() synchronously emits finished(),
    // whose handler (openDeviceStream) nulls sseReply_ — so a following
    // sseReply_->deleteLater() would run on a null pointer and segfault on
    // shutdown.  stopDeviceStream() nulls the member *before* abort(), which
    // makes the reentrant handler a no-op.
    stopDeviceStream();
}

void CloudClient::setBaseUrl(const QString& url) {
    baseUrl_ = url;
    while (baseUrl_.endsWith('/')) baseUrl_.chop(1);
}

void CloudClient::setToken(const QString& token) {
    token_ = token;
    // A new access token means authentication is live again: re-arm the
    // one-shot sessionExpired() latch (VIV-138).
    authLost_ = false;
}

void CloudClient::setRefreshToken(const QString& token) { refreshToken_ = token; }

// ── authenticated request plumbing (VIV-138) ────────────────────────────────
//
// Every authenticated call goes through sendAuthorized() so a 401 has exactly
// one meaning in exactly one place: try the refresh token once, replay the
// request with the new access token, and only give up (sessionExpired) if the
// refresh itself is rejected.  Before this, a 401 was folded into each call's
// generic error signal — the app kept its "signed in" state, showed an empty
// device list, and retried forever without ever telling the user.

void CloudClient::sendAuthorized(RequestFn make, DoneFn done, bool retried) {
    QNetworkReply* reply = make();
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, make, done, retried]() {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data    = reply->readAll();
        const QString    netErr  = reply->errorString();

        if (status != 401) { done(status, data, netErr); return; }

        // One renewal attempt per request.  `make` re-reads token_, so the
        // replay automatically carries the token we just obtained.
        if (!retried && !refreshToken_.isEmpty()) {
            renewTokens([this, make, done](bool ok) {
                if (ok) { sendAuthorized(make, done, /*retried=*/true); return; }
                // Renewal failed.  onAuthLost() has already reported it (or the
                // failure was transport-level); report the original 401 to the
                // caller so its own state — spinners, in-flight flags — unwinds.
                done(401, QByteArray(), QStringLiteral("session expired"));
            });
            return;
        }
        onAuthLost();
        done(status, data, netErr);
    });
}

void CloudClient::renewTokens(std::function<void(bool)> then) {
    if (refreshToken_.isEmpty() || baseUrl_.isEmpty()) {
        onAuthLost();
        then(false);
        return;
    }
    renewWaiters_.push_back(std::move(then));
    if (renewInFlight_) return;              // coalesce a burst onto one POST
    renewInFlight_ = true;

    QJsonObject body;
    body["refresh_token"] = refreshToken_;

    QNetworkRequest req{QUrl(baseUrl_ + "/refresh")};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply* reply =
        nam_.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        const QString access  = obj.value("token").toString();

        bool ok = false;
        if (status == 200 && !access.isEmpty()) {
            setToken(access);                       // also clears authLost_
            const QString fresh = obj.value("refresh_token").toString();
            if (!fresh.isEmpty()) refreshToken_ = fresh;
            log::info("Cloud", "access token renewed");
            emit tokensRenewed(token_, refreshToken_);
            ok = true;
        } else if (status > 0 && status != 401 && status < 500) {
            // A 4xx that isn't 401 means we asked wrongly, not that the session
            // died; treat it as a soft failure and let the caller's error path
            // run.  5xx and transport errors are transient by definition.
            log::warn("Cloud", "token refresh: unexpected status=%d", status);
        } else if (status == 401) {
            log::warn("Cloud", "refresh token rejected — session is over");
            refreshToken_.clear();
            onAuthLost();
        } else {
            log::warn("Cloud", "token refresh failed: %s",
                      reply->errorString().toUtf8().constData());
        }

        renewInFlight_ = false;
        // Swap the queue out first: a waiter may start a request that queues a
        // new waiter, and we must not run that one against this same result.
        std::vector<std::function<void(bool)>> waiters;
        waiters.swap(renewWaiters_);
        for (auto& w : waiters) w(ok);
    });
}

void CloudClient::onAuthLost() {
    if (authLost_) return;                   // report once per token
    authLost_ = true;
    log::warn("Cloud", "session is no longer valid — sign-in required");
    emit sessionExpired();
}

void CloudClient::signup(const QString& email, const QString& password) {
    postAuth("/signup", email, password);
}

void CloudClient::login(const QString& email, const QString& password) {
    postAuth("/login", email, password);
}

void CloudClient::postAuth(const QString& path, const QString& email,
                           const QString& password, bool wantRefresh) {
    if (baseUrl_.isEmpty()) {
        emit authFailed("No account server configured");
        return;
    }
    QJsonObject body;
    body["email"] = email;
    body["password"] = password;
    // VIV-138: ask for the access+refresh pair.  A server that predates the
    // flag rejects the unknown field with a 400, so the reply handler retries
    // once without it: an old self-hosted cloud still signs us in, just with a
    // legacy long session and no way to renew it.
    if (wantRefresh) body["refresh"] = true;

    QNetworkRequest req{QUrl(baseUrl_ + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    log::info("Cloud", "POST %s", (baseUrl_ + path).toUtf8().constData());
    QNetworkReply* reply = nam_.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, path, email, password, wantRefresh]() {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        log::info("Cloud", "auth reply: status=%d netErr='%s'", status,
                  reply->errorString().toUtf8().constData());
        const QByteArray data = reply->readAll();
        const QJsonObject obj = QJsonDocument::fromJson(data).object();
        if (status == 400 && wantRefresh) {
            log::info("Cloud",
                      "server rejected the refresh request - retrying without it");
            postAuth(path, email, password, /*wantRefresh=*/false);
            return;
        }
        if (status == 200 || status == 201) {
            emit authSucceeded(obj.value("token").toString(),
                               obj.value("refresh_token").toString(),
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
    sendAuthorized([this] {
        QNetworkRequest req{QUrl(baseUrl_ + "/license")};
        req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
        return nam_.get(req);
    }, [this](int status, const QByteArray& data, const QString& netErr) {
        if (status == 200 && data.size() == 95) {
            emit licenseFetched(data);
        } else if (status == 404) {
            emit licenseUnavailable();
        } else {
            log::warn("Cloud", "fetchLicense failed: status=%d size=%lld",
                      status, static_cast<long long>(data.size()));
            emit licenseError(status > 0 ? QString("server error (%1)").arg(status)
                                         : netErr);
        }
    });
}

// ── Device mesh (VIV-52) ─────────────────────────────────────────────────────

void CloudClient::registerDevice(const QString& deviceId, const QString& name,
                                 const QString& os, const QString& pubkeyHex,
                                 const QString& peerCode) {
    if (token_.isEmpty() || baseUrl_.isEmpty()) {
        emit deviceError("Not signed in");
        return;
    }
    QJsonObject body;
    body["device_id"] = deviceId;
    body["name"]      = name;
    body["os"]        = os;
    body["pubkey"]    = pubkeyHex;
    body["peer_code"] = peerCode;

    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    sendAuthorized([this, payload] {
        QNetworkRequest req{QUrl(baseUrl_ + "/devices/register")};
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
        return nam_.post(req, payload);
    }, [this](int status, const QByteArray& data, const QString& netErr) {
        if (status == 200 || status == 201) {
            const QJsonObject obj = QJsonDocument::fromJson(data).object();
            emit deviceRegistered(obj.value("device_id").toString(),
                                  obj.value("key_changed").toBool());
        } else {
            log::warn("Cloud", "registerDevice failed: status=%d", status);
            emit deviceError(status > 0 ? QString("server error (%1)").arg(status)
                                        : netErr);
        }
    });
}

void CloudClient::heartbeat(const QString& deviceId, const QString& peerCode) {
    if (token_.isEmpty() || baseUrl_.isEmpty()) return;
    QJsonObject body;
    body["device_id"] = deviceId;
    body["peer_code"] = peerCode;

    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    sendAuthorized([this, payload] {
        QNetworkRequest req{QUrl(baseUrl_ + "/devices/heartbeat")};
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
        return nam_.post(req, payload);
    }, [this](int status, const QByteArray&, const QString&) {
        // 410 = this device was removed from the account (membership revoked)
        // → sign out so the resurrect-on-404 loop never re-registers it.
        // 404 = the server merely forgot this install → re-register.
        if (status == 410)      emit deviceRevoked();
        else if (status == 404) emit deviceUnknown();
    });
}

void CloudClient::fetchDevices(const QString& selfDeviceId) {
    if (token_.isEmpty() || baseUrl_.isEmpty()) {
        emit devicesError("Not signed in");
        return;
    }
    QUrl url(baseUrl_ + "/devices/me");
    if (!selfDeviceId.isEmpty()) url.setQuery("device_id=" + selfDeviceId);

    sendAuthorized([this, url] {
        QNetworkRequest req{url};
        req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
        return nam_.get(req);
    }, [this](int status, const QByteArray& data, const QString& netErr) {
        if (status == 200) {
            const QJsonObject obj = QJsonDocument::fromJson(data).object();
            emit devicesFetched(obj.value("devices").toArray());
        } else {
            log::warn("Cloud", "fetchDevices failed: status=%d", status);
            emit devicesError(status > 0 ? QString("server error (%1)").arg(status)
                                         : netErr);
        }
    });
}

void CloudClient::deleteDevice(const QString& deviceId) {
    if (token_.isEmpty() || baseUrl_.isEmpty()) {
        emit deviceError("Not signed in");
        return;
    }
    sendAuthorized([this, deviceId] {
        QNetworkRequest req{QUrl(baseUrl_ + "/devices/" + deviceId)};
        req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
        return nam_.deleteResource(req);
    }, [this, deviceId](int status, const QByteArray&, const QString& netErr) {
        if (status == 204 || status == 404) {
            emit deviceDeleted(deviceId);   // 404 = already gone; treat as success
        } else {
            log::warn("Cloud", "deleteDevice failed: status=%d", status);
            emit deviceError(status > 0 ? QString("server error (%1)").arg(status)
                                        : netErr);
        }
    });
}

// ── Device stream (Server-Sent Events) ───────────────────────────────────────
//
// A single long-lived GET to /devices/stream.  The server pushes
//   data: {"type":"devices_changed","reason":"..."}\n\n
// on any change to this account's device list, and ": keepalive\n" comments
// every ~25s.  We keep the QNetworkReply alive, accumulate readyRead into
// sseBuffer_, split on the blank-line event boundary, and parse each event's
// `data:` lines.  On any drop we reconnect with capped exponential backoff.

void CloudClient::startDeviceStream() {
    if (token_.isEmpty() || baseUrl_.isEmpty()) return;
    if (sseWanted_ && sseReply_) return;   // already streaming
    sseWanted_    = true;
    sseRetried_   = false;
    sseBackoffMs_ = 1000;
    openDeviceStream();
}

void CloudClient::stopDeviceStream() {
    sseWanted_ = false;
    sseReconnect_.stop();
    if (sseReply_) {
        QNetworkReply* r = sseReply_;
        sseReply_ = nullptr;
        r->abort();
        r->deleteLater();
    }
    sseBuffer_.clear();
}

void CloudClient::openDeviceStream() {
    if (!sseWanted_ || token_.isEmpty() || baseUrl_.isEmpty()) return;
    if (sseReply_) return;                 // a reply is already live

    QNetworkRequest req{QUrl(baseUrl_ + "/devices/stream")};
    req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
    req.setRawHeader("Accept", "text/event-stream");
    // Long-poll: no reply timeout, and don't let the manager buffer the whole
    // (never-ending) body before delivering readyRead.
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);
    // Force HTTP/1.1: Qt negotiates HTTP/2 with the TLS reverse proxy by
    // default, and its h2 handling ends an endless SSE response almost
    // immediately (server + proxy hold the stream open fine over 1.1 —
    // confirmed by a raw client). Without this the reply finished ~1ms
    // after the initial ": connected" comment, so the stream reconnected
    // once per second instead of staying open.
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    sseBuffer_.clear();
    sseReply_ = nam_.get(req);
    log::info("Cloud", "device stream: connecting");
    connect(sseReply_, &QNetworkReply::readyRead, this,
            &CloudClient::onDeviceStreamData);
    connect(sseReply_, &QNetworkReply::finished, this, [this] {
        // The stream ended (server closed, network dropped, or we aborted).
        int status = 0;
        if (sseReply_) {
            status = sseReply_->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            sseReply_->deleteLater();
            sseReply_ = nullptr;
        }
        if (!sseWanted_) return;

        // VIV-138: a 401 here used to reconnect once a second forever.  Renew
        // the access token once and reopen; if the renewal is refused, the
        // session is over — stop entirely rather than hammer the server until
        // the user notices something is wrong.
        if (status == 401) {
            if (!sseRetried_ && !refreshToken_.isEmpty()) {
                sseRetried_ = true;          // cleared by a healthy 200 stream
                renewTokens([this](bool ok) {
                    if (!sseWanted_) return;
                    if (ok) { openDeviceStream(); return; }
                    if (authLost_) { sseWanted_ = false; sseReconnect_.stop(); return; }
                    sseRetried_ = false;     // transport hiccup, not an auth failure
                    scheduleDeviceStreamReconnect();
                });
                return;
            }
            onAuthLost();
            sseWanted_ = false;              // startDeviceStream() re-arms it
            sseReconnect_.stop();
            return;
        }
        scheduleDeviceStreamReconnect();
    });
}

void CloudClient::onDeviceStreamData() {
    if (!sseReply_) return;
    // First bytes of a 200 response → the stream is genuinely up; reset the
    // backoff and the one-shot renewal guard.  Checking the status matters:
    // an error response carries a body too, and resetting the backoff on those
    // bytes is exactly what turned one dead session into a 1 Hz reconnect
    // storm — 15,795 connections in 4.5 hours (VIV-138).  Anything else is
    // left for the finished() handler to classify.
    if (sseReply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
        return;
    sseBackoffMs_ = 1000;
    sseRetried_   = false;
    sseBuffer_.append(sseReply_->readAll());

    // Events are separated by a blank line.  Normalise CRLF so the split is
    // uniform, then process every complete event left in the buffer.
    sseBuffer_.replace("\r\n", "\n");
    int sep;
    while ((sep = sseBuffer_.indexOf("\n\n")) >= 0) {
        const QByteArray event = sseBuffer_.left(sep);
        sseBuffer_.remove(0, sep + 2);

        // Concatenate the payload of all `data:` lines (SSE allows several).
        QByteArray payload;
        for (const QByteArray& line : event.split('\n')) {
            if (line.startsWith(':')) continue;               // keepalive comment
            if (line.startsWith("data:")) {
                QByteArray v = line.mid(5);
                if (v.startsWith(' ')) v.remove(0, 1);
                payload.append(v);
            }
        }
        if (payload.isEmpty()) continue;
        const QJsonObject obj = QJsonDocument::fromJson(payload).object();
        if (obj.value("type").toString() == "devices_changed") {
            log::info("Cloud", "device stream: devices_changed (%s)",
                      obj.value("reason").toString().toUtf8().constData());
            emit devicesChanged();
        }
    }
}

void CloudClient::scheduleDeviceStreamReconnect() {
    if (!sseWanted_) return;
    const int delay = sseBackoffMs_;
    sseBackoffMs_ = std::min(sseBackoffMs_ * 2, sseBackoffMaxMs_);
    log::info("Cloud", "device stream: reconnecting in %dms", delay);
    sseReconnect_.start(delay);
}

} // namespace vivora::gui
