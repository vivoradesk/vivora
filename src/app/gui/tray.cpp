#include "app/gui/tray.h"

#include "app/gui/app_icon.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>

namespace vivora::gui {

Tray::Tray(QObject* parent) : QObject(parent) {
    tray_ = new QSystemTrayIcon(this);
    // Procedurally drawn brand icon — see app_icon.cpp.  Idle = grey-blue
    // until setSharing() flips us to the green sharing variant.
    tray_->setIcon(make_tray_idle_icon());
    tray_->setToolTip("Vivora");

    menu_ = new QMenu();
    showAct_         = menu_->addAction("Show Vivora");
    pauseResumeAct_  = menu_->addAction("Pause sharing");
    menu_->addSeparator();
    settingsAct_     = menu_->addAction("Settings…");
    menu_->addSeparator();
    quitAct_         = menu_->addAction("Quit Vivora");

    tray_->setContextMenu(menu_);
    tray_->show();

    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason r){
        // DoubleClick on Win/Linux, Trigger (single click) on macOS.
        if (r == QSystemTrayIcon::DoubleClick || r == QSystemTrayIcon::Trigger) {
            emit showRequested();
        }
    });
    connect(showAct_, &QAction::triggered, this, &Tray::showRequested);
    connect(pauseResumeAct_, &QAction::triggered, this, [this] {
        // setSharing flips the label every state change, so the current
        // text tells us which signal to emit.
        if (pauseResumeAct_->text().startsWith("Pause")) {
            emit pauseRequested();
        } else {
            emit resumeRequested();
        }
    });
    connect(settingsAct_,    &QAction::triggered, this, &Tray::settingsRequested);
    connect(quitAct_,        &QAction::triggered, this, &Tray::quitRequested);
}

Tray::~Tray() {
    if (menu_) { menu_->deleteLater(); menu_ = nullptr; }
}

void Tray::setPro(bool isPro) {
    if (pro_ == isPro) return;
    pro_ = isPro;
    setSharing(lastSharing_, lastClients_);   // rebuild tooltip with/without Pro
}

void Tray::setSharing(bool sharing, int clientCount) {
    lastSharing_ = sharing;
    lastClients_ = clientCount;
    pauseResumeAct_->setText(sharing ? "Pause sharing" : "Resume sharing");
    QString tip = "Vivora";
    if (sharing) {
        tip = clientCount == 0
            ? QString("Vivora — available")
            : QString("Vivora — sharing (%1 client%2)")
                .arg(clientCount).arg(clientCount == 1 ? "" : "s");
    } else {
        tip = "Vivora — paused";
    }
    if (pro_) tip += " · Pro";
    tray_->setToolTip(tip);
    // Visible state change — green sharing variant when at least one
    // viewer is active, grey-blue idle otherwise.  setIcon is cheap
    // (Qt caches the pixmap set) so it's safe to call on every flip.
    tray_->setIcon(sharing && clientCount > 0
                   ? make_tray_sharing_icon()
                   : make_tray_idle_icon());
}

void Tray::notify(const QString& title, const QString& body) {
    if (!tray_->supportsMessages()) return;
    tray_->showMessage(title, body, QSystemTrayIcon::Information, 5000);
}

} // namespace vivora::gui
