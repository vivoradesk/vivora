#pragma once

#include "app/gui/address_book.h"
#include "app/gui/cloud_client.h"
#include "app/gui/settings.h"
#include "app/gui/update_checker.h"
#include "host/session/host_approval_gate.h"

#include <QHash>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTimer>
#include <memory>
#include <vector>

namespace vivora::gui {

class Tray;
class HostWorker;
class ViewSession;

// Top-level QML bridge.  Owns the Settings, AddressBook, Tray and (in
// Phase A.1) the actual host / view session workers.  Exposed to QML as
// the singleton `App`.
//
// Phase A scope: state machine + identity load + tray wiring.  startSharing
// / connectToPeer just flip a bool for UI smoke testing — they do not yet
// spin up host_loop or view_loop.  Wiring comes in Phase A.1.
class AppController : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool    sharing       READ sharing       NOTIFY sharingChanged)
    Q_PROPERTY(int     clientCount   READ clientCount   NOTIFY clientCountChanged)
    Q_PROPERTY(QString myPeerCode    READ myPeerCode    NOTIFY identityChanged)
    Q_PROPERTY(QString myPubkeyHex   READ myPubkeyHex   NOTIFY identityChanged)
    Q_PROPERTY(int     activeViews   READ activeViews   NOTIFY activeViewsChanged)
    Q_PROPERTY(vivora::gui::Settings* settings    READ settings    CONSTANT)
    Q_PROPERTY(vivora::gui::AddressBook* peers    READ peers       CONSTANT)
    // License status (VIV-29) — verified offline against the embedded
    // Vivora public key.  licensePro gates the "Pro" badge + commercial use
    // (managed relay); licenseTier / licenseExpiry drive Settings → About.
    Q_PROPERTY(bool    licenseValid  READ licenseValid  NOTIFY licenseChanged)
    Q_PROPERTY(bool    licensePro    READ licensePro    NOTIFY licenseChanged)
    Q_PROPERTY(QString licenseTier   READ licenseTier   NOTIFY licenseChanged)
    Q_PROPERTY(QString licenseExpiry READ licenseExpiry NOTIFY licenseChanged)
    // VIV-31 account: signed-in email + whether we have a session.  License
    // is fetched from the cloud automatically once signed in.
    Q_PROPERTY(QString accountEmail    READ accountEmail    NOTIFY accountChanged)
    Q_PROPERTY(bool    accountLoggedIn READ accountLoggedIn NOTIFY accountChanged)
    // VIV-69 update check: a build newer than VIVORA_VERSION is published.
    Q_PROPERTY(bool    updateAvailable READ updateAvailable NOTIFY updateChanged)
    Q_PROPERTY(QString updateVersion   READ updateVersion   NOTIFY updateChanged)
    Q_PROPERTY(QString updateNotes     READ updateNotes     NOTIFY updateChanged)

public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    bool    sharing() const     { return sharing_; }
    int     clientCount() const { return clientCount_; }
    QString myPeerCode() const  { return myPeerCode_; }
    QString myPubkeyHex() const { return myPubkeyHex_; }
    int     activeViews() const { return activeViews_; }
    Settings*    settings() const { return settings_.get(); }
    AddressBook* peers()    const { return peers_.get(); }

    bool    licenseValid() const  { return licenseValid_; }
    bool    licensePro()   const  { return licensePro_; }
    QString licenseTier()  const  { return licensePro_ ? QStringLiteral("Pro")
                                          : QStringLiteral("Free"); }
    QString licenseExpiry() const { return licenseExpiry_; }

    QString accountEmail()    const { return accountEmail_; }
    bool    accountLoggedIn() const { return !accountEmail_.isEmpty(); }

    bool    updateAvailable() const { return updateAvailable_; }
    QString updateVersion()   const { return updateVersion_; }
    QString updateNotes()     const { return updateNotes_; }

    // Wired from main.cpp at app init.  AppController borrows the tray
    // pointer; ownership stays with gui_main().
    void setTray(Tray* tray);

