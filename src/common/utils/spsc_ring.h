#pragma once

#include <atomic>
#include <cstddef>

namespace vivora::util {

// Single-producer / single-consumer bounded lock-free ring buffer.
//
// Exactly one thread may call the producer side (try_push) and exactly one
// (different) thread may call the consumer side (try_pop).  size()/empty()
// are safe to read from either side as an instantaneous estimate; on the
// consumer side, between its own pop calls, size() is exact (the producer
// only grows it), which is what the "render penultimate" policy in the
// threaded client pipeline (VIV-81) relies on.
//
// Capacity must be a power of two.  Head/tail are free-running counters
// (size_t), indexed modulo Capacity; their unsigned difference is the live
// count and wraps correctly.  Producer and consumer indices sit on separate
// cache lines to avoid false sharing on the hot path.
template <typename T, size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2, "Capacity must be >= 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    SpscRing() : head_(0), tail_(0) {}
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Producer side.  Returns false when full — the caller picks the drop
    // policy (Q1: drop-newest + request IDR).
    bool try_push(const T& v) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= Capacity)
            return false;  // full
        buf_[head & (Capacity - 1)] = v;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side.  Returns false when empty.
    bool try_pop(T& out) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;  // empty
        out = buf_[tail & (Capacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Live element count.  Exact on the consumer thread between its pops.
    size_t size() const {
        return head_.load(std::memory_order_acquire)
             - tail_.load(std::memory_order_acquire);
    }
    bool empty() const { return size() == 0; }
    static constexpr size_t capacity() { return Capacity; }

private:
    // 64-byte separation so the producer's head and the consumer's tail
    // don't share a cache line (false sharing would serialise them).
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
    alignas(64) T buf_[Capacity];
};

} // namespace vivora::util
