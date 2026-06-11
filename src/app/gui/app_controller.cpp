#include "app/gui/app_controller.h"

#include "app/gui/address_book.h"
#include "app/gui/host_worker.h"
#include "app/gui/settings.h"
#include "app/gui/tray.h"
#if defined(VIVORA_WINDOWS) || defined(VIVORA_MACOS) || defined(VIVORA_LINUX)
#include "app/gui/view_session.h"
#endif

#include "common/crypto/host_identity.h"
#include "common/crypto/license_pubkey.h"
#include "common/crypto/license_token.h"
#include "common/utils/log.h"
#include "common/utils/peer_code.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>
#include <QUrl>

namespace vivora::gui {

AppController::AppController(QObject* parent) : QObject(parent) {
    settings_   = std::make_unique<Settings>(this);
    peers_      = std::make_unique<AddressBook>(this);
    hostWorker_ = std::make_unique<HostWorker>(this);

    // VIV-53 approval gate.  Lives here (shared_ptr) and gets handed
    // to the worker via HostWorkerConfig.  The callback runs on the
    // worker thread; we bounce to the GUI thread via QMetaObject so
    // QML signals only ever fire from the GUI side.
    approvalGate_ = std::make_shared<vivora::host::HostApprovalGate>();
    approvalGate_->set_callback([this](uint64_t key,
                                       const std::string& peer_code,
                                       const std::string& pubkey_hex,
                                       const std::string& ip_port,
                                       const std::string& device_name) {
        QString k        = QString::number(key);
        QString code     = QString::fromStdString(peer_code);
        QString pubkey   = QString::fromStdString(pubkey_hex);
        QString ip       = QString::fromStdString(ip_port);
        QString name     = QString::fromStdString(device_name);
        QMetaObject::invokeMethod(this, [this, k, code, pubkey, ip, name] {
            // Apply the approval policy here (GUI thread) so HostSession
            // stays dumb — it just reports Pending, we decide.
            //   0 = always_prompt        → show dialog
            //   1 = prompt_unknown_only  → auto-accept recognised pubkeys
            //   2 = auto_accept          → accept without a dialog, EXCEPT
            //                              a brand-new pubkey still prompts
            //                              (TOFU: a leaked code alone must
            //                              not grant silent access).
            // A per-peer "trusted" flag (don't-ask-again) auto-accepts in
            // any mode.  recognised/seen drive the dialog's trust card.
            const auto* peer = (!pubkey.isEmpty() && peers_)
                               ? peers_->findByPubkey(pubkey) : nullptr;
            const bool recognized = peer != nullptr;
            const int  seenCount  = peer ? peer->seen : 0;
            const bool trusted    = peer && peer->trusted;
            const int  mode       = settings_ ? settings_->approvalMode() : 0;

            // Stash the identity so approve/reject can record + optionally
            // pin the viewer once the user (or the auto path) decides.
            pendingApprovals_.insert(k, qMakePair(pubkey, code));

            const bool autoAccept = trusted
                || (mode == 2 && recognized)
                || (mode == 1 && recognized);
            if (autoAccept) {
                // Reuse the grant last chosen for this peer (VIV-60) so a
                // view-only trusted peer stays view-only; full access by
                // default for a freshly auto-accepted known peer.
                const bool gi = peer ? peer->grantInput     : true;
                const bool gc = peer ? peer->grantClipboard : true;
                const bool gf = peer ? peer->grantFile      : false;
                approveConnection(k, false, gi, gc, gf);
                return;
            }
            // System notification so the user notices the prompt when the
            // main window is hidden / in the background (VIV-55).  The OS
            // notification carries the default alert sound.
            if (tray_) tray_->notify("Vivora — incoming connection",
                QString("A peer (%1) wants to view your desktop.").arg(ip));
            emit connectionApprovalRequested(k, code, pubkey, ip,
                                             recognized, seenCount, name);
        }, Qt::QueuedConnection);
    });
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

    // VIV-29: verify the configured license offline on startup, and re-verify
    // whenever any setting changes (covers the license path being edited).
    refreshLicense();
    if (settings_) connect(settings_.get(), &Settings::changed,
                           this, &AppController::refreshLicense);

    // Always-available model (VIV-53): host starts immediately at app
    // launch.  Peer code is visible the moment the user sees the
    // window — no Start button to click.  Stop/Pause is reachable via
    // the tray menu and the Pause button in the sharing card.
    //
    // The 500ms delay matters on macOS: starting the host worker
    // immediately fires SCK init which raises the TCC Screen Recording
    // prompt — and if our main window is shown in the same event-loop
    // tick it lands ON TOP of the prompt, hiding it.  By the time the
    // singleShot fires, QApplication::exec has rendered the window so
    // the OS dialog stacks above it correctly.
    //
    // Dev opt-out: VIVORA_NO_AUTOSTART=1 keeps the host loop off so
    // testing UI changes doesn't burn the encoder + show TCC prompts.
    if (qEnvironmentVariableIsEmpty("VIVORA_NO_AUTOSTART")) {
        QTimer::singleShot(500, this, [this] { startSharing(); });
    }

    // Dev hook: VIVORA_FAKE_APPROVAL=1 fires a synthetic incoming-connection
    // prompt ~1.2s after launch so the ConnectionApprovalDialog (VIV-55) can
    // be eyeballed / screenshotted without a second machine.  Sample data
    // only — no real client.  Off by default.
    if (!qEnvironmentVariableIsEmpty("VIVORA_FAKE_APPROVAL")) {
        QTimer::singleShot(1200, this, [this] {
            // Fake a new (unrecognized) viewer with a sample fingerprint.
            emit connectionApprovalRequested(
                "424242", "civic-panda-4644",
                "6d2e0c4a7f3b9e1182a4c5d6e7f8091a2b3c4d5e6f70812233445566778899aa",
                "192.168.3.243:62378", /*recognized=*/false, /*seenCount=*/0,
                "John's MacBook Pro");
        });
    }
}

AppController::~AppController() = default;

void AppController::setTray(Tray* tray) {
    tray_ = tray;
    if (!tray_) return;
    connect(tray_, &Tray::showRequested,     this, &AppController::showMainWindow);
    connect(tray_, &Tray::pauseRequested,    this, &AppController::stopSharing);
    connect(tray_, &Tray::resumeRequested,   this, &AppController::startSharing);
    connect(tray_, &Tray::settingsRequested, this, &AppController::openSettings);
    connect(tray_, &Tray::quitRequested,     this, &AppController::quit);
    tray_->setSharing(sharing_, clientCount_);
    tray_->setPro(licensePro_);
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
    {
        // VIV-29: the Vivora-managed relay (relay.vivora.dev) is a Pro
        // feature.  Without a Pro license, don't even attempt it — fall back
        // to direct + rendezvous hole-punch.  Self-hosted relays (any other
        // host) are AGPL and stay available to everyone.
        const QString relay = settings_->relay();
        const bool gated = relay.contains("vivora.dev", Qt::CaseInsensitive)
                           && !licensePro_;
        wc.relay_server = gated ? std::string() : relay.toStdString();
        if (gated) log::info("AppController",
            "Managed relay needs Pro — sharing via direct/rendezvous only");
    }
    wc.license_file       = settings_->licenseFile().toStdString();
    wc.display_index      = settings_->displayIndex();
    wc.idle_timeout_min   = settings_->idleTimeoutMin();
    wc.idle_warning_sec   = settings_->idleWarningSec();
    wc.approval_gate      = approvalGate_;

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
#if defined(VIVORA_WINDOWS) || defined(VIVORA_MACOS) || defined(VIVORA_LINUX)
    GuiViewConfig vc;
    vc.host_ip            = "";  // rendezvous resolves
    vc.port               = static_cast<uint16_t>(settings_->hostPort());
    vc.stun_server        = settings_->stunServer().toStdString();
    vc.rendezvous_server  = settings_->rendezvous().toStdString();
    vc.peer_pubkey_hex    = peerCodeOrHex.toStdString();
    {
        // VIV-29: managed relay is Pro-gated (see startSharing).
        const QString relay = settings_->relay();
        const bool gated = relay.contains("vivora.dev", Qt::CaseInsensitive)
                           && !licensePro_;
        vc.relay_server = gated ? std::string() : relay.toStdString();
        if (gated) log::info("AppController",
            "Managed relay needs Pro — connecting via direct/rendezvous only");
    }
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
    peers_->touchOutgoing(peerCodeOrHex, peerCodeOrHex);
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

void AppController::approveConnection(const QString& key, bool remember,
                                      bool input, bool clipboard,
                                      bool fileTransfer) {
    if (!approvalGate_) return;
    bool ok = false;
    uint64_t k = key.toULongLong(&ok);
    if (!ok) return;
    // Record the capability grant before flipping to Approved so HostSession
    // reads the right grant on the transition (VIV-60).
    approvalGate_->set_grant(k, vivora::host::CapabilityGrant{
        input, clipboard, fileTransfer});
    approvalGate_->set_state(k, vivora::host::ApprovalState::Approved);

    // Record the viewer in the address book (seen++, surfaces under Recent
    // as an incoming ↓ peer) and, if the user ticked "don't ask again", pin
    // it as trusted so future connects auto-accept (VIV-61).  Persist the
    // grant either way so a later auto-accept reuses it (VIV-60).
    auto it = pendingApprovals_.find(key);
    if (it != pendingApprovals_.end()) {
        const QString pubkey = it.value().first;
        const QString pcode  = it.value().second;
        if (peers_ && !pubkey.isEmpty()) {
            peers_->touchIncoming(pubkey, pcode);
            peers_->setGrantByPubkey(pubkey, input, clipboard, fileTransfer);
            if (remember) peers_->setTrustedByPubkey(pubkey, true);
        }
        pendingApprovals_.erase(it);
    }
    log::info("AppController",
              "Approved key=%llu remember=%d input=%d clip=%d file=%d",
              static_cast<unsigned long long>(k), remember ? 1 : 0,
              input, clipboard, fileTransfer);
}

void AppController::rejectConnection(const QString& key) {
    if (!approvalGate_) return;
    bool ok = false;
    uint64_t k = key.toULongLong(&ok);
    if (!ok) return;
    approvalGate_->set_state(k, vivora::host::ApprovalState::Rejected);
    pendingApprovals_.remove(key);
    log::info("AppController", "Rejected connection key=%llu",
              static_cast<unsigned long long>(k));
}

void AppController::refreshRendezvous() {
    if (hostWorker_) {
        hostWorker_->requestRendezvousRefresh();
        log::info("AppController", "Manual rendezvous refresh requested");
    }
}

// ── License (VIV-29) ───────────────────────────────────────────────────────

void AppController::refreshLicense() {
    const bool was_valid = licenseValid_;
    const bool was_pro   = licensePro_;
    const QString was_exp = licenseExpiry_;

    licenseValid_ = false;
    licensePro_   = false;
    licenseExpiry_.clear();

    QString path = settings_ ? settings_->licenseFile() : QString();
    if (path.isEmpty()) {
        // Default location next to the config — where importLicense() drops it.
        const QString dir = QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation);
        const QString def = dir + "/license.bin";
        if (QFile::exists(def)) path = def;
    }

    if (!path.isEmpty()) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray data = f.read(crypto::LICENSE_TOKEN_SIZE + 1);
            if (data.size() == static_cast<int>(crypto::LICENSE_TOKEN_SIZE)) {
                crypto::LicenseClaims claims;
                const int64_t now = QDateTime::currentSecsSinceEpoch();
                if (crypto::verify_license(
                        reinterpret_cast<const uint8_t*>(data.constData()),
                        crypto::LICENSE_PUBLIC_KEY, now, claims)) {
                    licenseValid_   = true;
                    licensePro_     = (claims.tier == crypto::LicenseTier::Pro);
                    licenseExpiry_  = QDateTime::fromSecsSinceEpoch(claims.exp_unix)
                                          .toUTC().date().toString(Qt::ISODate);
                    log::info("AppController", "License OK: %s, expires %s",
                              licensePro_ ? "Pro" : "Trial",
                              licenseExpiry_.toUtf8().constData());
                } else {
                    log::warn("AppController",
                              "License present but invalid/expired: %s",
                              path.toUtf8().constData());
                }
            } else {
                log::warn("AppController", "License file is not %zu bytes: %s",
                          crypto::LICENSE_TOKEN_SIZE, path.toUtf8().constData());
            }
        }
    }

    if (tray_) tray_->setPro(licensePro_);
    if (licenseValid_ != was_valid || licensePro_ != was_pro ||
        licenseExpiry_ != was_exp) {
        emit licenseChanged();
    }
}

