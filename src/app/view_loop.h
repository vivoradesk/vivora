#pragma once

#include "app/view_platform.h"
#include <cstdint>

namespace deskbeam {

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
};

// Run the view (client) main loop.  Blocks until disconnected or window closed.
// Platform-specific decode/render is delegated to |platform|.
int run_view_loop(ViewPlatform& platform, const ViewLoopConfig& cfg);

} // namespace deskbeam
