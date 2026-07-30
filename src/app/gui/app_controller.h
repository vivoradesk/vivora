#pragma once

#include "app/clipboard_bridge.h"
#include "app/gui/address_book.h"
#include "app/gui/announcements_client.h"
#include "app/gui/cloud_client.h"
#include "app/gui/device_mesh_model.h"
#include "app/gui/polls_client.h"
#include "app/gui/settings.h"
#include "app/gui/update_checker.h"
#include "host/session/host_approval_gate.h"

#include <QHash>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QTimer>
#include <memory>
#include <vector>

namespace vivora::gui {

class Tray;
class HostWorker;
class NetworkChangeWatcher;
class ViewSession;
class ClipboardSync;

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
    // VIV-23: this device's key fingerprint (canonical short form) —
    // read-only, shown in Settings for out-of-band comparison.
    Q_PROPERTY(QString myFingerprint READ myFingerprint NOTIFY identityChanged)
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
    // VIV-52 device mesh.  myDevices is the account's live device list; the
    // "My Devices" UI binds to it when signed in.  deviceMeshTier gates the
    // surface ("pro" when Pro-licensed, else "free"); meshRefreshing drives the
    // cached/UPDATING… state while a /devices/me fetch is in flight.
    Q_PROPERTY(vivora::gui::DeviceMeshModel* myDevices READ myDevices CONSTANT)
    Q_PROPERTY(QString deviceMeshTier  READ deviceMeshTier  NOTIFY meshChanged)
    Q_PROPERTY(bool    meshRefreshing  READ meshRefreshing  NOTIFY meshRefreshingChanged)
    // VIV-69 update check: a build newer than VIVORA_VERSION is published.
    Q_PROPERTY(bool    updateAvailable READ updateAvailable NOTIFY updateChanged)
    Q_PROPERTY(QString updateVersion   READ updateVersion   NOTIFY updateChanged)
    Q_PROPERTY(QString updateNotes     READ updateNotes     NOTIFY updateChanged)
    // VIV-70 announcement modal (one eligible item shown at launch).
    Q_PROPERTY(bool         announcementVisible  READ announcementVisible  NOTIFY announcementChanged)
    Q_PROPERTY(QString      announcementType     READ announcementType     NOTIFY announcementChanged)
    Q_PROPERTY(QString      announcementTitle    READ announcementTitle    NOTIFY announcementChanged)
    Q_PROPERTY(QString      announcementBody     READ announcementBody     NOTIFY announcementChanged)
    Q_PROPERTY(QString      announcementImageUrl READ announcementImageUrl NOTIFY announcementChanged)
    Q_PROPERTY(QVariantList announcementButtons  READ announcementButtons  NOTIFY announcementChanged)
    // VIV-71 poll modal.
    Q_PROPERTY(bool         pollVisible      READ pollVisible      NOTIFY pollChanged)
    Q_PROPERTY(QString      pollQuestion     READ pollQuestion     NOTIFY pollChanged)
    Q_PROPERTY(QString      pollBody         READ pollBody         NOTIFY pollChanged)
    Q_PROPERTY(QString      pollResponseType READ pollResponseType NOTIFY pollChanged)
    Q_PROPERTY(QVariantList pollOptions      READ pollOptions      NOTIFY pollChanged)
    Q_PROPERTY(bool         pollShowResults  READ pollShowResults  NOTIFY pollChanged)
    Q_PROPERTY(QVariantMap  pollResults      READ pollResults      NOTIFY pollResultsChanged)

public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    bool    sharing() const     { return sharing_; }
    int     clientCount() const { return clientCount_; }
    QString myPeerCode() const  { return myPeerCode_; }
    QString myPubkeyHex() const { return myPubkeyHex_; }
    QString myFingerprint() const { return myFingerprint_; }
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

    DeviceMeshModel* myDevices() const { return myDevices_.get(); }
    // "pro" when Pro-licensed (trial folded into pro until VIV-108), else "free".
    // Not signed in also gates to "free".
    QString deviceMeshTier() const {
        return (accountLoggedIn() && licensePro_) ? QStringLiteral("pro")
                                                  : QStringLiteral("free");
    }
    bool    meshRefreshing()  const { return meshRefreshing_; }

