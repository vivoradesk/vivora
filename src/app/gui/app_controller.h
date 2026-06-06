#pragma once

#include "app/gui/address_book.h"
#include "app/gui/settings.h"
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
};

} // namespace vivora::gui
