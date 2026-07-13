// fec_test.cpp — unit tests for Reed-Solomon FEC encoder/decoder.
// Covers: single-erasure recovery, multi-erasure recovery up to M,
// variable-length shards, partial-group flush, no-loss fast path,
// mixed data+parity loss, and the "too many losses" give-up path.

#include "common/net/fec_codec.h"
#include "common/protocol/packet.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace vivora::net;
using namespace vivora::protocol;

// Build a synthetic Video data wire packet with a unique seq_no and a
// deterministic payload of `payload_len` bytes.
static std::vector<uint8_t> make_data_wire(uint16_t seq_no, uint32_t timestamp,
                                           size_t payload_len, uint8_t seed) {
    std::vector<uint8_t> wire(PacketHeader::WIRE_SIZE + payload_len);

    PacketHeader hdr;
    hdr.type = PacketType::Video;
    hdr.seq_no = seq_no;
    hdr.timestamp = timestamp;
    hdr.flags = FLAG_NONE;
    hdr.payload_len = static_cast<uint16_t>(payload_len);
    hdr.serialize(wire.data());

    for (size_t i = 0; i < payload_len; ++i) {
        wire[PacketHeader::WIRE_SIZE + i] =
            static_cast<uint8_t>((seed + i * 31) & 0xFF);
    }
    return wire;
}

// Build a synthetic FRAGMENT data wire: one frame_seq, fragment index `frag`
// written as the first 2 payload bytes (so wire_pkt_key() = seq<<16|frag, i.e.
// consecutive keys — required by ranged FEC).
static std::vector<uint8_t> make_frag_wire(uint16_t seq, uint16_t frag,
                                           uint32_t timestamp, size_t payload_len,
                                           uint8_t seed) {
    std::vector<uint8_t> wire(PacketHeader::WIRE_SIZE + payload_len);
    PacketHeader hdr;
    hdr.type = PacketType::Video;
    hdr.seq_no = seq;
    hdr.timestamp = timestamp;
    hdr.flags = FLAG_FRAGMENT;
    hdr.payload_len = static_cast<uint16_t>(payload_len);
    hdr.serialize(wire.data());
    uint8_t* pl = wire.data() + PacketHeader::WIRE_SIZE;
    pl[0] = static_cast<uint8_t>(frag & 0xFF);
    pl[1] = static_cast<uint8_t>(frag >> 8);
    for (size_t i = 2; i < payload_len; ++i)
        pl[i] = static_cast<uint8_t>((seed + i * 31) & 0xFF);
    return wire;
}

// Craft an FEC parity wire packet with fully-controlled K/M/parity_idx/group_id
// (the encoder never emits a mismatched group, so we build malformed ones by
// hand to exercise the decoder's robustness).  Layout mirrors fec_codec.cpp:
//   group_id(2 LE) | K(1) | M(1) | parity_idx(1) | keys(4*K) | lens(2*K) | shard
static std::vector<uint8_t> make_fec_wire(uint16_t group_id, uint8_t k, uint8_t m,
                                          uint8_t parity_idx, size_t shard_len,
                                          uint16_t frame_seq = 0) {
    constexpr size_t FEC_HEADER_FIXED = 5;  // group_id + K + M + idx
    const size_t payload = FEC_HEADER_FIXED
                         + static_cast<size_t>(k) * (4 + 2) + shard_len;
    std::vector<uint8_t> wire(PacketHeader::WIRE_SIZE + payload);

    PacketHeader hdr;
    hdr.type = PacketType::Video;
    hdr.seq_no = frame_seq;
    hdr.timestamp = 0;
    hdr.flags = FLAG_FEC;
    hdr.payload_len = static_cast<uint16_t>(payload);
    hdr.serialize(wire.data());

    uint8_t* p = wire.data() + PacketHeader::WIRE_SIZE;
    p[0] = static_cast<uint8_t>(group_id & 0xFF);
    p[1] = static_cast<uint8_t>(group_id >> 8);
    p += 2;
    *p++ = k;
    *p++ = m;
    *p++ = parity_idx;
    for (int j = 0; j < k; ++j) {
        uint32_t key = (static_cast<uint32_t>(frame_seq) << 16) | j;
        p[0] = static_cast<uint8_t>(key);
        p[1] = static_cast<uint8_t>(key >> 8);
        p[2] = static_cast<uint8_t>(key >> 16);
        p[3] = static_cast<uint8_t>(key >> 24);
        p += 4;
    }
    for (int j = 0; j < k; ++j) {
        uint16_t l = static_cast<uint16_t>(shard_len);
        p[0] = static_cast<uint8_t>(l & 0xFF);
        p[1] = static_cast<uint8_t>(l >> 8);
        p += 2;
    }
    for (size_t b = 0; b < shard_len; ++b)
        *p++ = static_cast<uint8_t>(b * 5 + parity_idx);
    return wire;
}