    bool    updateAvailable() const { return updateAvailable_; }
    QString updateVersion()   const { return updateVersion_; }
    QString updateNotes()     const { return updateNotes_; }

    bool         announcementVisible()  const { return annVisible_; }
    QString      announcementType()     const { return annType_; }
    QString      announcementTitle()    const { return annTitle_; }
    QString      announcementBody()     const { return annBody_; }
    QString      announcementImageUrl() const { return annImage_; }
    QVariantList announcementButtons()  const { return annButtons_; }

    bool         pollVisible()      const { return pollVisible_; }
    QString      pollQuestion()     const { return pollQuestion_; }
    QString      pollBody()         const { return pollBody_; }
    QString      pollResponseType() const { return pollType_; }
    QVariantList pollOptions()      const { return pollOptions_; }
    bool         pollShowResults()  const { return pollShowResults_; }
    QVariantMap  pollResults()      const { return pollResults_; }

    // Wired from main.cpp at app init.  AppController borrows the tray
    // pointer; ownership stays with gui_main().
    void setTray(Tray* tray);

public slots:
    void startSharing();
    void stopSharing();
    void connectToPeer(const QString& peerCodeOrHex);
    // VIV-52: connect to one of our own account devices.  Pre-pins the
    // device's known mesh key (when it's a current active member) so the
    // viewer skips the first-connect TOFU dialog, then dials its peer code.
    // Any doubt (empty/warned/undecodable key, pin failure) falls back to
    // connectToPeer — the user just sees the normal TOFU prompt.
    Q_INVOKABLE void connectToAccountDevice(const QString& peerCode,
                                            const QString& pubkeyHex);
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
    // audio/fileTransfer (VIV-60/VIV-65) are the per-connection capability
    // grants from the dialog toggles.
    Q_INVOKABLE void approveConnection(const QString& key, bool remember = false,
                                       bool input = true, bool clipboard = true,
                                       bool audio = true,
                                       bool fileTransfer = false);
    Q_INVOKABLE void rejectConnection(const QString& key);

    // Force an immediate rendezvous re-registration.  QML calls this
    // from the Refresh button in the sharing card.
    Q_INVOKABLE void refreshRendezvous();

    // VIV-23 TOFU trust prompt resolution — QML calls this from the
    // TrustPromptDialog buttons.  trust=true pins (or replaces) the peer's
    // key in known_peers.txt and re-dials the connect that was paused;
    // trust=false drops the attempt.
    Q_INVOKABLE void resolveTrustPrompt(bool trust);

    // Small QML helper: put `text` on the system clipboard (used by the
    // copy button next to the fingerprint in Settings, VIV-23).
    Q_INVOKABLE void copyToClipboard(const QString& text);

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
    // VIV-70: open an announcement button's URL / dismiss the current modal.
    Q_INVOKABLE void openAnnouncementUrl(const QString& url);
    Q_INVOKABLE void dismissAnnouncement();
    // VIV-71: submit the current poll's answer / dismiss the poll modal.
    // choice = list of option-id strings; rating = 1-5 (0 if N/A).
    Q_INVOKABLE void submitPollResponse(const QVariantList& choice, int rating,
                                        const QString& comment);
    Q_INVOKABLE void dismissPoll();

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
    // VIV-52: device-mesh tier changed / a refresh started or finished.
    void meshChanged();
    void meshRefreshingChanged();
    // A newer build is available (VIV-69) — QML shows the update banner.
    void updateChanged();
    // An announcement is ready to show / was dismissed (VIV-70).
    void announcementChanged();
    // A poll is ready to show / was answered (VIV-71); results arrived.
    void pollChanged();
    void pollResultsChanged();
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
    // VIV-23: outgoing connect paused on a TOFU trust question.  QML shows
    // TrustPromptDialog; mismatch=false is the friendly first-connect
    // variant, mismatch=true is the red key-changed (possible MITM) one.
    // Fingerprints are the canonical short form (crypto::key_fingerprint).
    void trustPromptRequested(QString peerCode,
                              QString newFingerprint,
                              QString oldFingerprint,
                              bool    mismatch);

private:
    void loadIdentity();        // populates myPeerCode_ + myPubkeyHex_

