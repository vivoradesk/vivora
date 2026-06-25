#include "host/session/paced_sender.h"
#include "common/utils/log.h"

#include <cstring>

namespace vivora::host {

PacedSender::PacedSender(net::IUdpSocket& socket, int gap_us)
    : socket_(socket), gap_us_(gap_us) {
    ring_ = std::make_unique<util::SpscRing<Pkt, kRingCap>>();
}

int PacedSender::send_to(const uint8_t* data, size_t len,
                         const net::SocketAddr& dest) {
    if (len > kMaxPkt) return socket_.send_to(data, len, dest);  // oversized
    Pkt p;
    p.dest = dest;
    p.len  = static_cast<uint16_t>(len);
    std::memcpy(p.data, data, len);
    if (!ring_->try_push(p)) {
        ++dropped_;
        if ((dropped_ & 0x3FF) == 1)
            log::warn("PacedSender", "send queue full — dropped %llu packets",
                      static_cast<unsigned long long>(dropped_));
        return -1;
    }
    return static_cast<int>(len);
}

void PacedSender::pump() {
    const auto now = Clock::now();
    if (!next_init_) { paced_next_ = now; next_init_ = true; }

    // Clamp the schedule so a long gap (e.g. during capture+encode, when pump
    // isn't called) doesn't make a whole backlog "due" at once and burst.  At
    // most one packet is due per real gap_us elapsed; the queued backlog then
    // drains one-per-gap as real time advances — paced, never bursted.
    const auto gap = std::chrono::microseconds(gap_us_);
    if (paced_next_ < now - gap) paced_next_ = now - gap;

    Pkt p;
    while (paced_next_ <= now) {
        if (!ring_->try_pop(p)) { paced_next_ = now; break; }  // empty
        socket_.send_to(p.data, p.len, p.dest);
        ++sent_; ++sent_window_;
        paced_next_ += gap;
    }

    // Diagnostic: once/sec, report drain rate / drops / queue depth.
    if (last_log_.time_since_epoch().count() == 0) last_log_ = now;
    if (now - last_log_ >= std::chrono::seconds(1)) {
        log::info("PacedSender", "sent=%llu/s total=%llu dropped=%llu q=%zu gap=%dus",
                  (unsigned long long)sent_window_, (unsigned long long)sent_,
                  (unsigned long long)dropped_, ring_->size(), gap_us_);
        last_log_ = now; sent_window_ = 0;
    }
}

void PacedSender::flush() {
    Pkt p;
    while (ring_->try_pop(p)) {
        socket_.send_to(p.data, p.len, p.dest);
        ++sent_;
    }
}

} // namespace vivora::host