// Feed a set of wires through encoder, collecting all emitted FEC parity wires.
static std::vector<std::vector<uint8_t>>
encode_group(FecEncoder& enc,
             const std::vector<std::vector<uint8_t>>& data_wires,
             uint16_t frame_seq, uint32_t timestamp,
             bool flush_remaining)
{
    std::vector<std::vector<uint8_t>> fec_out;
    for (const auto& w : data_wires) {
        auto parts = enc.feed(w.data(), w.size(), frame_seq, timestamp);
        for (auto& p : parts) fec_out.push_back(std::move(p));
    }
    if (flush_remaining) {
        auto parts = enc.flush(frame_seq, timestamp);
        for (auto& p : parts) fec_out.push_back(std::move(p));
    }
    return fec_out;
}

// Count how many of the originally-sent wires appear (byte-equal) in the
// union of (delivered data + recovered from FEC).
static int verify_all_present(const std::vector<std::vector<uint8_t>>& originals,
                              const std::vector<std::vector<uint8_t>>& delivered,
                              const std::vector<std::vector<uint8_t>>& recovered) {
    int hits = 0;
    for (const auto& orig : originals) {
        bool found = false;
        for (const auto& d : delivered) {
            if (d == orig) { found = true; break; }
        }
        if (!found) {
            for (const auto& r : recovered) {
                if (r == orig) { found = true; break; }
            }
        }
        if (found) ++hits;
    }
    return hits;
}

// Test 1: K=4, M=1, lose 1 data packet → recover exactly that one.
static void test_single_erasure() {
    printf("  single erasure (K=4, M=1)...\n");
    FecEncoder enc;
    enc.set_group_size(4);
    enc.set_parity_count(1);

    std::vector<std::vector<uint8_t>> wires;
    for (int i = 0; i < 4; ++i)
        wires.push_back(make_data_wire(100 + i, 1000, 200, static_cast<uint8_t>(i)));

    auto fec = encode_group(enc, wires, 100, 1000, false);
    assert(fec.size() == 1);

    FecDecoder dec;
    // Deliver data packets 0, 1, 3 (drop 2), then FEC.
    std::vector<std::vector<uint8_t>> recovered;
    dec.feed(wires[0].data(), wires[0].size(), recovered);
    dec.feed(wires[1].data(), wires[1].size(), recovered);
    dec.feed(wires[3].data(), wires[3].size(), recovered);
    assert(recovered.empty());
    dec.feed(fec[0].data(), fec[0].size(), recovered);

    // In-line recovery (VIV-82): the decoder recovers the instant it holds
    // K shards — on the parity feed itself, not deferred to the next tick().
    assert(recovered.size() == 1);
    assert(recovered[0] == wires[2]);

    // A resolved group must not re-emit on tick.
    dec.tick(recovered);
    assert(recovered.size() == 1);
    printf("    PASS\n");
}

// Test 2: K=8, M=3, lose 3 data packets.
static void test_triple_erasure() {
    printf("  triple erasure (K=8, M=3)...\n");
    FecEncoder enc;
    enc.set_group_size(8);
    enc.set_parity_count(3);

    std::vector<std::vector<uint8_t>> wires;
    for (int i = 0; i < 8; ++i)
        wires.push_back(make_data_wire(200 + i, 2000, 500, static_cast<uint8_t>(i * 7)));

    auto fec = encode_group(enc, wires, 200, 2000, false);
    assert(fec.size() == 3);

    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;
    // Drop indices 1, 4, 6 — deliver the rest.
    std::vector<int> drop = {1, 4, 6};
    for (int i = 0; i < 8; ++i) {
        if (std::find(drop.begin(), drop.end(), i) != drop.end()) continue;
        dec.feed(wires[i].data(), wires[i].size(), recovered);
    }
    for (auto& f : fec)
        dec.feed(f.data(), f.size(), recovered);

    dec.tick(recovered);

    assert(recovered.size() == 3);
    // Each recovered packet must match exactly one dropped original.
    int matched = 0;
    for (int idx : drop) {
        for (const auto& r : recovered) {
            if (r == wires[idx]) { ++matched; break; }
        }
    }
    assert(matched == 3);
    printf("    PASS\n");
}

