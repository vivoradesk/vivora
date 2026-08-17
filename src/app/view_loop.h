#pragma once

#include "app/clipboard_bridge.h"
#include "app/view_platform.h"
#include "app/video_pipeline.h"
#include "client/net/client_session.h"
#include "common/utils/spsc_ring.h"
#include "common/utils/types.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace vivora {

struct ViewLoopConfig {
    const char* host_ip = nullptr;
    uint16_t    port    = 9876;
    // STUN server "host:port" for reflexive-address discovery.  Empty string
    // disables STUN (LAN-only). Hostnames are resolved via getaddrinfo.
    const char* stun_server = nullptr;
    // Host's long-term Curve25519 public key, as 64 lowercase hex chars.
    // The host logs its own pubkey on startup — paste that here (or scan QR).
    // Required: Noise_NK refuses to start without this pin.
    const char* host_key_hex = nullptr;
    // Optional rendezvous server "host:port".  When set together with --peer,
    // the client looks up the host's reflexive endpoint at the rendezvous
    // and connects to whatever it returns; --view IP becomes a fallback.
    const char* rendezvous_server = nullptr;
    // Same hex format as --host-key; identifies which host to look up.
    const char* peer_pubkey_hex = nullptr;
    // Optional relay endpoint + agreed session id (64 hex chars).
    const char* relay_server  = nullptr;
    const char* relay_session_hex = nullptr;
    const char* license_file = nullptr;
    // GUI mode hook: when non-null, iter() exits the loop at the next
    // chance.  CLI ignores it (uses Ctrl+C / window-close instead).
    std::atomic<bool>* stop_flag = nullptr;
    // VIV-23 GUI mode hook: interactive TOFU.  When true, an unknown or
    // changed peer pin makes init() fail with a TrustPending (see
    // trust_pending() below) instead of auto-pinning / hard-refusing.
    // CLI leaves this false → legacy behaviour.
    bool interactive_trust = false;
    // VIV-22 clipboard sync (GUI mode only).  When set, each iter() drains
    // the bridge's outbound slot into session.send_clipboard() and pushes
    // any reassembled host clipboard into the inbound slot for the
    // GUI-thread ClipboardSync.  CLI leaves this null → feature off.
    std::shared_ptr<ClipboardBridge> clipboard;
    // User-configured viewing caps (Settings → Viewing; 0 = none).
    // fps cap bounds the client's PerfReport ratchet; the bitrate cap is
    // forwarded to the host inside PerfReport as a hard controller clamp.
    uint16_t view_fps_cap  = 0;
    uint32_t view_max_kbps = 0;
    // VIV-54 auto-reconnect budget in milliseconds (client_reconnect_timeout).
    // 0 = disabled (Connected → Disconnected on host timeout, the legacy CLI
    // behaviour).  The GUI populates this from Settings (default 5 min).
    uint32_t reconnect_timeout_ms = 0;
    // Display label for the reconnect banner ("Reconnecting to <peer>…").
    // Empty falls back to a generic wording.
    const char* peer_label = nullptr;
};

// Iterable view-loop state machine.  Split out of the legacy
// while(true) so the GUI can drive one iteration per QTimer tick
// from the main thread (where StreamWindow lives), while the CLI
// keeps its own tight while-loop via run_view_loop() below.
//
// Lifecycle:
//   ViewLoopState s;
//   if (!s.init(platform, cfg)) ...   // returns false if setup fails
//   while (s.iter()) {}               // false = clean exit / disconnect
//   int rc = s.exit_code();
class ViewLoopState {
public:
    ViewLoopState();
    ~ViewLoopState();

    // Do one-time setup: parse keys, configure session, STUN, rendezvous,
    // relay, start the UDP socket, wire input callback.  Returns false on
    // any fatal misconfiguration; check exit_code() to map to a process
    // exit status.
    bool init(ViewPlatform& platform, const ViewLoopConfig& cfg);

    // Run one iteration of the view loop body.  Returns false when the
    // user closed the window, the session disconnected, or stop_flag
    // was set.  Cleanup (session.stop, platform.shutdown) is deferred
    // to the destructor so callers can re-query exit_code afterwards.
    bool iter();

    int exit_code() const { return exit_code_; }

    // Live stats for the GUI; updated in iter().
    uint64_t frames_decoded() const { return frames_decoded_; }
    double   rtt_ms() const;
    client::SessionState state() const;