    std::unique_ptr<Settings>    settings_;
    std::unique_ptr<AddressBook> peers_;
    std::unique_ptr<HostWorker>  hostWorker_;
    // VIV-57: OS network-change events (reachability / transport / mac
    // wake) force an immediate rendezvous re-register so the host's
    // reflexive address is never stale for the full 30 s keepalive.
    std::unique_ptr<NetworkChangeWatcher> netWatcher_;
    // Shared with the HostWorker thread.  Holds per-client approval
    // state and the callback we wire to bounce notifications back into
    // the GUI thread (where QML can show the approval dialog).
    std::shared_ptr<vivora::host::HostApprovalGate> approvalGate_;
    // VIV-22 clipboard sync while sharing.  The bridge is shared with the
    // host worker thread (via HostWorkerConfig); the ClipboardSync QObject
    // lives on the GUI thread and owns the QClipboard wiring.  Both exist
    // only while sharing_ is true.
    std::shared_ptr<vivora::ClipboardBridge> hostClipboardBridge_;
    std::unique_ptr<ClipboardSync>           hostClipboardSync_;
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
    QString myFingerprint_;

    // VIV-23: the connect attempt paused on a trust question.  dial is the
    // exact string the user dialled (re-fed to connectToPeer on consent);
    // code + newHex identify what to pin.
    QString trustDial_;
    QString trustCode_;
    QString trustNewHex_;

    // VIV-29 license status (verified offline).
    bool    licenseValid_ = false;
    bool    licensePro_   = false;
    QString licenseExpiry_;

    // VIV-31 account/cloud.
    CloudClient cloud_;
    QString     accountEmail_;
    QString     accountUserId_;
    void        wireCloud();

    // VIV-52 device mesh.  The model is owned here and exposed as App.myDevices;
    // the heartbeat timer keeps this install marked online; the SSE stream (in
    // CloudClient) pushes devicesChanged, which triggers a debounced re-fetch.
    std::unique_ptr<DeviceMeshModel> myDevices_;
    QTimer  meshHeartbeatTimer_;   // 60s presence ping while signed in
    QTimer  meshRefreshDebounce_;  // coalesce SSE/register/delete → one fetch
    bool    meshRefreshing_ = false;
    // VIV-52: snapshot of the account's active (non-warned) device pubkeys,
    // lowercased, from the last /devices/me fetch.  Diffed on each refresh so
    // a pubkey that leaves the set → an immediate kick request on the gate.
    QSet<QString> meshActivePubkeys_;
    // VIV-52: armed only after this install's first successful device register
    // this session.  The self-logout-on-absence check is gated on it so the
    // initial pre-register /devices/me fetch (which cannot yet contain our own
    // row) does not mistake a normal login for a removal.  Reset on logOut().
    bool    meshRegistered_ = false;
    void    wireDeviceMesh();
    void    startDeviceMesh();     // register + heartbeat + stream + first fetch
    void    stopDeviceMesh();      // on sign-out
    void    refreshDevices();      // GET /devices/me (sets meshRefreshing_)
    QString meshDeviceName() const;
    QString meshDeviceOs() const;

    // VIV-69 update check (notify-only).
    UpdateChecker update_;
    bool          updateAvailable_ = false;
    QString       updateVersion_;
    QString       updateUrl_;
    QString       updateNotes_;
    void          wireUpdate();

    // VIV-70 announcements.  annQueue_ holds the remaining eligible items; we
    // show them one at a time, advancing on dismiss.
    AnnouncementsClient announcements_;
    bool         annVisible_ = false;
    QString      annId_, annType_, annTitle_, annBody_, annImage_;
    QVariantList annButtons_;
    QVariantList annQueue_;
    void         wireAnnouncements();
    void         showNextAnnouncement();

    // VIV-71 polls.  One poll shown at launch (highest priority eligible).
    PollsClient  polls_;
    bool         pollVisible_ = false;
    bool         pollShowResults_ = false;
    QString      pollId_, pollQuestion_, pollBody_, pollType_;
    QVariantList pollOptions_;
    QVariantMap  pollResults_;
    void         wirePolls();
};

} // namespace vivora::gui
