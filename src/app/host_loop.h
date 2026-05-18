#pragma once

#include "app/host_platform.h"
#include "host/encode/video_encoder.h"
#include <atomic>
#include <cstdint>
#include <functional>

namespace deskbeam {

struct HostLoopConfig {
    uint16_t port = 9876;
    uint32_t manual_bitrate_bps = 0;   // 0 = auto from resolution
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
};

// Run the host main loop.  Blocks until the client disconnects.
// All platform-specific work is delegated to |platform|.
int run_host_loop(HostPlatform& platform, const HostLoopConfig& cfg);

} // namespace deskbeam
