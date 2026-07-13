#pragma once

#include "app/clipboard_bridge.h"
#include "app/host_platform.h"
#include "host/encode/video_encoder.h"
#include "host/session/host_approval_gate.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace vivora {

struct HostLoopConfig {
    uint16_t port = 9876;
    uint32_t manual_bitrate_bps = 0;   // 0 = auto from resolution
    // VIV-67 stream framerate cap.  The host paces capture+encode (and the
    // static-screen heartbeat) at most this fast, and it is the ceiling the
    // client-driven adaptive framerate works under — adaptation may lower
    // the effective rate, never raise it above this.  0 falls back to 60.
    uint16_t max_fps = 60;
    EncoderKind encoder_kind = EncoderKind::Auto;
    VideoCodec  codec = VideoCodec::HEVC;
    // GUI mode hooks.  When non-null:
    //   stop_flag       — loop exits the next iteration when set true.
    //                     Also disables the CLI-mode auto-exit on
    //                     "all clients disconnected" (GUI keeps listening).
    //   client_count_out — written every iteration with current attached
    //                     client count, so the UI can poll it cheaply.
    //   state_out       — 0 = idle/listening, 1 = at least one client connected.
    std::atomic<bool>* stop_flag        = nullptr;
    std::atomic<int>*  client_count_out = nullptr;
    std::atomic<int>*  state_out        = nullptr;
    // STUN server "host:port" for reflexive-address discovery.  Empty string
    // disables STUN (LAN-only). Hostnames are resolved via getaddrinfo.
    const char* stun_server = nullptr;
    // Optional rendezvous server "host:port".  When set, the host registers
    // its long-term pubkey at the rendezvous so peers can locate it by id.
    const char* rendezvous_server = nullptr;
    // Optional relay endpoint + 32-byte session id (64 hex chars). When
    // both are set, host BINDs at the relay and routes every client-bound
    // packet through DBRL DATA. Manual flag for now (testing); becomes
    // automatic when direct hole-punching times out in a later iteration.
    const char* relay_server  = nullptr;
    const char* relay_session_hex = nullptr;
    // Path to a 95-byte license token file, attached to the relay BIND.
    // Required by Pro-managed relay (--require-license); ignored by
    // self-host instances.
    const char* license_file = nullptr;

    // Idle-timeout (GUI Phase B).  When clients sit silent for
    // `idle_timeout_min` minutes, host_loop fires `on_idle_warning`
    // (typically a tray toast).  If they stay silent another
    // `idle_warning_sec` seconds, host_loop force-disconnects every
    // attached client.  idle_timeout_min == 0 disables the whole
    // feature (CLI default).
    int  idle_timeout_min = 0;
    int  idle_warning_sec = 30;
    std::function<void(int seconds_until_disconnect)> on_idle_warning;

    // Per-client connection approval (VIV-53).  When non-null, every
    // new client that finishes its handshake lands in Pending state
    // and won't receive frames until the GUI sets it Approved via
    // gate.set_state().  CLI mode leaves this null → all clients are
    // implicitly approved (matches pre-VIV-53 behaviour).
    std::shared_ptr<host::HostApprovalGate> approval_gate;

    // VIV-53 Refresh button (GUI Phase B).  Setting this atomic
    // forces an immediate rendezvous re-registration on the next
    // poll iteration.  Cleared by the loop after firing.
    std::atomic<bool>* rendezvous_refresh_flag = nullptr;

    // VIV-22 clipboard sync (GUI mode only).  When set, each loop tick
    // drains the bridge's outbound slot into session.send_clipboard() and
    // pushes any reassembled viewer clipboard into the inbound slot for
    // the GUI-thread ClipboardSync to apply.  CLI mode leaves this null
    // (no QClipboard without a GUI event loop) → feature off.
    std::shared_ptr<ClipboardBridge> clipboard;
};

// Run the host main loop.  Blocks until the client disconnects.
// All platform-specific work is delegated to |platform|.
int run_host_loop(HostPlatform& platform, const HostLoopConfig& cfg);

} // namespace vivora
