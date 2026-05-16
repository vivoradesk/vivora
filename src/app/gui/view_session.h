#pragma once

#ifdef DESKBEAM_WINDOWS

#include "app/view_loop.h"
#include "app/windows_view_platform.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <memory>
#include <string>

namespace deskbeam::gui {

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
    std::string relay_server;
    std::string relay_session_hex;
    std::string license_file;
};

// One active "Connect to peer" session in the GUI.
//
// Lives on the main thread.  Owns its WindowsViewPlatform (which owns
// the StreamWindow QWidget — also main-thread per Qt rules) and a
// ViewLoopState driven by a QTimer at ~60Hz.  Each tick runs one pass
// of the view-loop body: poll the UDP socket, drain FEC groups, feed
// decoded frames to the GPU, present.  Total budget per tick is on
// the order of 5-10ms — well under the 16ms slot, so the main-thread
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

signals:
    // Loop has fully torn down — session.stop(), platform.shutdown()
    // have run.  AppController removes us from its active-views list
    // on this signal.
    void finished();

private slots:
    void onTick();

private:
    GuiViewConfig                     cfg_;
    std::string                       host_ip_storage_;  // backs ViewLoopConfig.host_ip
    std::atomic<bool>                 stop_flag_{false};
    std::unique_ptr<WindowsViewPlatform> platform_;
    std::unique_ptr<ViewLoopState>    loop_;
    QTimer                            tick_;
    ViewLoopConfig                    loop_cfg_{};
    bool                              finished_emitted_ = false;
};

} // namespace deskbeam::gui

#endif // DESKBEAM_WINDOWS
