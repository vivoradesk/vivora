#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace vivora::util {

// Multi-producer / single-consumer bounded lock-free queue (Vyukov's
// bounded MPMC algorithm; safe for the MPSC use here).  Used for the
// threaded client's send funnel (VIV-81): input events come from the GUI
// thread, NACK/IDR/FecReport/PerfReport from the net/decode threads, and the
// single net thread drains and seals them — so send_cs_ has exactly one
// toucher and there are no nonce races.
//
// Capacity must be a power of two.  Each cell carries a sequence number that
// gates producers and the consumer without locks.  Enqueue may be called
// from any number of threads; dequeue from one.
template <typename T, size_t Capacity>
class MpscQueue {
    static_assert(Capacity >= 2, "Capacity must be >= 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    MpscQueue() {
        for (size_t i = 0; i < Capacity; ++i)
            cells_[i].seq.store(i, std::memory_order_relaxed);
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }
    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    // Any-thread producer.  Returns false when full.
    bool try_enqueue(const T& v) {
        Cell* cell;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & (Capacity - 1)];
            const size_t seq = cell->seq.load(std::memory_order_acquire);
            const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (dif == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,
                                                       std::memory_order_relaxed))
                    break;
            } else if (dif < 0) {
                return false;  // full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = v;
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Single-consumer dequeue.  Returns false when empty.
    bool try_dequeue(T& out) {
        Cell* cell;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & (Capacity - 1)];
            const size_t seq = cell->seq.load(std::memory_order_acquire);
            const intptr_t dif =
                static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (dif == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1,
                                                       std::memory_order_relaxed))
                    break;
            } else if (dif < 0) {
                return false;  // empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        out = cell->data;
        cell->seq.store(pos + Capacity, std::memory_order_release);
        return true;
    }

    static constexpr size_t capacity() { return Capacity; }

private:
    struct Cell {
        std::atomic<size_t> seq;
        T                   data;
    };
    alignas(64) Cell cells_[Capacity];
    alignas(64) std::atomic<size_t> enqueue_pos_;
    alignas(64) std::atomic<size_t> dequeue_pos_;
};

} // namespace vivora::util
