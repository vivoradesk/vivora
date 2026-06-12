#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

namespace vivora::gui {

// Thin async HTTP client for the vivora-cloud account/license API (VIV-31).
// All calls are non-blocking; results come back as signals on the GUI thread.
// Only the handful of endpoints the desktop client needs: signup, login, and
// license retrieval.  The bearer token is held by the caller (AppController)
// and persisted in Settings.
class CloudClient : public QObject {
    Q_OBJECT
public:
    explicit CloudClient(QObject* parent = nullptr);

    void setBaseUrl(const QString& url);     // e.g. https://cloud.vivora.dev
    void setToken(const QString& token);     // bearer session token
    bool hasToken() const { return !token_.isEmpty(); }

    void signup(const QString& email, const QString& password);
    void login(const QString& email, const QString& password);
    void fetchLicense();                     // GET /license → licenseFetched / …

signals:
    // token = bearer session; userId for the checkout link; email echoed back.
    void authSucceeded(const QString& token, const QString& userId, const QString& email);
    void authFailed(const QString& message);

    void licenseFetched(const QByteArray& token);   // raw 95-byte license
    void licenseUnavailable();                       // 404 — account has no Pro
    void licenseError(const QString& message);

private:
    void postAuth(const QString& path, const QString& email, const QString& password);

    QNetworkAccessManager nam_;
    QString baseUrl_;
    QString token_;
};

} // namespace vivora::gui