    // VIV-23: after init() returned false, true when the failure was a
    // TOFU trust question (first connect / key changed) rather than a real
    // error.  `out` receives the details for the GUI dialog.
    bool trust_pending(client::TrustPending& out) const {
        if (!session_.has_trust_pending()) return false;
        out = session_.trust_pending();
        return true;
    }

private:
    // Min interval between IDR requests.  Was a fixed 600ms — but under frequent
    // loss that throttle IS the freeze floor: a loss shortly after an IDR can't
    // re-request for 600ms.  Now env-tunable (VIVORA_IDR_INTERVAL_MS), default
    // 250ms, so recovery retries ~2.5x faster (VIV-82).  The recovery keyframe
    // is heavily FEC-boosted (kf_m up to 24) so it survives to make this pay off.
    static int min_idr_interval_ms();

    // Set once in init(); read on every iter().
    ViewPlatform*     platform_ = nullptr;
    const ViewLoopConfig* cfg_  = nullptr;

    // Session is constructed in init(); freed on destruction.
    client::ClientSession session_;

    // Loop-local state lifted from the original function locals.
    bool      got_keyframe_      = false;
    uint64_t  frames_decoded_    = 0;
    uint64_t  last_drops_        = 0;
    TimePoint last_idr_request_{};
    TimePoint last_log_time_{};
    uint64_t  last_log_frames_   = 0;
    uint64_t  last_arrived_count_ = 0;
    float     last_arrived_fps_  = 0.0f;
    bool      decoder_ready_     = false;
    // VIV-112 runtime fallback: set once we've asked the host to downgrade the
    // codec after a decoder-init failure, so we try the downgrade at most once
    // and don't loop if H.264 also fails to init.
    bool      codec_downgrade_tried_ = false;
    bool      audio_started_     = false;
    int       exit_code_         = 0;
    bool      torn_down_         = false;

    // In-stream menu state (VIV-74).  view_only_ gates the input callback so
    // the user can watch without their mouse/keyboard reaching the host;
    // user_disconnect_ is set by the menu's Disconnect button and ends the
    // loop on the next iter().  Atomic: the input callback may fire from the
    // platform's event pump while the menu toggles from the UI thread.
    std::atomic<bool> view_only_{false};
    std::atomic<bool> user_disconnect_{false};

    // Session uptime for the menu header pill (VIV-74) — stamped once the
    // session first reaches Connected.
    TimePoint session_start_{};
    bool      session_started_ = false;

    // Status-overlay driving (VIV-62): "Connecting…" / "Waiting for host to
    // accept…" before the first frame, and a brief close reason when a
    // session that never produced a frame disconnects.
    std::string status_shown_;               // last text pushed (content dedup)
    bool        disconnecting_  = false;
    TimePoint   disconnect_at_{};
    static constexpr int DISCONNECT_LINGER_MS = 1800;
    void update_status(const char* text);

    // VIV-54 auto-reconnect UI driving.  peer_label_ backs the banner text;
    // prev_state_ lets iter() spot the Reconnecting → Connected edge so it can
    // drop the stale reference chain and request an IDR to resume rendering.
    std::string          peer_label_;
    client::SessionState prev_state_ = client::SessionState::Disconnected;
    // Push the "Reconnecting to <peer> — attempt N (Ns)…" banner and, on the
    // resume edge, request the IDR.  Returns true while the loop should keep
    // the window open and skip normal frame processing (i.e. we're mid-
    // reconnect).  Shared by iter() and iter_threaded().
    bool handle_reconnect_ui();

    // ---- Threaded pipeline (VIV-81; VIVORA_PIPELINE=threaded) -------------
    // When on, a decode thread pulls compressed frames from q1_, runs them
    // through the platform's IVideoPipeline, and parks decoded handles in q2_.
    // The main loop (iter_threaded) hands compressed frames to q1_ and drains
    // q2_ with the render-penultimate policy to present.  Legacy iter() is
    // untouched.  pipeline_ is owned by the platform; null → fall back to
    // legacy even if the flag is set (e.g. platforms without an impl).
    struct CompressedFrame {
        std::vector<uint8_t> data;   // ring slot keeps capacity across pushes
        uint32_t timestamp = 0;
        uint16_t seq       = 0;
        bool     keyframe  = false;
        bool     heartbeat = false;
        bool     discontinuity = false;  // VIV-82: refs broken (frame dropped before)
    };
    bool                 threaded_ = false;
    IVideoPipeline*      pipeline_ = nullptr;
    std::unique_ptr<util::SpscRing<CompressedFrame, 8>> q1_;  // main → decode
    std::unique_ptr<util::SpscRing<FrameHandle, 4>>     q2_;  // decode → main
    std::thread          decode_thread_;
    std::atomic<bool>    decode_running_{false};
    std::atomic<bool>    decode_needs_idr_{false};  // decode → main: reject
    bool                 iter_threaded();
    void                 decode_thread_proc();

    void teardown();
};

// CLI-side thin wrapper.  Blocks until ViewLoopState::iter() returns false.
int run_view_loop(ViewPlatform& platform, const ViewLoopConfig& cfg);

} // namespace vivora
