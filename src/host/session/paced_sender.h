#pragma once

#include "common/net/socket.h"
#include "common/utils/spsc_ring.h"

#include <chrono>
#include <cstdint>
#include <memory>

namespace vivora::host {

// Decoupled, paced UDP sender (VIV-82).  send_to() enqueues an already-sealed
// wire packet; pump() — called very frequently from the host loop's existing
// idle spin — drains the queue at a fixed inter-packet gap so a frame's packets
// go out spread over time instead of as a micro-burst the WiFi AP drops ~20%
// of.
//
// Why pump-from-the-loop and not a send thread: fine (sub-millisecond) pacing
// needs precise timing.  A separate thread either busy-spins (and starves the
// single-threaded capture/encode loop) or sleeps (Windows ~1-3ms granularity →
// drains far too slowly → queue overflows → catastrophic drop).  The host loop
// ALREADY spins between frames waiting for the capture interval; draining a few
// due packets per spin iteration is free, single-threaded (no socket-send/recv
// race), and time-gated for precise spacing.
class PacedSender {
public:
    // gap_us = target inter-packet spacing.
    PacedSender(net::IUdpSocket& socket, int gap_us);

    PacedSender(const PacedSender&) = delete;
    PacedSender& operator=(const PacedSender&) = delete;

    // Enqueue a packet.  Returns len on success, -1 if the queue is full
    // (dropped).  Host-loop thread only.
    int send_to(const uint8_t* data, size_t len, const net::SocketAddr& dest);

    // Drain every packet whose scheduled send time has arrived.  Call as often
    // as possible from the host loop (idle spin + after each send); pacing
    // accuracy tracks the call frequency.
    void pump();

    // Flush anything still queued straight to the socket (no pacing) — used at
    // teardown so the last packets aren't stranded.
    void flush();

    uint64_t dropped() const { return dropped_; }
    uint64_t sent()    const { return sent_; }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t kRingCap = 4096;  // absorbs the 1000-pkt BW probe
    static constexpr size_t kMaxPkt  = 1500;
    struct Pkt {
        net::SocketAddr dest;
        uint16_t        len = 0;
        uint8_t         data[kMaxPkt];
    };

    net::IUdpSocket&  socket_;
    int               gap_us_;
    std::unique_ptr<util::SpscRing<Pkt, kRingCap>> ring_;
    Clock::time_point paced_next_{};
    bool              next_init_ = false;
    uint64_t          dropped_ = 0;
    uint64_t          sent_    = 0;
    Clock::time_point last_log_{};
    uint64_t          sent_window_ = 0;
};

} // namespace vivora::host
