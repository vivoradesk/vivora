#pragma once

#include "common/net/socket.h"
#include "common/utils/spsc_ring.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace vivora::host {

// Decoupled, paced UDP sender (VIV-82).  The host loop enqueues already-sealed
// wire packets via send_to(); a dedicated thread drains the lock-free SPSC ring
// one packet per `gap_us`, so a frame's packets go out spread over time instead
// of as a micro-burst the WiFi AP drops ~20% of.
//
// Pacing accuracy uses a high-resolution OS timer (Win32
// CreateWaitableTimerEx HIGH_RESOLUTION / POSIX clock_nanosleep) — NOT a
// busy-spin (which starved the single-threaded capture/encode loop) and NOT
// std::this_thread::sleep (Windows ~1-3 ms granularity drained far too slowly).
// Sealing stays on the host loop, so cipher nonce order == enqueue order ==
// FIFO ring order == wire order.  Strict SPSC: producer = host loop, consumer
// = the send thread.
class PacedSender {
public:
    PacedSender(net::IUdpSocket& socket, int gap_us);
    ~PacedSender();

    PacedSender(const PacedSender&) = delete;
    PacedSender& operator=(const PacedSender&) = delete;

    // Enqueue a packet.  Returns len on success, -1 if the queue is full
    // (dropped).  Host-loop thread only.
    int send_to(const uint8_t* data, size_t len, const net::SocketAddr& dest);

    void stop();
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    void thread_proc();

    static constexpr size_t kRingCap = 4096;  // absorbs the 1000-pkt BW probe
    static constexpr size_t kMaxPkt  = 1500;
    struct Pkt {
        net::SocketAddr dest;
        uint16_t        len = 0;
        uint8_t         data[kMaxPkt];
    };

    net::IUdpSocket&      socket_;
    int                   gap_us_;
    std::unique_ptr<util::SpscRing<Pkt, kRingCap>> ring_;
    std::thread           thread_;
    std::atomic<bool>     running_{false};
    std::atomic<uint64_t> dropped_{0};
};

} // namespace vivora::host
