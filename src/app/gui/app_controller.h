#pragma once

#include "app/gui/address_book.h"
#include "app/gui/settings.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>
#include <vector>

namespace deskbeam::gui {

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
    Q_PROPERTY(deskbeam::gui::Settings* settings    READ settings    CONSTANT)
    Q_PROPERTY(deskbeam::gui::AddressBook* peers    READ peers       CONSTANT)

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

private:
    void loadIdentity();        // populates myPeerCode_ + myPubkeyHex_

    std::unique_ptr<Settings>    settings_;
    std::unique_ptr<AddressBook> peers_;
    std::unique_ptr<HostWorker>  hostWorker_;
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

} // namespace deskbeam::gui