public slots:
    void startSharing();
    void stopSharing();
    void connectToPeer(const QString& peerCodeOrHex);
    void disconnectView(int viewId);
    void openSettings();
    // Asks for confirmation if there are active sessions, then exits the
    // QApplication.  Hooked to the tray "Quit" entry and to the QML
    // settings dialog Quit button.
    void quit();
    // Brings the main window back from the tray.  Wired from QML.
    void showMainWindow();

    // VIV-53 connection approval — QML calls these from the
    // ConnectionApprovalDialog buttons.  `key` is the per-client
    // identifier the controller surfaced via connectionApprovalRequested.
    // `remember` (VIV-61) pins the viewer as trusted so future connects
    // from the same key auto-accept ("don't ask again").  input/clipboard/
    // fileTransfer (VIV-60) are the per-connection capability grants from
    // the dialog toggles.
    Q_INVOKABLE void approveConnection(const QString& key, bool remember = false,
                                       bool input = true, bool clipboard = true,
                                       bool fileTransfer = false);
    Q_INVOKABLE void rejectConnection(const QString& key);

    // Force an immediate rendezvous re-registration.  QML calls this
    // from the Refresh button in the sharing card.
    Q_INVOKABLE void refreshRendezvous();

    // VIV-29: import a license token file — copies it next to the config as
    // license.bin, points the setting at it and re-verifies.  Accepts a
    // plain path or a file:// URL (from the QML file dialog).
    Q_INVOKABLE void importLicense(const QString& pathOrUrl);
    // Re-read + verify the configured license file.  Called on startup and
    // whenever the license path changes.
    Q_INVOKABLE void refreshLicense();

    // VIV-31 account actions (call the vivora-cloud API).
    Q_INVOKABLE void signUp(const QString& email, const QString& password);
    Q_INVOKABLE void logIn(const QString& email, const QString& password);
    Q_INVOKABLE void logOut();
    // Re-pull the license from the cloud for the signed-in account.
    Q_INVOKABLE void refreshLicenseFromCloud();
    // Open the Pro checkout page (website) in the browser, passing the
    // account id so Paddle binds the subscription to it.
    Q_INVOKABLE void openUpgradePage();
    // Open the releases/download page for the available update (VIV-69).
    Q_INVOKABLE void openDownloadPage();

signals:
    void sharingChanged();
    void clientCountChanged();
    void identityChanged();
    void activeViewsChanged();
    // Open the settings dialog.  Connected to QML which loads
    // SettingsDialog.qml on demand.
    void settingsRequested();
    // Bring the window to the foreground.
    void showWindowRequested();
    // License status changed (loaded / imported / expired).
    void licenseChanged();
    // Account state changed (signed in / out).
    void accountChanged();
    // A newer build is available (VIV-69) — QML shows the update banner.
    void updateChanged();
    // Account action failed — QML shows the message inline in the form.
    void accountError(const QString& message);
    // VIV-53: new client awaiting approval.  QML shows
    // ConnectionApprovalDialog with these details.  key is a
    // stringified address used to identify the client when the user
    // clicks Accept / Reject.  recognized/seenCount (VIV-61) drive the
    // dialog's "recognized key · seen N times" vs "new key" trust card.
    void connectionApprovalRequested(QString key,
                                     QString peerCode,
                                     QString pubkeyHex,
                                     QString ipPort,
                                     bool    recognized,
                                     int     seenCount,
                                     QString deviceName);

private:
    void loadIdentity();        // populates myPeerCode_ + myPubkeyHex_

    std::unique_ptr<Settings>    settings_;
    std::unique_ptr<AddressBook> peers_;
    std::unique_ptr<HostWorker>  hostWorker_;
    // Shared with the HostWorker thread.  Holds per-client approval
    // state and the callback we wire to bounce notifications back into
    // the GUI thread (where QML can show the approval dialog).
    std::shared_ptr<vivora::host::HostApprovalGate> approvalGate_;
    // Pending approval prompts: key -> (pubkeyHex, peerCode).  Lets
    // approveConnection() record/pin the viewer in the address book once
    // the user decides (VIV-61).  Cleared on approve/reject.
    QHash<QString, QPair<QString, QString>> pendingApprovals_;
    // One ViewSession per "Connect to peer" click.  Owned here so the
    // stream window survives even when QML drops its reference.
    std::vector<std::unique_ptr<ViewSession>> viewSessions_;
    Tray*                        tray_ = nullptr;

    // Polls hostWorker_'s atomic counters into Q_PROPERTYs.  Cheap
    // (~3 atomic loads per tick); 500ms is fast enough that the tray
    // tooltip + UI feel responsive without burning a thread.
    QTimer  pollTimer_;

    bool    sharing_     = false;
    int     clientCount_ = 0;
    int     activeViews_ = 0;
    QString myPeerCode_;
    QString myPubkeyHex_;

    // VIV-29 license status (verified offline).
    bool    licenseValid_ = false;
    bool    licensePro_   = false;
    QString licenseExpiry_;

    // VIV-31 account/cloud.
    CloudClient cloud_;
    QString     accountEmail_;
    QString     accountUserId_;
    void        wireCloud();

    // VIV-69 update check (notify-only).
    UpdateChecker update_;
    bool          updateAvailable_ = false;
    QString       updateVersion_;
    QString       updateUrl_;
    QString       updateNotes_;
    void          wireUpdate();
};

} // namespace vivora::gui
