// Unit tests for the lock-free pipeline primitives (VIV-81):
//   - SpscRing  : FIFO order, full/empty edges, the "render penultimate"
//                 consume policy used by Q2 (decode->render).
//   - MpscQueue : many producers + one consumer, no loss / no duplicates
//                 (the send funnel).
#include "common/utils/spsc_ring.h"
#include "common/utils/mpsc_queue.h"

#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

using namespace vivora::util;

static int g_failures = 0;
#define CHECK(cond, msg)                              \
    do {                                              \
        if (!(cond)) { std::printf("FAIL: %s\n", (msg)); ++g_failures; } \
    } while (0)

// SPSC full/empty boundaries + drop-newest-on-full.
static void test_spsc_full_empty() {
    SpscRing<int, 4> r;
    int x;
    CHECK(!r.try_pop(x), "pop on empty -> false");
    CHECK(r.try_push(1) && r.try_push(2) && r.try_push(3) && r.try_push(4),
          "fill to capacity");
    CHECK(!r.try_push(5), "push when full -> false (drop-newest)");
    CHECK(r.size() == 4, "size == capacity when full");
    CHECK(r.try_pop(x) && x == 1, "FIFO pop returns oldest");
    CHECK(r.try_push(5), "push succeeds after a pop");
}

// Q2 "render penultimate" policy: drop oldest until <=2 remain, pop one
// (the penultimate when two were queued) to render, keep the newest as a
// one-frame cushion.
static void test_penultimate_policy() {
    SpscRing<int, 4> r;
    auto consume = [&](int& rendered, bool& had, int& dropped_count) {
        dropped_count = 0;
        while (r.size() > 2) { int d; r.try_pop(d); ++dropped_count; }
        int v; had = r.try_pop(v); if (had) rendered = v;
    };

    // Queue 10(old) 20 30(new): drop 10, render 20 (penultimate), keep 30.
    r.try_push(10); r.try_push(20); r.try_push(30);
    int rendered = -1, dropped = -1; bool had = false;
    consume(rendered, had, dropped);
    CHECK(had && rendered == 20, "renders penultimate (20)");
    CHECK(dropped == 1, "drops one stale frame (10)");
    CHECK(r.size() == 1, "keeps newest as cushion");
    int cushion = -1; r.try_pop(cushion);
    CHECK(cushion == 30, "cushion is the newest frame (30)");

    // Single queued frame: render it, nothing dropped, no cushion left.
    r.try_push(99);
    consume(rendered, had, dropped);
    CHECK(had && rendered == 99 && dropped == 0, "single frame renders, no drop");
    CHECK(r.empty(), "empty after consuming the only frame");
}

// SPSC under real concurrency: producer 0..N-1, consumer must see every
// value exactly once, in order.
static void test_spsc_concurrent() {
    SpscRing<uint32_t, 1024> ring;
    const uint32_t N = 2'000'000;
    std::thread prod([&] {
        for (uint32_t i = 0; i < N;) {
            if (ring.try_push(i)) ++i;  // spin while full
        }
    });
    uint32_t expected = 0, got = 0;
    bool order_ok = true;
    while (got < N) {
        uint32_t v;
        if (ring.try_pop(v)) {
            if (v != expected) order_ok = false;
            ++expected;
            ++got;
        }
    }
    prod.join();
    CHECK(order_ok, "SPSC preserves FIFO order under concurrency");
    CHECK(got == N, "SPSC delivers all items");
    CHECK(ring.empty(), "SPSC ring empty at end");
}

// MPSC: several producers each emit a disjoint range; the single consumer
// must collect every value exactly once.
static void test_mpsc_concurrent() {
    MpscQueue<uint64_t, 4096> q;
    const int      PRODUCERS = 4;
    const uint64_t PER       = 500'000;
    const uint64_t TOTAL     = static_cast<uint64_t>(PRODUCERS) * PER;

    std::vector<std::thread> prods;
    for (int p = 0; p < PRODUCERS; ++p) {
        prods.emplace_back([&, p] {
            const uint64_t base = static_cast<uint64_t>(p) * PER;
            for (uint64_t i = 0; i < PER;) {
                if (q.try_enqueue(base + i)) ++i;  // spin while full
            }
        });
    }

    std::vector<uint8_t> seen(TOTAL, 0);
    uint64_t got = 0;
    bool bad = false;
    while (got < TOTAL) {
        uint64_t v;
        if (q.try_dequeue(v)) {
            if (v >= TOTAL || seen[v]) bad = true;
            else seen[v] = 1;
            ++got;
        }
    }
    for (auto& t : prods) t.join();

    CHECK(!bad, "MPSC: no duplicates / no out-of-range values");
    CHECK(got == TOTAL, "MPSC delivers all items");
    bool complete = true;
    for (uint64_t i = 0; i < TOTAL; ++i)
        if (!seen[i]) complete = false;
    CHECK(complete, "MPSC: every value received exactly once");
}

int main() {
    test_spsc_full_empty();
    test_penultimate_policy();
    test_spsc_concurrent();
    test_mpsc_concurrent();
    if (g_failures == 0) {
        std::printf("lockfree_test: ALL PASS\n");
        return 0;
    }
    std::printf("lockfree_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