// Test 3: variable-length packets (different shard sizes inside one group).
static void test_variable_length() {
    printf("  variable-length shards (K=5, M=2)...\n");
    FecEncoder enc;
    enc.set_group_size(5);
    enc.set_parity_count(2);

    std::vector<std::vector<uint8_t>> wires;
    const size_t sizes[5] = {100, 350, 200, 1100, 50};
    for (int i = 0; i < 5; ++i)
        wires.push_back(make_data_wire(300 + i, 3000, sizes[i], static_cast<uint8_t>(i + 11)));

    auto fec = encode_group(enc, wires, 300, 3000, false);
    assert(fec.size() == 2);

    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;
    // Drop 0 (small) and 3 (large).
    dec.feed(wires[1].data(), wires[1].size(), recovered);
    dec.feed(wires[2].data(), wires[2].size(), recovered);
    dec.feed(wires[4].data(), wires[4].size(), recovered);
    dec.feed(fec[0].data(), fec[0].size(), recovered);
    dec.feed(fec[1].data(), fec[1].size(), recovered);

    dec.tick(recovered);

    assert(recovered.size() == 2);
    // Recovered bytes must equal originals *truncated back to original length*.
    bool got0 = false, got3 = false;
    for (const auto& r : recovered) {
        if (r == wires[0]) got0 = true;
        else if (r == wires[3]) got3 = true;
    }
    assert(got0 && got3);
    printf("    PASS\n");
}

// Test 4: partial flush — group closed early with only 3 of K=10 packets.
static void test_partial_flush() {
    printf("  partial flush (K_eff=3 with M=2)...\n");
    FecEncoder enc;
    enc.set_group_size(10);
    enc.set_parity_count(2);

    std::vector<std::vector<uint8_t>> wires;
    for (int i = 0; i < 3; ++i)
        wires.push_back(make_data_wire(400 + i, 4000, 250, static_cast<uint8_t>(i + 77)));

    // Feed 3 packets (less than K) then flush.
    auto fec = encode_group(enc, wires, 400, 4000, true);
    assert(fec.size() == 2);  // M parity packets

    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;
    // Drop packet 0 and 2 (2 losses, M=2 → recoverable).
    dec.feed(wires[1].data(), wires[1].size(), recovered);
    dec.feed(fec[0].data(), fec[0].size(), recovered);
    dec.feed(fec[1].data(), fec[1].size(), recovered);
    dec.tick(recovered);

    assert(recovered.size() == 2);
    bool got0 = false, got2 = false;
    for (const auto& r : recovered) {
        if (r == wires[0]) got0 = true;
        else if (r == wires[2]) got2 = true;
    }
    assert(got0 && got2);
    printf("    PASS\n");
}

// Test 5: all data packets received — no recovery needed, no spurious output.
static void test_no_loss() {
    printf("  no-loss fast path (K=6, M=2)...\n");
    FecEncoder enc;
    enc.set_group_size(6);
    enc.set_parity_count(2);

    std::vector<std::vector<uint8_t>> wires;
    for (int i = 0; i < 6; ++i)
        wires.push_back(make_data_wire(500 + i, 5000, 300, static_cast<uint8_t>(i + 3)));

    auto fec = encode_group(enc, wires, 500, 5000, false);

    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;
    for (auto& w : wires) dec.feed(w.data(), w.size(), recovered);
    for (auto& f : fec)   dec.feed(f.data(), f.size(), recovered);
    dec.tick(recovered);

    assert(recovered.empty());
    printf("    PASS\n");
}

