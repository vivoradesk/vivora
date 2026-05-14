#include "app/gui/app_controller.h"

#include "app/gui/address_book.h"
#include "app/gui/settings.h"
#include "app/gui/tray.h"

#include "common/crypto/host_identity.h"
#include "common/utils/log.h"
#include "common/utils/peer_code.h"

#include <QApplication>
#include <QMessageBox>

namespace deskbeam::gui {

AppController::AppController(QObject* parent) : QObject(parent) {
    settings_ = std::make_unique<Settings>(this);
    peers_    = std::make_unique<AddressBook>(this);
    loadIdentity();

    // Honour "Start sharing on launch" — stub for Phase A.  Phase A.1
    // will actually spin up the host worker; for now this just flips the
    // bool so the UI shows "● Sharing".
    if (settings_->startSharingOnLaunch()) {
        startSharing();
    }
}

AppController::~AppController() = default;

void AppController::setTray(Tray* tray) {
    tray_ = tray;
    if (!tray_) return;
    connect(tray_, &Tray::showRequested,        this, &AppController::showMainWindow);
    connect(tray_, &Tray::stopSharingRequested, this, &AppController::stopSharing);
    connect(tray_, &Tray::settingsRequested,    this, &AppController::openSettings);
    connect(tray_, &Tray::quitRequested,        this, &AppController::quit);
    tray_->setSharing(sharing_, clientCount_);
}

void AppController::loadIdentity() {
    crypto::KeyPair kp;
    if (!crypto::load_or_create_host_identity(kp, "")) {
        log::error("AppController", "Failed to load/create host identity");
        return;
    }
    myPubkeyHex_ = QString::fromStdString(crypto::hex_encode(kp.public_key, 32));
    myPeerCode_  = QString::fromStdString(peer_code::encode(kp.public_key));
    emit identityChanged();
}

void AppController::startSharing() {
    if (sharing_) return;
    // Phase A stub — Phase A.1 will actually run the host worker.
    sharing_     = true;
    clientCount_ = 0;
    log::info("AppController", "Start sharing (stub — Phase A.1 hooks the real worker)");
    emit sharingChanged();
    emit clientCountChanged();
    if (tray_) tray_->setSharing(sharing_, clientCount_);
}

void AppController::stopSharing() {
    if (!sharing_) return;
    sharing_     = false;
    clientCount_ = 0;
    log::info("AppController", "Stop sharing");
    emit sharingChanged();
    emit clientCountChanged();
    if (tray_) tray_->setSharing(sharing_, clientCount_);
}

void AppController::connectToPeer(const QString& peerCodeOrHex) {
    log::info("AppController", "Connect requested: %s",
              peerCodeOrHex.toUtf8().constData());
    // Phase A stub — increments view count for UI feedback.  Phase A.1
    // will spawn the actual ClientSession worker and open a stream
    // window when the handshake succeeds.
    activeViews_++;
    emit activeViewsChanged();
    if (tray_) tray_->notify("DeskBeam",
        QString("Connecting to %1 — stub mode (Phase A.1 hooks the worker)").arg(peerCodeOrHex));
}

void AppController::disconnectView(int /*viewId*/) {
    if (activeViews_ <= 0) return;
    activeViews_--;
    emit activeViewsChanged();
}

void AppController::openSettings() {
    emit settingsRequested();
}

void AppController::showMainWindow() {
    emit showWindowRequested();
}

void AppController::quit() {
    if (sharing_ || activeViews_ > 0) {
        const QString detail = QString(
            "You have %1 active session%2 (sharing=%3).  Quit anyway?")
            .arg(sharing_ ? activeViews_ + 1 : activeViews_)
            .arg((sharing_ ? activeViews_ + 1 : activeViews_) == 1 ? "" : "s")
            .arg(sharing_ ? "yes" : "no");
        const auto btn = QMessageBox::warning(
            nullptr, "Quit DeskBeam?", detail,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (btn != QMessageBox::Yes) return;
    }
    QApplication::quit();
}

} // namespace deskbeam::gui