void AppController::importLicense(const QString& pathOrUrl) {
    QString src = pathOrUrl;
    if (src.startsWith("file:")) src = QUrl(src).toLocalFile();
    if (src.isEmpty()) return;

    // Validate before adopting it, so a bad file doesn't silently replace a
    // good license.
    QFile in(src);
    if (!in.open(QIODevice::ReadOnly)) {
        log::warn("AppController", "importLicense: cannot open %s",
                  src.toUtf8().constData());
        return;
    }
    const QByteArray data = in.read(crypto::LICENSE_TOKEN_SIZE + 1);
    crypto::LicenseClaims claims;
    if (data.size() != static_cast<int>(crypto::LICENSE_TOKEN_SIZE) ||
        !crypto::verify_license(
            reinterpret_cast<const uint8_t*>(data.constData()),
            crypto::LICENSE_PUBLIC_KEY,
            QDateTime::currentSecsSinceEpoch(), claims)) {
        log::warn("AppController", "importLicense: invalid/expired token %s",
                  src.toUtf8().constData());
        if (tray_) tray_->notify("Vivora",
            "That license file is invalid or expired.");
        return;
    }

    // Copy into the app data dir as license.bin and point the setting at it.
    const QString dir = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString dst = dir + "/license.bin";
    QFile::remove(dst);
    QFile out(dst);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(data);
        out.close();
        if (settings_) settings_->setLicenseFile(dst);   // triggers refreshLicense via changed()
        refreshLicense();
        log::info("AppController", "License imported -> %s",
                  dst.toUtf8().constData());
    } else {
        log::warn("AppController", "importLicense: cannot write %s",
                  dst.toUtf8().constData());
    }
}

void AppController::quit() {
    // Confirm only if there are REAL active sessions: someone is
    // currently connected to us, or we have outgoing view sessions.
    // The mere fact that the host loop is running (sharing_=true under
    // the always-available model from VIV-53) no longer warrants a
    // popup — it's the default state on every launch.
    const int total = clientCount_ + activeViews_;
    if (total > 0) {
        const QString detail = QString(
            "You have %1 active session%2.  Quit anyway?")
            .arg(total).arg(total == 1 ? "" : "s");
        const auto btn = QMessageBox::warning(
            nullptr, "Quit Vivora?", detail,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (btn != QMessageBox::Yes) return;
    }
    QApplication::quit();
}

} // namespace vivora::gui
