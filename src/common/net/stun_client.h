#pragma once

#include "common/net/socket.h"

#include <cstddef>
#include <cstdint>

namespace deskbeam::net {

// Minimal STUN client (RFC 5389 / 8489 subset). Sends a Binding Request on the
// caller-supplied UDP socket and parses the response's XOR-MAPPED-ADDRESS (or
// legacy MAPPED-ADDRESS) attribute — the socket's external "reflexive"
// address as seen by the public internet.
//
// The same socket is reused for the subsequent video/audio session, so the
// NAT binding the STUN server observed is exactly the binding the peer will
// reach. Allocating a fresh socket for STUN would discover a different
// binding on symmetric NATs and defeat the point.
class StunClient {
public:
    // Sends a Binding Request and waits up to `timeout_ms` for a matching
    // response. Returns SocketAddr{0,0} on timeout, transport error, or if the
    // response's transaction ID / magic cookie don't match.
    //
    // Blocks the calling thread. Do not call on hot paths — intended for
    // one-shot discovery at session startup.
    static SocketAddr discover(const SocketAddr& stun_server,
                               IUdpSocket& socket,
                               int timeout_ms = 1000);

    // For unit tests: parse a raw Binding Response buffer. Returns {0,0} on
    // any malformed field or TID mismatch. `expected_tid` must point to 12
    // bytes matching the TID used in the outbound request.
    static SocketAddr parse_binding_response(const uint8_t* buf, size_t len,
                                             const uint8_t* expected_tid);
};

} // namespace deskbeam::net