// Test 6: mixed loss — drop 1 data + 1 parity with M=2 → still recoverable.
static void test_mixed_loss() {
    printf("  mixed data+parity loss (K=5, M=2, 1+1 loss)...\n");
    FecEncoder enc;
    enc.set_group_size(5);
    enc.set_parity_count(2);

    std::vector<std::vector<uint8_t>> wires;
    for (int i = 0; i < 5; ++i)
        wires.push_back(make_data_wire(600 + i, 6000, 400, static_cast<uint8_t>(i * 13)));

    auto fec = encode_group(enc, wires, 600, 6000, false);
    assert(fec.size() == 2);

    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;
    // Deliver 4 data (drop index 2), drop parity[0], deliver parity[1].
    for (int i = 0; i < 5; ++i) {
        if (i == 2) continue;
        dec.feed(wires[i].data(), wires[i].size(), recovered);
    }
    dec.feed(fec[1].data(), fec[1].size(), recovered);
    dec.tick(recovered);

    assert(recovered.size() == 1);
    assert(recovered[0] == wires[2]);
    printf("    PASS\n");
}

// Test 7: too many losses — M=2 but drop 3 data packets.  Should give up
// cleanly (no recovery, no crash) and mark group resolved.
static void test_too_many_losses() {
    printf("  too-many-losses give-up (K=5, M=2, drop 3)...\n");
    FecEncoder enc;
    enc.set_group_size(5);
    enc.set_parity_count(2);

    std::vector<std::vector<uint8_t>> wires;
    for (int i = 0; i < 5; ++i)
        wires.push_back(make_data_wire(700 + i, 7000, 350, static_cast<uint8_t>(i + 100)));

    auto fec = encode_group(enc, wires, 700, 7000, false);
    assert(fec.size() == 2);

    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;
    // Deliver only 2 data packets + both parities (total=4, need K=5).
    dec.feed(wires[0].data(), wires[0].size(), recovered);
    dec.feed(wires[1].data(), wires[1].size(), recovered);
    dec.feed(fec[0].data(), fec[0].size(), recovered);
    dec.feed(fec[1].data(), fec[1].size(), recovered);
    dec.tick(recovered);

    assert(recovered.empty());
    printf("    PASS\n");
}

// Test 8: FEC packets can arrive before the data packets — order independent.
static void test_fec_first() {
    printf("  FEC arrives before data (K=4, M=2)...\n");
    FecEncoder enc;
    enc.set_group_size(4);
    enc.set_parity_count(2);

    std::vector<std::vector<uint8_t>> wires;
    for (int i = 0; i < 4; ++i)
        wires.push_back(make_data_wire(800 + i, 8000, 300, static_cast<uint8_t>(i + 50)));

    auto fec = encode_group(enc, wires, 800, 8000, false);

    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;

    // FEC first — decoder must buffer it in the ring / group.
    for (auto& f : fec) dec.feed(f.data(), f.size(), recovered);
    // Then 2 data packets (drop 0 and 3).
    dec.feed(wires[1].data(), wires[1].size(), recovered);
    dec.feed(wires[2].data(), wires[2].size(), recovered);
    dec.tick(recovered);

    assert(recovered.size() == 2);
    bool got0 = false, got3 = false;
    for (const auto& r : recovered) {
        if (r == wires[0]) got0 = true;
        else if (r == wires[3]) got3 = true;
    }
    assert(got0 && got3);
    printf("    PASS\n");
}

// Test 9: two groups back-to-back with adaptive M change between them.
static void test_two_groups_adaptive_m() {
    printf("  two back-to-back groups, M changes (1 then 3)...\n");
    FecEncoder enc;
    enc.set_group_size(4);
    enc.set_parity_count(1);

    // Group 1: M=1, K=4, one loss.
    std::vector<std::vector<uint8_t>> g1;
    for (int i = 0; i < 4; ++i)
        g1.push_back(make_data_wire(900 + i, 9000, 200, static_cast<uint8_t>(i)));
    auto fec1 = encode_group(enc, g1, 900, 9000, false);
    assert(fec1.size() == 1);

    // Change M to 3 (simulating adaptive raise).
    enc.set_parity_count(3);

    // Group 2: M=3, K=4, three losses.
    std::vector<std::vector<uint8_t>> g2;
    for (int i = 0; i < 4; ++i)
        g2.push_back(make_data_wire(1000 + i, 10000, 200, static_cast<uint8_t>(i + 99)));
    auto fec2 = encode_group(enc, g2, 1000, 10000, false);
    assert(fec2.size() == 3);

    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;
    // Group 1: drop idx 2.
    dec.feed(g1[0].data(), g1[0].size(), recovered);
    dec.feed(g1[1].data(), g1[1].size(), recovered);
    dec.feed(g1[3].data(), g1[3].size(), recovered);
    dec.feed(fec1[0].data(), fec1[0].size(), recovered);
    // Group 2: drop idx 0, 1, 3.
    dec.feed(g2[2].data(), g2[2].size(), recovered);
    for (auto& f : fec2) dec.feed(f.data(), f.size(), recovered);

    dec.tick(recovered);

    assert(recovered.size() == 4);
    // One recovery from group 1, three from group 2.
    int c1 = 0, c2 = 0;
    for (const auto& r : recovered) {
        if (r == g1[2]) ++c1;
        for (int i : {0, 1, 3}) if (r == g2[i]) ++c2;
    }
    assert(c1 == 1);
    assert(c2 == 3);
    printf("    PASS\n");
}

