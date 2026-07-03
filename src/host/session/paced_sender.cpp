#include "host/session/paced_sender.h"
#include "common/utils/log.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef VIVORA_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "winmm.lib")  // timeBeginPeriod
#else
#include <ctime>
#endif

namespace vivora::host {

namespace {

// Accurate short sleep for pacing, without busy-spinning.
#ifdef VIVORA_WINDOWS
struct HrSleeper {
    HANDLE h = nullptr;
    bool   high_res = false;
    HrSleeper() {
        // HIGH_RESOLUTION needs Win10 1803+; fall back to a normal timer (which
        // still benefits from the process-wide timeBeginPeriod set below) if
        // unavailable.
        h = CreateWaitableTimerExW(nullptr, nullptr,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        high_res = (h != nullptr);
        if (!h) h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        log::info("PacedSender", "wait timer: %s",
                  high_res ? "high-resolution" : (h ? "normal (fallback)" : "FAILED"));
    }
    ~HrSleeper() { if (h) CloseHandle(h); }
    void wait_us(int us) {
        if (us <= 0) return;
        if (!h) return;
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(us) * 10;  // 100 ns units, relative
        if (SetWaitableTimer(h, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(h, INFINITE);
    }
};
#else
struct HrSleeper {
    void wait_us(int us) {
        if (us <= 0) return;
        struct timespec ts;
        ts.tv_sec  = us / 1'000'000;
        ts.tv_nsec = (us % 1'000'000) * 1000L;
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
    }
};
#endif

} // namespace

PacedSender::PacedSender(net::IUdpSocket& socket, int gap_us)
    : socket_(socket), gap_us_(gap_us) {
    ring_ = std::make_unique<util::SpscRing<Pkt, kRingCap>>();
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&PacedSender::thread_proc, this);
}

PacedSender::~PacedSender() { stop(); }

void PacedSender::stop() {
    if (running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
    }
}

int PacedSender::send_to(const uint8_t* data, size_t len,
                         const net::SocketAddr& dest) {
    if (len > kMaxPkt) return socket_.send_to(data, len, dest);  // oversized
    Pkt p;
    p.dest = dest;
    p.len  = static_cast<uint16_t>(len);
    std::memcpy(p.data, data, len);
    if (!ring_->try_push(p)) {
        const uint64_t n = dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & 0x3FF) == 1)
            log::warn("PacedSender", "send queue full — dropped %llu packets",
                      static_cast<unsigned long long>(n));
        return -1;
    }
    return static_cast<int>(len);
}

void PacedSender::thread_proc() {
#ifdef VIVORA_WINDOWS
    timeBeginPeriod(1);  // backstop accuracy if the timer fell back to normal
#endif
    HrSleeper sleeper;
    Pkt p;
    uint64_t sent = 0, sent_window = 0;
    size_t max_q = 0;
    auto last_log = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire)) {
        // Send a small chunk, then one accurate wait covering the chunk's
        // worth of gap.  Chunking cuts the per-packet wait overhead (which
        // alone couldn't keep up) while keeping bursts small enough for the AP.
        constexpr int kChunk = 8;
        int n = 0;
        while (n < kChunk && ring_->try_pop(p)) {
            socket_.send_to(p.data, p.len, p.dest);
            ++sent; ++sent_window; ++n;
        }
        max_q = std::max(max_q, ring_->size());
        if (n == 0) {
            sleeper.wait_us(200);  // idle: brief accurate nap, no spin
            continue;
        }
        sleeper.wait_us(gap_us_ * n);  // pace: spread to the next chunk

        const auto t = std::chrono::steady_clock::now();
        if (t - last_log >= std::chrono::seconds(1)) {
            log::info("PacedSender", "sent=%llu/s total=%llu dropped=%llu max_q=%zu gap=%dus",
                      (unsigned long long)sent_window, (unsigned long long)sent,
                      (unsigned long long)dropped_.load(std::memory_order_relaxed),
                      max_q, gap_us_);
            last_log = t; sent_window = 0; max_q = 0;
        }
    }
#ifdef VIVORA_WINDOWS
    timeEndPeriod(1);
#endif
}

} // namespace vivora::host
