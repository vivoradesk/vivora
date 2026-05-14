#include "app/gui/tray.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QStyle>

namespace deskbeam::gui {

Tray::Tray(QObject* parent) : QObject(parent) {
    tray_ = new QSystemTrayIcon(this);
    // Placeholder icon — picked from the platform style so it shows
    // *something* on every OS without having to ship a graphic yet.  Will
    // be replaced by a proper SVG in resources/ later.
    tray_->setIcon(QApplication::style()->standardIcon(QStyle::SP_ComputerIcon));
    tray_->setToolTip("DeskBeam");

    menu_ = new QMenu();
    showAct_         = menu_->addAction("Show DeskBeam");
    stopSharingAct_  = menu_->addAction("Stop sharing");
    stopSharingAct_->setEnabled(false);
    menu_->addSeparator();
    settingsAct_     = menu_->addAction("Settings…");
    menu_->addSeparator();
    quitAct_         = menu_->addAction("Quit DeskBeam");

    tray_->setContextMenu(menu_);
    tray_->show();

    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason r){
        // DoubleClick on Win/Linux, Trigger (single click) on macOS.
        if (r == QSystemTrayIcon::DoubleClick || r == QSystemTrayIcon::Trigger) {
            emit showRequested();
        }
    });
    connect(showAct_,        &QAction::triggered, this, &Tray::showRequested);
    connect(stopSharingAct_, &QAction::triggered, this, &Tray::stopSharingRequested);
    connect(settingsAct_,    &QAction::triggered, this, &Tray::settingsRequested);
    connect(quitAct_,        &QAction::triggered, this, &Tray::quitRequested);
}

Tray::~Tray() {
    if (menu_) { menu_->deleteLater(); menu_ = nullptr; }
}

void Tray::setSharing(bool sharing, int clientCount) {
    stopSharingAct_->setEnabled(sharing);
    QString tip = "DeskBeam";
    if (sharing) {
        tip = QString("DeskBeam — sharing (%1 client%2)")
                .arg(clientCount).arg(clientCount == 1 ? "" : "s");
    }
    tray_->setToolTip(tip);
    // Could swap the icon here for a "live" variant — placeholder for now.
}

void Tray::notify(const QString& title, const QString& body) {
    if (!tray_->supportsMessages()) return;
    tray_->showMessage(title, body, QSystemTrayIcon::Information, 5000);
}

} // namespace deskbeam::gui