// Test 10: fuzz — random losses across many groups, always ≤ M losses.
static void test_fuzz_within_budget() {
    printf("  fuzz: random losses within M budget...\n");
    std::mt19937 rng(424242);
    const int iterations = 40;

    for (int iter = 0; iter < iterations; ++iter) {
        std::uniform_int_distribution<int> k_dist(3, 15);
        std::uniform_int_distribution<int> m_dist(1, 5);
        const int k = k_dist(rng);
        const int m = m_dist(rng);
        // At most min(M, K) losses: recovery needs losses <= M, and you
        // cannot drop more data packets than the K that exist.  (When M > K
        // — valid, e.g. K=3 M=5 — clamping to K avoids drawing a `losses`
        // bigger than the K data indices the drop loop below indexes.)
        std::uniform_int_distribution<int> loss_dist(0, std::min(m, k));
        const int losses = loss_dist(rng);

        FecEncoder enc;
        enc.set_group_size(static_cast<uint8_t>(k));
        enc.set_parity_count(static_cast<uint8_t>(m));

        std::vector<std::vector<uint8_t>> wires;
        std::uniform_int_distribution<int> len_dist(64, 1200);
        for (int i = 0; i < k; ++i) {
            size_t len = len_dist(rng);
            wires.push_back(make_data_wire(
                static_cast<uint16_t>(2000 + iter * 100 + i),
                static_cast<uint32_t>(1000 * iter + i),
                len, static_cast<uint8_t>(iter * 7 + i)));
        }

        auto fec = encode_group(enc, wires, static_cast<uint16_t>(2000 + iter * 100),
                                static_cast<uint32_t>(1000 * iter), false);
        assert(static_cast<int>(fec.size()) == m);

        // Pick `losses` distinct data indices to drop.
        std::vector<int> idx(k);
        for (int i = 0; i < k; ++i) idx[i] = i;
        std::shuffle(idx.begin(), idx.end(), rng);
        std::vector<bool> dropped(k, false);
        for (int i = 0; i < losses; ++i) dropped[idx[i]] = true;

        FecDecoder dec;
        std::vector<std::vector<uint8_t>> recovered;
        for (int i = 0; i < k; ++i) {
            if (!dropped[i])
                dec.feed(wires[i].data(), wires[i].size(), recovered);
        }
        for (auto& f : fec) dec.feed(f.data(), f.size(), recovered);
        dec.tick(recovered);

        int present = verify_all_present(wires,
            /*delivered=*/std::vector<std::vector<uint8_t>>{}, recovered);
        // Above call only checks recovered side; also add survivors for full check:
        std::vector<std::vector<uint8_t>> survivors;
        for (int i = 0; i < k; ++i) if (!dropped[i]) survivors.push_back(wires[i]);
        present = verify_all_present(wires, survivors, recovered);
        assert(present == k);
    }
    printf("    PASS (%d iterations)\n", iterations);
}

