#include "app/gui/app_controller.h"

#include "app/gui/address_book.h"
#include "app/gui/host_worker.h"
#include "app/gui/settings.h"
#include "app/gui/tray.h"
#if defined(VIVORA_WINDOWS) || defined(VIVORA_MACOS)
#include "app/gui/view_session.h"
#endif

#include "common/crypto/host_identity.h"
#include "common/utils/log.h"
#include "common/utils/peer_code.h"

#include <QApplication>
#include <QMessageBox>

namespace vivora::gui {

AppController::AppController(QObject* parent) : QObject(parent) {
    settings_   = std::make_unique<Settings>(this);
    peers_      = std::make_unique<AddressBook>(this);
    hostWorker_ = std::make_unique<HostWorker>(this);
    connect(hostWorker_.get(), &HostWorker::stopped, this, [this] {
        sharing_     = false;
        clientCount_ = 0;
        emit sharingChanged();
        emit clientCountChanged();
        if (tray_) tray_->setSharing(sharing_, clientCount_);
        log::info("AppController", "Host worker stopped");
    });
    connect(hostWorker_.get(), &HostWorker::initFailed, this,
            [this](QString reason) {
        log::error("AppController", "Host init failed: %s",
                   reason.toUtf8().constData());
        if (tray_) tray_->notify("Vivora: host failed to start", reason);
    });
    connect(hostWorker_.get(), &HostWorker::idleWarning, this,
            [this](int secs) {
        log::info("AppController", "Idle warning — disconnect in %ds", secs);
        if (tray_) tray_->notify("Vivora: idle",
            QString("No input from your viewer for %1 min — "
                    "disconnecting in %2s.")
                .arg(settings_->idleTimeoutMin()).arg(secs));
    });
    pollTimer_.setInterval(500);
    connect(&pollTimer_, &QTimer::timeout, this, [this] {
        if (!hostWorker_->running()) return;
        const int n = hostWorker_->clientCount();
        if (n != clientCount_) {
            clientCount_ = n;
            emit clientCountChanged();
            if (tray_) tray_->setSharing(sharing_, clientCount_);
        }
    });
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
    HostWorkerConfig wc;
    wc.port               = static_cast<uint16_t>(settings_->hostPort());
    wc.manual_bitrate_bps = settings_->bitrateMbps() * 1'000'000u;
    wc.codec              = settings_->codecIndex() == 1
        ? vivora::VideoCodec::HEVC : vivora::VideoCodec::H264;
    wc.encoder_kind       = vivora::EncoderKind::Auto;
    wc.stun_server        = settings_->stunServer().toStdString();
    wc.rendezvous_server  = settings_->rendezvous().toStdString();
    wc.relay_server       = settings_->relay().toStdString();
    wc.license_file       = settings_->licenseFile().toStdString();
    wc.display_index      = settings_->displayIndex();
    wc.idle_timeout_min   = settings_->idleTimeoutMin();
    wc.idle_warning_sec   = settings_->idleWarningSec();

    sharing_     = true;
    clientCount_ = 0;
    emit sharingChanged();
    emit clientCountChanged();
    if (tray_) tray_->setSharing(sharing_, clientCount_);
    pollTimer_.start();

    log::info("AppController", "Start sharing (port=%u, codec=%s, rdv='%s')",
              wc.port,
              wc.codec == vivora::VideoCodec::HEVC ? "hevc" : "h264",
              wc.rendezvous_server.c_str());
    hostWorker_->start(wc);
}

void AppController::stopSharing() {
    if (!sharing_) return;
    log::info("AppController", "Stop sharing requested");
    pollTimer_.stop();
    hostWorker_->stop();
    // sharing_/clientCount_ get reset on the worker's stopped() signal.
}

void AppController::connectToPeer(const QString& peerCodeOrHex) {
    log::info("AppController", "Connect requested: %s",
              peerCodeOrHex.toUtf8().constData());
#if defined(VIVORA_WINDOWS) || defined(VIVORA_MACOS)
    GuiViewConfig vc;
    vc.host_ip            = "";  // rendezvous resolves
    vc.port               = static_cast<uint16_t>(settings_->hostPort());
    vc.stun_server        = settings_->stunServer().toStdString();
    vc.rendezvous_server  = settings_->rendezvous().toStdString();
    vc.peer_pubkey_hex    = peerCodeOrHex.toStdString();
    vc.relay_server       = settings_->relay().toStdString();
    vc.license_file       = settings_->licenseFile().toStdString();

    auto vs = std::make_unique<ViewSession>(this);
    ViewSession* vs_ptr = vs.get();
    connect(vs_ptr, &ViewSession::finished, this, [this, vs_ptr] {
        for (auto it = viewSessions_.begin(); it != viewSessions_.end(); ++it) {
            if (it->get() == vs_ptr) {
                viewSessions_.erase(it);
                activeViews_ = static_cast<int>(viewSessions_.size());
                emit activeViewsChanged();
                break;
            }
        }
        log::info("AppController", "View session ended (%d remaining)", activeViews_);
    });
    if (!vs->start(vc)) {
        log::error("AppController", "ViewSession::start failed");
        if (tray_) tray_->notify("Vivora",
            QString("Could not connect to %1").arg(peerCodeOrHex));
        return;
    }
    viewSessions_.push_back(std::move(vs));
    activeViews_ = static_cast<int>(viewSessions_.size());
    emit activeViewsChanged();
    // Pre-populate the address book so the peer shows up under Recent
    // once the session lands (real pubkey resolution happens later).
    peers_->touch(peerCodeOrHex, peerCodeOrHex);
#else
    (void)peerCodeOrHex;
    log::warn("AppController", "Connect not yet implemented on this platform");
#endif
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
            nullptr, "Quit Vivora?", detail,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (btn != QMessageBox::Yes) return;
    }
    QApplication::quit();
}

} // namespace vivora::gui
