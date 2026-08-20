#pragma once

#include "app/clipboard_bridge.h"
#include "app/gui/clipboard_sync.h"
#include "app/view_loop.h"
#include "app/view_platform.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace vivora::gui {

// Durable, GUI-owned twin of ViewLoopConfig.  We hold std::string
// backing storage because ViewLoopConfig stores raw `const char*`
// that the loop will dereference iter-after-iter — those pointers
// must outlive every iter() call.
struct GuiViewConfig {
    std::string host_ip;         // empty = "0.0.0.0" (rendezvous only)
    uint16_t    port             = 9900;
    std::string stun_server;
    std::string host_key_hex;    // 64 hex; optional when peer code provided
    std::string rendezvous_server;
    std::string peer_pubkey_hex; // hex pubkey OR memorable code
    // VIV-52: the account device's mesh pubkey when dialled from My Devices
    // (empty for a plain peer-code connect).  Lets a later membership removal
    // stop exactly the session viewing the removed device.
    std::string account_pubkey_hex;
    std::string relay_server;
    std::string relay_session_hex;
    std::string license_file;
    // User viewing caps from Settings (0 = none).
    int view_fps_cap  = 0;
    int view_max_kbps = 0;
    // VIV-54 auto-reconnect budget in ms (client_reconnect_timeout; 0 = off).
    uint32_t reconnect_timeout_ms = 0;
};

// One active "Connect to peer" session in the GUI.
//
// Lives on the main thread.  Owns the concrete platform (Windows
// StreamWindow QWidget, macOS Cocoa NSWindow) which is created
// on this same thread per Qt / AppKit rules, and a ViewLoopState
// driven by a QTimer at ~60Hz.  Each tick runs one pass of the
// view-loop body: poll the UDP socket, drain FEC groups, feed
// decoded frames to the GPU, present.  Total budget per tick is
// on the order of 5-10ms — well under the 16ms slot, so the main
// event loop stays responsive for QML and the tray.
class ViewSession : public QObject {
    Q_OBJECT

public:
    explicit ViewSession(QObject* parent = nullptr);
    ~ViewSession() override;

    // Wire up the session.  Returns false if platform init or socket
    // bind failed; the caller should drop the ViewSession in that case.
    bool start(const GuiViewConfig& cfg);

    // Cooperative stop.  Window stays visible until the loop's last
    // iter cleans up; finished() fires from the QTimer callback.
    void stop();

    // VIV-23: when start() returned false because the peer needs a TOFU
    // trust decision (first connect, or key changed), these expose the
    // details captured before the loop was torn down.  AppController shows
    // the trust dialog and, on consent, pins the key and re-dials.
    bool    trustPromptPending() const { return trustPending_; }
    bool    trustMismatch()      const { return trustMismatch_; }
    QString trustPeerCode()      const { return trustPeerCode_; }

    // VIV-52: the mesh pubkey of the account device this session is viewing
    // (empty for a plain peer-code connect).  Used to tear the session down
    // when that device is removed from the account.
    QString accountPubkeyHex() const {
        return QString::fromStdString(cfg_.account_pubkey_hex);
    }
    QString trustNewPubkeyHex()  const { return trustNewHex_; }
    QString trustOldPubkeyHex()  const { return trustOldHex_; }

    // True once the background connect handshake succeeded and the tick loop
    // started.  Lets AppController tell a genuine connect FAILURE (never
    // connected → "Could not connect") apart from a normal session end.
    bool    everConnected()      const { return everConnected_; }

    // Why the connect failed, phrased for a user.  Captured before the loop
    // is torn down; empty when the session connected or is waiting on a TOFU
    // trust decision.
    QString initError()          const { return initError_; }

    // Classified form of the same thing; AppController uses it to decide
    // whether the managed relay would actually have helped.
    ViewLoopState::ConnectFailure connectFailure() const { return connectFailure_; }

    // Whether a single frame ever arrived.  everConnected() only says the loop
    // started; this says the session actually did something, which is what
    // separates "your networks would not let you through" from "the stream
    // ran and then ended".
    bool    everStreamed()       const { return everStreamed_; }

signals:
    // Loop has fully torn down — session.stop(), platform.shutdown()
    // have run.  AppController removes us from its active-views list
    // on this signal.
    void finished();

private slots:
    void onTick();

private:
    // Build the platform-specific ViewPlatform implementation
    // (WindowsViewPlatform / MacViewPlatform).  Defined per-platform
    // in view_session.cpp under VIVORA_WINDOWS / VIVORA_MACOS guards.
    bool init_platform();

    // Resume point after the worker thread finished loop_->init(): start the
    // tick loop on success, or tear down + emit finished() on failure.  Always
    // runs on the GUI thread (posted via a queued invoke).
    void finishConnect(bool ok);

    GuiViewConfig                          cfg_;
    std::string                            host_ip_storage_;  // backs ViewLoopConfig.host_ip
    std::atomic<bool>                      stop_flag_{false};
    std::unique_ptr<vivora::ViewPlatform>  platform_;
    std::unique_ptr<ViewLoopState>         loop_;
    // VIV-22 clipboard sync while viewing.  Everything runs on the GUI
    // thread here (the view loop itself is QTimer-driven), but the shared
    // bridge keeps the wiring identical to the host side.
    std::shared_ptr<vivora::ClipboardBridge> clipboardBridge_;
    std::unique_ptr<ClipboardSync>            clipboardSync_;
    QTimer                                 tick_;
    ViewLoopConfig                         loop_cfg_{};
    bool                                   finished_emitted_ = false;
    // Background connect: loop_->init() (DNS + rendezvous + Noise handshake)
    // runs here so the GUI event loop stays responsive during a cold connect.
    std::thread                            connect_thread_;
    bool                                   everConnected_ = false;
    QString                                initError_;
    bool                                   everStreamed_ = false;
    ViewLoopState::ConnectFailure          connectFailure_ =
        ViewLoopState::ConnectFailure::None;
    // VIV-23 trust-prompt capture (see trustPromptPending above).
    bool    trustPending_  = false;
    bool    trustMismatch_ = false;
    QString trustPeerCode_;
    QString trustNewHex_;
    QString trustOldHex_;
};

} // namespace vivora::gui