// Test 11 (VIV-11): a group_id reused with a different M used to index
// parity_shards (sized to the FIRST M) out of bounds — a heap overflow that
// segfaulted the Linux client under netem loss 50%.  The decoder must drop
// the mismatching packet instead of writing past the vector.
static void test_group_id_km_mismatch_no_oob() {
    printf("  group-id reuse, mismatched K/M — no OOB (VIV-11)...\n");
    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;
    const size_t shard = 200;

    // Establish group 7 with K=4, M=2 → parity_shards sized to 2.
    auto good = make_fec_wire(7, /*k=*/4, /*m=*/2, /*idx=*/0, shard);
    dec.feed(good.data(), good.size(), recovered);

    // Same group 7, but M=10 and parity_idx=9.  parity_idx passes the
    // per-packet "< M" check yet indexes the size-2 parity_shards.  Pre-fix:
    // heap-buffer-overflow.  Post-fix: dropped.
    auto bad_m = make_fec_wire(7, /*k=*/4, /*m=*/10, /*idx=*/9, shard);
    dec.feed(bad_m.data(), bad_m.size(), recovered);

    // Same group 7, mismatched K — also dropped (stale pkt_keys/lens).
    auto bad_k = make_fec_wire(7, /*k=*/20, /*m=*/2, /*idx=*/1, shard);
    dec.feed(bad_k.data(), bad_k.size(), recovered);

    dec.tick(recovered);
    assert(recovered.empty());   // nothing spuriously recovered, no crash
    printf("    PASS\n");
}

// Test 12 (VIV-11): blast the decoder with extreme loss + malformed packets
// (random raw bytes, out-of-range/truncated FEC headers, reused group ids
// with random K/M).  Pure no-crash / no-OOB soak — run under ASan/valgrind to
// be meaningful.  Deterministic seed so failures reproduce.
static void test_extreme_loss_garbage_fuzz() {
    printf("  extreme loss + garbage fuzz (no crash)...\n");
    std::mt19937 rng(0xC0FFEEu);
    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;

    for (int iter = 0; iter < 4000; ++iter) {
        switch (rng() % 5) {
        case 0: {  // random raw garbage, possibly shorter than a header
            std::uniform_int_distribution<int> len(0, 64);
            std::vector<uint8_t> g(static_cast<size_t>(len(rng)));
            for (auto& b : g) b = static_cast<uint8_t>(rng());
            dec.feed(g.data(), g.size(), recovered);
            break;
        }
        case 1: {  // crafted FEC with random (often invalid) K/M/idx/group
            std::uniform_int_distribution<int> gd(0, 24), kd(0, 200),
                md(0, 80), id(0, 80), sd(0, 300);
            auto w = make_fec_wire(static_cast<uint16_t>(gd(rng)),
                                   static_cast<uint8_t>(kd(rng)),
                                   static_cast<uint8_t>(md(rng)),
                                   static_cast<uint8_t>(id(rng)),
                                   static_cast<size_t>(sd(rng)));
            // Sometimes truncate to exercise short-buffer guards.
            if ((rng() & 3) == 0 && w.size() > 12)
                w.resize(10 + rng() % (w.size() - 10));
            dec.feed(w.data(), w.size(), recovered);
            break;
        }
        case 2: {  // valid-ish data wire over a small key space
            std::uniform_int_distribution<int> sq(0, 40), ln(20, 400);
            auto w = make_data_wire(static_cast<uint16_t>(sq(rng)), 0,
                                    static_cast<size_t>(ln(rng)),
                                    static_cast<uint8_t>(rng()));
            dec.feed(w.data(), w.size(), recovered);
            break;
        }
        case 3:
            dec.tick(recovered);
            break;
        case 4:
            if (rng() % 50 == 0) dec.reset();
            break;
        }
        recovered.clear();
    }
    printf("    PASS\n");
}

// Ranged FEC (VIV-82): large pooled group, contiguous burst loss, variable
// payload lengths.  Verifies (a) the ranged parity packet stays small (no
// per-K key list) regardless of K, and (b) recovery rebuilds the exact
// originals — including their lengths, which ranged derives from each
// recovered packet's own header rather than a length list.
static void test_ranged_recovery() {
    printf("  ranged FEC, pooled K=40 M=10, contiguous burst...\n");
    FecEncoder enc;
    enc.set_ranged(true);
    enc.set_group_size(40);
    enc.set_parity_count(10);

    std::vector<std::vector<uint8_t>> wires;
    for (int i = 0; i < 40; ++i)
        wires.push_back(make_frag_wire(500, static_cast<uint16_t>(i), 2000,
                                       40 + (i % 7) * 17, static_cast<uint8_t>(i * 3)));

    auto fec = encode_group(enc, wires, 500, 2000, false);
    assert(fec.size() == 10);
    // The whole point: ranged parity carries no per-K key list, so it stays
    // well under an MTU even at large K.
    for (const auto& f : fec) assert(f.size() < 1400);

    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;
    std::vector<std::vector<uint8_t>> delivered;
    // Drop a contiguous burst of 10 (indices 12..21) — the realistic failure.
    for (int i = 0; i < 40; ++i) {
        if (i >= 12 && i < 22) continue;
        dec.feed(wires[i].data(), wires[i].size(), recovered);
        delivered.push_back(wires[i]);
    }
    for (auto& f : fec) dec.feed(f.data(), f.size(), recovered);
    dec.tick(recovered);

    assert(recovered.size() == 10);
    int hits = verify_all_present(wires, delivered, recovered);
    assert(hits == 40);   // all originals present (delivered ∪ recovered), exact bytes
    printf("    PASS\n");
}

