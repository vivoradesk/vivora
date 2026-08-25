// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <vector>

class QNetworkReply;

namespace vivora::gui {

// Thin async HTTP client for the vivora-cloud account/license API (VIV-31)
// and the personal device-mesh API (VIV-52).  All calls are non-blocking;
// results come back as signals on the GUI thread.  The bearer token is held
// by the caller (AppController) and persisted in Settings.
class CloudClient : public QObject {
    Q_OBJECT
public:
    explicit CloudClient(QObject* parent = nullptr);
    ~CloudClient() override;

    void setBaseUrl(const QString& url);     // e.g. https://cloud.vivora.dev
    void setToken(const QString& token);     // short-lived bearer access token
    bool hasToken() const { return !token_.isEmpty(); }
    // VIV-138: the long-lived refresh token, traded for a fresh access token
    // whenever one expires.  Persisted by the caller alongside the access token.
    void setRefreshToken(const QString& token);
    bool hasRefreshToken() const { return !refreshToken_.isEmpty(); }

    void signup(const QString& email, const QString& password);
    void login(const QString& email, const QString& password);
    void fetchLicense();                     // GET /license → licenseFetched / …

    // ── Device mesh (VIV-52) ────────────────────────────────────────────
    // Upsert this install in the account's device list.  201 (new) / 200
    // (existing) → deviceRegistered(id, keyChanged); any error → deviceError.
    void registerDevice(const QString& deviceId, const QString& name,
                        const QString& os, const QString& pubkeyHex,
                        const QString& peerCode);
    // Presence ping.  204 on success; a 404 means the server forgot this
    // install (e.g. after a wipe) → deviceUnknown() so the caller re-registers.
    void heartbeat(const QString& deviceId, const QString& peerCode);
    // GET /devices/me?device_id=<self> → devicesFetched(devices[]) / devicesError.
    // selfDeviceId lets the server flag the current row (is_current).
    void fetchDevices(const QString& selfDeviceId);
    // DELETE /devices/{id} → deviceDeleted(id) on 204/404, deviceError otherwise.
    void deleteDevice(const QString& deviceId);

    // Server-Sent Events subscription to /devices/stream.  Emits devicesChanged
    // on every "devices_changed" event, auto-reconnecting with backoff on drop.
    // Idempotent; a no-op without a token.  Call stopDeviceStream() to tear down.
    void startDeviceStream();
    void stopDeviceStream();

signals:
    // token = bearer access token, refreshToken = its renewal credential (empty
    // if the server is too old to issue one); userId for the checkout link;
    // email echoed back.
    void authSucceeded(const QString& token, const QString& refreshToken,
                       const QString& userId, const QString& email);
    void authFailed(const QString& message);

    // VIV-138.  tokensRenewed: a 401 was recovered by trading the refresh token
    // for a new access token — persist both, nothing else changes.
    // sessionExpired: authentication is gone for good (no refresh token, or the
    // refresh token itself was rejected).  Emitted at most once per token, and
    // never for a transport failure — an offline client is not a signed-out one.
    void tokensRenewed(const QString& token, const QString& refreshToken);
    void sessionExpired();

    void licenseFetched(const QByteArray& token);   // raw 95-byte license
    void licenseUnavailable();                       // 404 — account has no Pro
    void licenseError(const QString& message);

    // Device mesh (VIV-52).
    void devicesFetched(const QJsonArray& devices);
    void devicesError(const QString& message);
    void deviceRegistered(const QString& deviceId, bool keyChanged);
    void deviceDeleted(const QString& deviceId);
    void deviceUnknown();                            // heartbeat 404 → re-register
    void deviceRevoked();                            // heartbeat 410 → membership revoked → sign out
    void deviceError(const QString& message);        // register/delete failure
    void devicesChanged();                           // SSE "devices_changed"

private:
    // wantRefresh=false is the retry for a server that predates VIV-138 and
    // rejects the unknown "refresh" field outright (self-hosted deployments).
    void postAuth(const QString& path, const QString& email,
                  const QString& password, bool wantRefresh = true);

    // Authenticated request plumbing (VIV-138).  `make` builds and sends the
    // reply — it is called again on retry, so it must read token_ at call time
    // rather than capturing it.  `done` receives the outcome exactly once.
    using RequestFn = std::function<QNetworkReply*()>;
    using DoneFn    = std::function<void(int status, const QByteArray& data,
                                         const QString& netError)>;
    void sendAuthorized(RequestFn make, DoneFn done, bool retried = false);
    // Trades refreshToken_ for a fresh access token, then runs `then(ok)`.
    // Concurrent callers coalesce onto the one in-flight POST /refresh.
    void renewTokens(std::function<void(bool)> then);
    void onAuthLost();                               // emit sessionExpired once

    // SSE (device stream) internals.
    void openDeviceStream();                         // (re)opens the streaming GET
    void onDeviceStreamData();                       // drains readyRead into events
    void scheduleDeviceStreamReconnect();            // backoff, then openDeviceStream

    QNetworkAccessManager nam_;
    QString baseUrl_;
    QString token_;
    QString refreshToken_;

    // Renewal state.  authLost_ latches once sessionExpired() has been emitted
    // so a burst of 401s (license + devices + register + stream) reports it
    // once; setToken() clears it.
    bool renewInFlight_ = false;
    bool authLost_      = false;
    std::vector<std::function<void(bool)>> renewWaiters_;

    // Device-stream state.  sseWanted_ stays true between startDeviceStream()
    // and stopDeviceStream(), so an unexpected drop triggers a reconnect but a
    // deliberate stop / sign-out does not.
    QNetworkReply* sseReply_ = nullptr;
    QByteArray     sseBuffer_;
    QTimer         sseReconnect_;
    bool           sseWanted_    = false;
    bool           sseRetried_   = false;            // one refresh+reopen per 401
    int            sseBackoffMs_ = 1000;             // grows to sseBackoffMaxMs_
    static constexpr int sseBackoffMaxMs_ = 30000;
};

} // namespace vivora::gui