// Test 14: K + M ≤ 255 is enforced inside the encoder regardless of the
// order the caller sets K and M in (VIV-96 #3) — and the clamped geometry
// still encodes and recovers.
static void test_km_clamp_255() {
    printf("  K+M clamp to 255 (both set orders + clamped round-trip)...\n");

    // K itself is capped at 200 (pooled ranged groups, VIV-82).
    FecEncoder enc;
    enc.set_group_size(250);
    assert(enc.group_size() == 200);

    // Oversized M set first, then K raised: K wins, M shrinks: 200+200 → M=55.
    FecEncoder enc2;
    enc2.set_group_size(2);
    enc2.set_parity_count(200);
    enc2.set_group_size(200);
    assert(enc2.group_size() == 200);
    assert(enc2.parity_count() == 55);

    // K set first, then an oversized M request is clamped on set: 200+60 → M=55.
    FecEncoder enc3;
    enc3.set_group_size(200);
    enc3.set_parity_count(60);
    assert(enc3.parity_count() == 55);

    // Boundary: exactly K + M = 255 passes untouched.
    FecEncoder encB;
    encB.set_group_size(200);
    encB.set_parity_count(55);
    assert(encB.parity_count() == 55);

    // Round-trip on a clamped geometry.  The decoder rejects K > 200
    // (fec_codec.cpp sanity bounds), so use K=200: requested M=60 clamps
    // to 55, and the group still encodes and recovers 5 erasures.
    FecEncoder enc4;
    enc4.set_group_size(200);
    enc4.set_parity_count(60);
    assert(enc4.parity_count() == 55);

    std::vector<std::vector<uint8_t>> wires;
    for (int i = 0; i < 200; ++i)
        wires.push_back(make_data_wire(static_cast<uint16_t>(700 + i), 3000, 40,
                                       static_cast<uint8_t>(i)));
    auto fec = encode_group(enc4, wires, 700, 3000, false);
    assert(fec.size() == 55);

    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;
    std::vector<std::vector<uint8_t>> delivered;
    for (int i = 0; i < 200; ++i) {
        if (i % 40 == 7) continue;  // drop 5 spread-out packets
        dec.feed(wires[i].data(), wires[i].size(), recovered);
        delivered.push_back(wires[i]);
    }
    for (auto& f : fec) dec.feed(f.data(), f.size(), recovered);
    dec.tick(recovered);

    assert(recovered.size() == 5);
    assert(verify_all_present(wires, delivered, recovered) == 200);
    printf("    PASS\n");
}

int main() {
#ifdef _MSC_VER
    // Route CRT/STL debug assertions (e.g. "vector subscript out of range")
    // to stderr instead of a modal dialog so the test fails non-interactively
    // under CI / automation.
    for (int rep : {_CRT_ASSERT, _CRT_ERROR}) {
        _CrtSetReportMode(rep, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(rep, _CRTDBG_FILE_STDERR);
    }
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);  // flush each line so a crash shows progress
    printf("=== Reed-Solomon FEC Tests ===\n");
    test_single_erasure();
    test_triple_erasure();
    test_variable_length();
    test_partial_flush();
    test_no_loss();
    test_mixed_loss();
    test_too_many_losses();
    test_fec_first();
    test_two_groups_adaptive_m();
    test_fuzz_within_budget();
    test_group_id_km_mismatch_no_oob();
    test_extreme_loss_garbage_fuzz();
    test_ranged_recovery();
    test_km_clamp_255();
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
