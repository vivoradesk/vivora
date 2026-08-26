// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// fec_test.cpp — unit tests for Reed-Solomon FEC encoder/decoder.
// Covers: single-erasure recovery, multi-erasure recovery up to M,
// variable-length shards, partial-group flush, no-loss fast path,
// mixed data+parity loss, and the "too many losses" give-up path.

#include "common/net/fec_codec.h"
#include "common/protocol/packet.h"
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#include "check.h"

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
    CHECK(fec.size() == 1);

    FecDecoder dec;
    // Deliver data packets 0, 1, 3 (drop 2), then FEC.
    std::vector<std::vector<uint8_t>> recovered;
    dec.feed(wires[0].data(), wires[0].size(), recovered);
    dec.feed(wires[1].data(), wires[1].size(), recovered);
    dec.feed(wires[3].data(), wires[3].size(), recovered);
    CHECK(recovered.empty());
    dec.feed(fec[0].data(), fec[0].size(), recovered);

    // In-line recovery (VIV-82): the decoder recovers the instant it holds
    // K shards — on the parity feed itself, not deferred to the next tick().
    CHECK(recovered.size() == 1);
    CHECK(recovered[0] == wires[2]);

    // A resolved group must not re-emit on tick.
    dec.tick(recovered);
    CHECK(recovered.size() == 1);
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
    CHECK(fec.size() == 3);

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

    CHECK(recovered.size() == 3);
    // Each recovered packet must match exactly one dropped original.
    int matched = 0;
    for (int idx : drop) {
        for (const auto& r : recovered) {
            if (r == wires[idx]) { ++matched; break; }
        }
    }
    CHECK(matched == 3);
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
    CHECK(fec.size() == 2);

    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;
    // Drop 0 (small) and 3 (large).
    dec.feed(wires[1].data(), wires[1].size(), recovered);
    dec.feed(wires[2].data(), wires[2].size(), recovered);
    dec.feed(wires[4].data(), wires[4].size(), recovered);
    dec.feed(fec[0].data(), fec[0].size(), recovered);
    dec.feed(fec[1].data(), fec[1].size(), recovered);

    dec.tick(recovered);

    CHECK(recovered.size() == 2);
    // Recovered bytes must equal originals *truncated back to original length*.
    bool got0 = false, got3 = false;
    for (const auto& r : recovered) {
        if (r == wires[0]) got0 = true;
        else if (r == wires[3]) got3 = true;
    }
    CHECK(got0 && got3);
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
    CHECK(fec.size() == 2);  // M parity packets

    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;
    // Drop packet 0 and 2 (2 losses, M=2 → recoverable).
    dec.feed(wires[1].data(), wires[1].size(), recovered);
    dec.feed(fec[0].data(), fec[0].size(), recovered);
    dec.feed(fec[1].data(), fec[1].size(), recovered);
    dec.tick(recovered);

    CHECK(recovered.size() == 2);
    bool got0 = false, got2 = false;
    for (const auto& r : recovered) {
        if (r == wires[0]) got0 = true;
        else if (r == wires[2]) got2 = true;
    }
    CHECK(got0 && got2);
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

    CHECK(recovered.empty());
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
    CHECK(fec.size() == 2);

    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;
    // Deliver 4 data (drop index 2), drop parity[0], deliver parity[1].
    for (int i = 0; i < 5; ++i) {
        if (i == 2) continue;
        dec.feed(wires[i].data(), wires[i].size(), recovered);
    }
    dec.feed(fec[1].data(), fec[1].size(), recovered);
    dec.tick(recovered);

    CHECK(recovered.size() == 1);
    CHECK(recovered[0] == wires[2]);
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
    CHECK(fec.size() == 2);

    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered;
    // Deliver only 2 data packets + both parities (total=4, need K=5).
    dec.feed(wires[0].data(), wires[0].size(), recovered);
    dec.feed(wires[1].data(), wires[1].size(), recovered);
    dec.feed(fec[0].data(), fec[0].size(), recovered);
    dec.feed(fec[1].data(), fec[1].size(), recovered);
    dec.tick(recovered);

    CHECK(recovered.empty());
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

    CHECK(recovered.size() == 2);
    bool got0 = false, got3 = false;
    for (const auto& r : recovered) {
        if (r == wires[0]) got0 = true;
        else if (r == wires[3]) got3 = true;
    }
    CHECK(got0 && got3);
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
    CHECK(fec1.size() == 1);

    // Change M to 3 (simulating adaptive raise).
    enc.set_parity_count(3);

    // Group 2: M=3, K=4, three losses.
    std::vector<std::vector<uint8_t>> g2;
    for (int i = 0; i < 4; ++i)
        g2.push_back(make_data_wire(1000 + i, 10000, 200, static_cast<uint8_t>(i + 99)));
    auto fec2 = encode_group(enc, g2, 1000, 10000, false);
    CHECK(fec2.size() == 3);

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

    CHECK(recovered.size() == 4);
    // One recovery from group 1, three from group 2.
    int c1 = 0, c2 = 0;
    for (const auto& r : recovered) {
        if (r == g1[2]) ++c1;
        for (int i : {0, 1, 3}) if (r == g2[i]) ++c2;
    }
    CHECK(c1 == 1);
    CHECK(c2 == 3);
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
        CHECK(static_cast<int>(fec.size()) == m);

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
        CHECK(present == k);
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
    CHECK(recovered.empty());   // nothing spuriously recovered, no crash
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
    CHECK(fec.size() == 10);
    // The whole point: ranged parity carries no per-K key list, so it stays
    // well under an MTU even at large K.
    for (const auto& f : fec) CHECK(f.size() < 1400);

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

    CHECK(recovered.size() == 10);
    int hits = verify_all_present(wires, delivered, recovered);
    CHECK(hits == 40);   // all originals present (delivered ∪ recovered), exact bytes
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
    CHECK(enc.group_size() == 200);

    // Oversized M set first, then K raised: K wins, M shrinks: 200+200 → M=55.
    FecEncoder enc2;
    enc2.set_group_size(2);
    enc2.set_parity_count(200);
    enc2.set_group_size(200);
    CHECK(enc2.group_size() == 200);
    CHECK(enc2.parity_count() == 55);

    // K set first, then an oversized M request is clamped on set: 200+60 → M=55.
    FecEncoder enc3;
    enc3.set_group_size(200);
    enc3.set_parity_count(60);
    CHECK(enc3.parity_count() == 55);

    // Boundary: exactly K + M = 255 passes untouched.
    FecEncoder encB;
    encB.set_group_size(200);
    encB.set_parity_count(55);
    CHECK(encB.parity_count() == 55);

    // Round-trip on a clamped geometry.  The decoder rejects K > 200
    // (fec_codec.cpp sanity bounds), so use K=200: requested M=60 clamps
    // to 55, and the group still encodes and recovers 5 erasures.
    FecEncoder enc4;
    enc4.set_group_size(200);
    enc4.set_parity_count(60);
    CHECK(enc4.parity_count() == 55);

    std::vector<std::vector<uint8_t>> wires;
    for (int i = 0; i < 200; ++i)
        wires.push_back(make_data_wire(static_cast<uint16_t>(700 + i), 3000, 40,
                                       static_cast<uint8_t>(i)));
    auto fec = encode_group(enc4, wires, 700, 3000, false);
    CHECK(fec.size() == 55);

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

    CHECK(recovered.size() == 5);
    CHECK(verify_all_present(wires, delivered, recovered) == 200);
    printf("    PASS\n");
}

// ---------------------------------------------------------------------------
// VIV-88: FEC geometry / wire-order vs burst-and-uniform loss.
//
// VIV-88 asks whether the "parity tail" is what makes sustained 10% loss
// freeze the stream, and requires the answer come from NUMBERS rather than a
// rig A/B (the live attempt during VIV-84 was confounded by two Clumsy
// instances).  This harness builds real encoder output for several geometries,
// applies each positional loss pattern to ALL of them (paired: identical
// channel, only the packet->position mapping differs) and counts frames that
// fail to fully reconstruct — one unrecoverable group costs the user a dropped
// frame -> drop-to-keyframe -> visible freeze.
//
// Variants, all at IDENTICAL wire overhead (same N, same parity percentage, so
// the same number of packets on the wire — the FEC carve-out rule means
// overhead is the budget, and comparisons are only fair at equal budget):
//   D6-tail   production today: 6 interleaved groups, all parity in the frame
//             tail (VideoSender::prepare_frame_interleaved).
//   D6-mixed  VIV-88 direction #2: same groups, parity spread among the data
//             columns (Bresenham) so a burst bites data and parity alike.
//   D2-tail / D1-tail  fewer, larger groups — parity pooled over more data.
//
// Scale the sample with VIVORA_FEC_SIM_FRAMES (default keeps ctest snappy).
// ---------------------------------------------------------------------------

namespace viv88 {

struct GroupWires {
    std::vector<std::vector<uint8_t>> data;
    std::vector<std::vector<uint8_t>> parity;
};

// A built frame plus one transmit order over its wires.
struct Frame {
    std::vector<GroupWires>                  groups;
    std::vector<std::vector<uint8_t>>        all_data;   // fragment order
    std::vector<const std::vector<uint8_t>*> order;      // transmit order
    std::vector<uint8_t>                     is_data;    // parallel to order
    std::vector<int>                         owner;      // group index per position
};

// Mirror of VideoSender::prepare_frame_interleaved's group split: MIN_GROUP_K=8
// floor, remainder spread over the first groups (sizes differ by <=1), per-group
// M = ceil(K * pct / 100), ranged parity.
static void build_groups(Frame& f, int N, int interleave, int pct, uint16_t frame_seq) {
    static constexpr int MIN_GROUP_K = 8;
    int G = interleave;
    const int max_groups = (N / MIN_GROUP_K) > 0 ? (N / MIN_GROUP_K) : 1;
    if (G > max_groups) G = max_groups;
    while ((N + G - 1) / G > 128) ++G;

    FecEncoder enc;
    enc.set_ranged(true);
    int idx = 0;
    for (int g = 0; g < G; ++g) {
        const int K = (N - idx) / (G - g);
        int M = (K * pct + 99) / 100;
        if (M < 1) M = 1;
        if (K + M > 255) M = 255 - K;
        enc.set_group_size(static_cast<uint8_t>(K));
        enc.set_parity_count(static_cast<uint8_t>(M));

        GroupWires gw;
        for (int j = 0; j < K; ++j) {
            const int fi = idx + j;
            auto wire = make_frag_wire(frame_seq, static_cast<uint16_t>(fi), 1000,
                                       1100, static_cast<uint8_t>(fi * 7));
            auto parts = enc.feed(wire.data(), wire.size(), frame_seq, 1000);
            gw.data.push_back(std::move(wire));
            for (auto& p : parts) gw.parity.push_back(std::move(p));
        }
        if (gw.parity.empty()) {
            auto fw = enc.flush(frame_seq, 1000);
            for (auto& w : fw) gw.parity.push_back(std::move(w));
        }
        f.groups.push_back(std::move(gw));
        idx += K;
    }
    for (auto& g : f.groups)
        for (auto& d : g.data) f.all_data.push_back(d);
}

// Production transpose: round-robin the data columns across groups, then the
// parity columns — so every group's parity lands in the frame's tail.
static void order_parity_tail(Frame& f) {
    size_t max_d = 0, max_p = 0;
    for (auto& g : f.groups) max_d = std::max(max_d, g.data.size());
    for (auto& g : f.groups) max_p = std::max(max_p, g.parity.size());
    for (size_t j = 0; j < max_d; ++j)
        for (auto& g : f.groups)
            if (j < g.data.size()) { f.order.push_back(&g.data[j]); f.is_data.push_back(1);
                                     f.owner.push_back(static_cast<int>(&g - f.groups.data())); }
    for (size_t j = 0; j < max_p; ++j)
        for (auto& g : f.groups)
            if (j < g.parity.size()) { f.order.push_back(&g.parity[j]); f.is_data.push_back(0);
                                       f.owner.push_back(static_cast<int>(&g - f.groups.data())); }
}

// Same columns, merged so parity is spread uniformly through the frame
// (Bresenham: emit parity whenever it falls behind the P/(D+P) rate).  All
// parity exists once the frame is built, so this costs no extra latency — only
// the wire position changes.
static void order_parity_mixed(Frame& f) {
    std::vector<const std::vector<uint8_t>*> d, p;
    std::vector<int> downer, powner;
    size_t max_d = 0, max_p = 0;
    for (auto& g : f.groups) max_d = std::max(max_d, g.data.size());
    for (auto& g : f.groups) max_p = std::max(max_p, g.parity.size());
    for (size_t j = 0; j < max_d; ++j)
        for (auto& g : f.groups) if (j < g.data.size()) {
            d.push_back(&g.data[j]); downer.push_back(static_cast<int>(&g - f.groups.data())); }
    for (size_t j = 0; j < max_p; ++j)
        for (auto& g : f.groups) if (j < g.parity.size()) {
            p.push_back(&g.parity[j]); powner.push_back(static_cast<int>(&g - f.groups.data())); }

    const size_t D = d.size(), P = p.size(), T = D + P;
    size_t di = 0, pi = 0;
    for (size_t i = 0; i < T; ++i) {
        const bool take_parity = (pi < P) && (pi * T < i * P);
        if (take_parity || di >= D) { f.order.push_back(p[pi]); f.is_data.push_back(0);
                                      f.owner.push_back(powner[pi]); ++pi; }
        else                        { f.order.push_back(d[di]); f.is_data.push_back(1);
                                      f.owner.push_back(downer[di]); ++di; }
    }
}

// Feed the survivors of one transmit order into a fresh decoder; true when
// every original data packet came back (delivered u recovered).
static bool frame_survives(const Frame& f, const std::vector<uint8_t>& drop) {
    FecDecoder dec;
    std::vector<std::vector<uint8_t>> recovered, delivered;
    for (size_t i = 0; i < f.order.size(); ++i) {
        if (i < drop.size() && drop[i]) continue;
        dec.feed(f.order[i]->data(), f.order[i]->size(), recovered);
        if (f.is_data[i]) delivered.push_back(*f.order[i]);
    }
    dec.tick(recovered);
    return verify_all_present(f.all_data, delivered, recovered)
           == static_cast<int>(f.all_data.size());
}

struct Variant {
    const char* name;
    int         interleave;
    bool        mixed;
};

// Build every variant for one (N, pct).  Returns false when they do not come
// out at the same wire cost, in which case the caller must not compare them:
// every variant is dropped through ONE mask sized to fv[0], so a variant with
// more packets would carry an undroppable tail and win on overhead rather than
// on geometry.
//
// Equal cost is not a given.  Parity per group is ceil(K*pct/100), so a frame
// split into G groups rounds up G times against once at D=1: at N=48, pct=25
// both come to 12 (6*2 == 12), but at pct=46 D=6 pays 6*ceil(3.68) = 24 while
// D=1 pays ceil(22.08) = 23.  This used to be an assert, which meant Release
// compiled the check away and ran the comparison anyway while Debug aborted
// the whole suite -- the CI job that runs both is what surfaced it.
static bool build_variants(std::vector<Frame>& out, const Variant* vs, int nv,
                           int N, int pct) {
    out.clear();
    out.resize(static_cast<size_t>(nv));
    bool comparable = true;
    for (int v = 0; v < nv; ++v) {
        build_groups(out[v], N, vs[v].interleave, pct, /*frame_seq=*/700);
        if (vs[v].mixed) order_parity_mixed(out[v]);
        else             order_parity_tail(out[v]);
        CHECK(out[v].all_data.size() == static_cast<size_t>(N));
        if (out[v].order.size() != out[0].order.size()) comparable = false;
    }
    return comparable;
}

} // namespace viv88

static void test_fec_geometry_under_loss() {
    printf("  VIV-88 geometry sim: interleave depth + parity placement...\n");
    int frames = 150;
    if (const char* e = std::getenv("VIVORA_FEC_SIM_FRAMES")) {
        const int v = std::atoi(e);
        if (v > 0) frames = v;
    }

    static const viv88::Variant VARIANTS[] = {
        { "D6-tail ", 6, false },   // production
        { "D6-mixed", 6, true  },   // VIV-88 direction #2
        { "D2-tail ", 2, false },
        { "D1-tail ", 1, false },
    };
    const int NV = static_cast<int>(sizeof(VARIANTS) / sizeof(VARIANTS[0]));

    // 48 packets/frame, the shape the rig produced at D=6 (MIN_GROUP_K floor).
    // pct 25 = the steady ladder rung; 48 stands in for where WiFi settles in
    // practice (~46) because it is the nearby value whose per-group ceilings
    // agree across all four variants -- 6*ceil(8*0.48) == ceil(48*0.48) == 24 --
    // so the comparison is like-for-like.  See build_variants().
    const int PCTS[]   = { 25, 48 };
    const int BURSTS[] = { 0, 4, 12 };
    const int UNIFS[]  = { 0, 500, 1000 };   // basis points: 0 / 5 / 10 %

    int totals[8] = {0};
    CHECK(NV <= 8);

    for (int pct : PCTS) {
        std::vector<viv88::Frame> fv;
        if (!viv88::build_variants(fv, VARIANTS, NV, /*N=*/48, pct)) {
            printf("    pct=%d  SKIPPED: variants differ in wire cost, "
                   "so a shared drop mask would not compare them fairly\n", pct);
            for (int v = 0; v < NV; ++v)
                printf("      %s %zu pkts\n", VARIANTS[v].name, fv[v].order.size());
            continue;
        }
        printf("    pct=%d  wire=%zu pkts (48 data + %zu parity)\n",
               pct, fv[0].order.size(), fv[0].order.size() - 48);
        printf("      burst unif%%");
        for (int v = 0; v < NV; ++v) printf("  %s", VARIANTS[v].name);
        printf("\n");

        uint32_t seed = 0x5188C0DEu;
        for (int burst : BURSTS) {
            for (int unif : UNIFS) {
                if (burst == 0 && unif == 0) continue;   // lossless: all perfect
                ++seed;
                int lost[8] = {0};
                std::mt19937 rng(seed);
                const size_t T = fv[0].order.size();
                std::vector<uint8_t> drop(T);
                for (int n = 0; n < frames; ++n) {
                    std::fill(drop.begin(), drop.end(), 0);
                    // Uniform background loss (radio noise / Clumsy's uniform
                    // drop).  Raw modulo rather than uniform_int_distribution:
                    // the latter is not portable across stdlibs and this
                    // harness must yield identical numbers everywhere.
                    if (unif > 0)
                        for (size_t i = 0; i < T; ++i)
                            if (static_cast<int>(rng() % 10000) < unif) drop[i] = 1;
                    // One contiguous burst at a random offset (WiFi retry
                    // exhaustion) — the SAME pattern for every variant.
                    if (burst > 0) {
                        const size_t start = static_cast<size_t>(rng() % T);
                        for (int b = 0; b < burst; ++b) {
                            const size_t pos = start + static_cast<size_t>(b);
                            if (pos < T) drop[pos] = 1;
                        }
                    }
                    for (int v = 0; v < NV; ++v)
                        if (!viv88::frame_survives(fv[v], drop)) ++lost[v];
                }
                printf("      %5d %5.1f", burst, unif / 100.0);
                for (int v = 0; v < NV; ++v) { printf("  %8d", lost[v]); totals[v] += lost[v]; }
                printf("\n");
            }
        }
    }

    printf("    TOTAL frames lost (of %d per cell):", frames);
    for (int v = 0; v < NV; ++v) printf("  %s=%d", VARIANTS[v].name, totals[v]);
    printf("\n");

    // The findings this locks in (see VIV-88):
    //  1. Spreading parity among the data columns is NOT an improvement — it
    //     co-locates a group's parity with its own data, so one burst can wipe
    //     both.  The parity tail maximally separates them.  Direction #2 of the
    //     ticket is refuted; production geometry stays.
    //  2. At equal overhead, pooling the frame into FEWER, LARGER groups
    //     dominates: parity can cover losses wherever they fall instead of
    //     needing <=M in every small group.
    CHECK(totals[1] >= totals[0] && "parity-mixed must not beat parity-tail");
    CHECK(totals[3] <= totals[0] && "pooled (D1) must not be worse than D6");
    printf("    PASS\n");
}


// VIV-88: pooling wins on recovery, but this project trades nothing for
// latency — so measure what the bigger RS geometry actually costs.  Encode is
// O(K*M) GF ops per shard byte, and pooling multiplies K and M instead of
// summing small groups, so the cost must be measured, not assumed.  Times the
// RS work ONLY (the data wires are built once up front) so the number is the
// codec's, not the harness's.
static void bench_fec_geometry_cost() {
    printf("  VIV-88 geometry cost: RS encode/recover per frame...\n");
    const int PCT = 46;
    for (int N : { 48, 200 }) {
        std::vector<std::vector<uint8_t>> wires;
        wires.reserve(static_cast<size_t>(N));
        for (int i = 0; i < N; ++i)
            wires.push_back(make_frag_wire(700, static_cast<uint16_t>(i), 1000,
                                           1100, static_cast<uint8_t>(i * 7)));
        printf("    N=%d pkts/frame (%d KB)\n", N, N * 1100 / 1024);
        for (int D : { 6, 2, 1 }) {
            static constexpr int MIN_GROUP_K = 8;
            int G = D;
            const int max_groups = (N / MIN_GROUP_K) > 0 ? (N / MIN_GROUP_K) : 1;
            if (G > max_groups) G = max_groups;
            while ((N + G - 1) / G > 128) ++G;

            const int REPS = 100;
            size_t parity_pkts = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int r = 0; r < REPS; ++r) {
                FecEncoder enc;
                enc.set_ranged(true);
                int idx = 0;
                parity_pkts = 0;
                for (int g = 0; g < G; ++g) {
                    const int K = (N - idx) / (G - g);
                    int M = (K * PCT + 99) / 100;
                    if (M < 1) M = 1;
                    if (K + M > 255) M = 255 - K;
                    enc.set_group_size(static_cast<uint8_t>(K));
                    enc.set_parity_count(static_cast<uint8_t>(M));
                    for (int j = 0; j < K; ++j) {
                        auto parts = enc.feed(wires[static_cast<size_t>(idx + j)].data(),
                                              wires[static_cast<size_t>(idx + j)].size(),
                                              700, 1000);
                        parity_pkts += parts.size();
                    }
                    idx += K;
                }
            }
            const auto t1 = std::chrono::steady_clock::now();
            const double enc_us =
                std::chrono::duration<double, std::micro>(t1 - t0).count() / REPS;
            printf("      D=%d  G=%d K~%d  parity=%zu   RS encode %8.1f us/frame"
                   "   (%.1f%% of a core at 60fps)\n",
                   D, G, (N + G - 1) / G, parity_pkts, enc_us, enc_us * 60.0 / 10000.0);
        }
    }
    printf("    PASS\n");
}


// VIV-88 direction #1 sizing: how far short do the FAILING groups actually
// fall?  A Reed-Solomon group of K data + M parity resolves as soon as ANY K
// of its K+M shards arrive, so a group that ends up "short by n" needs exactly
// n more shards — any n of them.  If n is 1 or 2, a single targeted
// retransmission turns a dropped frame (drop-to-keyframe -> visible freeze)
// back into a clean recovery.  This measures the ceiling of that idea on the
// production geometry before any NACK code is written.
static void test_nack_rescue_headroom() {
    printf("  VIV-88 rescue headroom: how short do failing groups fall?...\n");
    int frames = 400;
    if (const char* e = std::getenv("VIVORA_FEC_SIM_FRAMES")) {
        const int v = std::atoi(e);
        if (v > 0) frames = v;
    }

    const int PCT = 46;      // where adaptive FEC settles on the WiFi rig
    const int N   = 48;
    const struct { int burst; int unif; } CELLS[] = {
        { 0, 1000 }, { 4, 500 }, { 4, 1000 }, { 12, 500 }, { 12, 1000 },
    };

    long failed_groups = 0, need1 = 0, need2 = 0, need3plus = 0;
    long lost_frames = 0, savable1 = 0, savable2 = 0, total_frames = 0;

    viv88::Frame f;
    viv88::build_groups(f, N, /*interleave=*/6, PCT, /*frame_seq=*/700);
    viv88::order_parity_tail(f);
    const size_t T = f.order.size();
    const size_t G = f.groups.size();

    printf("    geometry: %zu groups, wire=%zu (%d data + %zu parity), pct=%d\n",
           G, T, N, T - static_cast<size_t>(N), PCT);

    uint32_t seed = 0x5188C0DEu;
    for (const auto& cell : CELLS) {
        std::mt19937 rng(++seed);
        std::vector<uint8_t> drop(T);
        for (int n = 0; n < frames; ++n) {
            std::fill(drop.begin(), drop.end(), 0);
            if (cell.unif > 0)
                for (size_t i = 0; i < T; ++i)
                    if (static_cast<int>(rng() % 10000) < cell.unif) drop[i] = 1;
            if (cell.burst > 0) {
                const size_t start = static_cast<size_t>(rng() % T);
                for (int b = 0; b < cell.burst; ++b) {
                    const size_t pos = start + static_cast<size_t>(b);
                    if (pos < T) drop[pos] = 1;
                }
            }
            ++total_frames;

            // Per-group shortfall: a group holds K data + M parity; it resolves
            // with any K shards, so short = K - (surviving data + parity).
            std::vector<int> present(G, 0);
            for (size_t i = 0; i < T; ++i)
                if (!drop[i]) ++present[static_cast<size_t>(f.owner[i])];

            bool frame_lost = false;
            int worst = 0;
            for (size_t g = 0; g < G; ++g) {
                const int K = static_cast<int>(f.groups[g].data.size());
                const int shortfall = K - present[g];
                if (shortfall <= 0) continue;
                frame_lost = true;
                ++failed_groups;
                if (shortfall == 1) ++need1;
                else if (shortfall == 2) ++need2;
                else ++need3plus;
                if (shortfall > worst) worst = shortfall;
            }
            if (frame_lost) {
                ++lost_frames;
                if (worst <= 1) ++savable1;   // one retransmit per failed group
                if (worst <= 2) ++savable2;   // two per failed group
            }
        }
    }

    printf("    failed groups: %ld  (need 1: %ld = %.0f%%, need 2: %ld = %.0f%%, need 3+: %ld)\n",
           failed_groups, need1, failed_groups ? 100.0 * need1 / failed_groups : 0.0,
           need2, failed_groups ? 100.0 * need2 / failed_groups : 0.0, need3plus);
    printf("    lost frames: %ld of %ld   rescued by +1 shard/group: %ld (%.0f%%)"
           "   by +2: %ld (%.0f%%)\n",
           lost_frames, total_frames, savable1,
           lost_frames ? 100.0 * savable1 / lost_frames : 0.0,
           savable2, lost_frames ? 100.0 * savable2 / lost_frames : 0.0);
    // The premise of direction #1: a large share of failures are within one or
    // two shards of recovery, so a targeted retransmit is worth its complexity.
    CHECK(need1 + need2 > 0);
    printf("    PASS\n");
}

// VIV-88: the targeted FEC-rescue NACK and the group-hold window it needs.
//
// The value is that it asks for exactly the shards that turn a doomed group
// into a recovered one; the risk is asking too early or too often (the VIV-82
// attempt, 604971a, was reverted for exactly that).  And it only means anything
// if the group survives long enough to be rescued — tick() runs every poll and
// would otherwise declare a one-shard-short group failed within milliseconds.
// So: near-complete only, after the grace window, once per rate-limit period,
// inside the caller's budget, the named shard really closes the group, and the
// hold window is what keeps the group alive to be closed.
static void test_fec_rescue_nack() {
    printf("  VIV-88 targeted rescue NACK + group-hold window...\n");

    // K=8 data + M=2 parity, ranged — the production per-group shape at D=6.
    const int K = 8, M = 2;
    auto build = [&](std::vector<std::vector<uint8_t>>& wires) {
        FecEncoder enc;
        enc.set_ranged(true);
        enc.set_group_size(static_cast<uint8_t>(K));
        enc.set_parity_count(static_cast<uint8_t>(M));
        wires.clear();
        for (int i = 0; i < K; ++i)
            wires.push_back(make_frag_wire(900, static_cast<uint16_t>(i), 3000,
                                           300, static_cast<uint8_t>(i * 5)));
        return encode_group(enc, wires, 900, 3000, false);
    };
    // Deliver everything except data 2 and 5, plus one parity: 6 + 1 = 7 of
    // K=8, i.e. short by exactly one — the case that dominates real failures.
    auto feed_short_by_one = [&](FecDecoder& dec,
                                 const std::vector<std::vector<uint8_t>>& wires,
                                 const std::vector<std::vector<uint8_t>>& parity,
                                 std::vector<std::vector<uint8_t>>& recovered) {
        for (int i = 0; i < K; ++i)
            if (i != 2 && i != 5) dec.feed(wires[i].data(), wires[i].size(), recovered);
        dec.feed(parity[0].data(), parity[0].size(), recovered);
    };

    // ---- without the hold window, tick() buries the group (old behaviour) --
    {
        std::vector<std::vector<uint8_t>> wires;
        auto parity = build(wires);
        FecDecoder dec;                      // rescue window defaults to 0
        std::vector<std::vector<uint8_t>> recovered;
        feed_short_by_one(dec, wires, parity, recovered);
        dec.tick(recovered);
        CHECK(dec.total_failed() == 1);     // declared lost immediately
        std::vector<uint32_t> keys;
        dec.collect_rescue_keys(keys, 0, 0, 8);
        CHECK(keys.empty());                // nothing left to rescue
    }

    // ---- with the window, the group survives, is named, and is closed ------
    {
        std::vector<std::vector<uint8_t>> wires;
        auto parity = build(wires);
        FecDecoder dec;
        dec.set_rescue_window_ms(60000);     // never expires during the test
        std::vector<std::vector<uint8_t>> recovered;
        feed_short_by_one(dec, wires, parity, recovered);
        dec.tick(recovered);
        CHECK(dec.total_failed() == 0);     // held open, not buried

        std::vector<uint32_t> keys;
        dec.collect_rescue_keys(keys, /*grace_ms=*/0, /*rl_ms=*/0, /*max_keys=*/8);
        CHECK(keys.size() == 1);            // short by one -> ask for one
        const uint32_t want = keys[0];
        // It must name one of the two lost DATA packets — never a parity or an
        // already-arrived packet, since only data lives in the host's retx ring.
        const uint32_t key2 = wire_pkt_key(wires[2].data(), wires[2].size());
        const uint32_t key5 = wire_pkt_key(wires[5].data(), wires[5].size());
        CHECK(want == key2 || want == key5);

        // The promise: hand back that one shard and the group closes, with the
        // other lost packet reconstructed by RS.
        const std::vector<uint8_t>& answer = (want == key2) ? wires[2] : wires[5];
        dec.feed(answer.data(), answer.size(), recovered);
        dec.tick(recovered);
        std::vector<std::vector<uint8_t>> delivered;
        for (int i = 0; i < K; ++i)
            if (i != 2 && i != 5) delivered.push_back(wires[i]);
        delivered.push_back(answer);
        CHECK(verify_all_present(wires, delivered, recovered) == K);
        CHECK(dec.total_failed() == 0);     // rescued, never counted as a loss
    }

    // ---- grace window: nothing is chased while packets may be in flight ----
    {
        std::vector<std::vector<uint8_t>> wires;
        auto parity = build(wires);
        FecDecoder dec;
        dec.set_rescue_window_ms(60000);
        std::vector<std::vector<uint8_t>> recovered;
        feed_short_by_one(dec, wires, parity, recovered);

        std::vector<uint32_t> keys;
        dec.collect_rescue_keys(keys, /*grace_ms=*/60000, /*rl_ms=*/0, 8);
        CHECK(keys.empty());   // group is younger than the grace window
        dec.collect_rescue_keys(keys, /*grace_ms=*/0, /*rl_ms=*/0, 8);
        CHECK(keys.size() == 1);  // same group, grace satisfied
    }

    // ---- rate limit: one request per group per period ----------------------
    {
        std::vector<std::vector<uint8_t>> wires;
        auto parity = build(wires);
        FecDecoder dec;
        dec.set_rescue_window_ms(60000);
        std::vector<std::vector<uint8_t>> recovered;
        feed_short_by_one(dec, wires, parity, recovered);

        std::vector<uint32_t> first, second;
        dec.collect_rescue_keys(first, 0, /*rl_ms=*/60000, 8);
        CHECK(first.size() == 1);
        dec.collect_rescue_keys(second, 0, /*rl_ms=*/60000, 8);
        CHECK(second.empty());   // still inside the per-group rate limit
    }

    // ---- hopeless groups are neither held nor chased -----------------------
    {
        std::vector<std::vector<uint8_t>> wires;
        auto parity = build(wires);
        FecDecoder dec;
        dec.set_rescue_window_ms(60000);
        std::vector<std::vector<uint8_t>> recovered;
        // 3 data + 1 parity = 4 of 8: short by 4, far past MAX_RESCUE_SHORTFALL.
        for (int i = 0; i < 3; ++i)
            dec.feed(wires[i].data(), wires[i].size(), recovered);
        dec.feed(parity[0].data(), parity[0].size(), recovered);
        dec.tick(recovered);
        CHECK(dec.total_failed() == 1);   // fails at once; no pointless delay
        std::vector<uint32_t> keys;
        dec.collect_rescue_keys(keys, 0, 0, 8);
        CHECK(keys.empty());
    }

    // ---- budget cap is honoured across many groups ------------------------
    {
        FecDecoder dec;
        dec.set_rescue_window_ms(60000);
        std::vector<std::vector<uint8_t>> recovered;
        // Distinct frame_seq AND distinct group ids: one encoder, so the group
        // counter advances per group the way the real sender's does.
        FecEncoder enc;
        enc.set_ranged(true);
        for (int g = 0; g < 6; ++g) {
            enc.set_group_size(static_cast<uint8_t>(K));
            enc.set_parity_count(static_cast<uint8_t>(M));
            std::vector<std::vector<uint8_t>> wires;
            for (int i = 0; i < K; ++i)
                wires.push_back(make_frag_wire(static_cast<uint16_t>(1000 + g),
                                               static_cast<uint16_t>(i), 3000, 300,
                                               static_cast<uint8_t>(i * 5)));
            auto parity = encode_group(enc, wires, static_cast<uint16_t>(1000 + g),
                                       3000, false);
            CHECK(!parity.empty());
            for (int i = 0; i < K; ++i)
                if (i != 1 && i != 6) dec.feed(wires[i].data(), wires[i].size(), recovered);
            dec.feed(parity[0].data(), parity[0].size(), recovered);
        }
        std::vector<uint32_t> keys;
        dec.collect_rescue_keys(keys, 0, 0, /*max_keys=*/3);
        CHECK(keys.size() <= 3);   // never exceeds the caller's budget
        CHECK(!keys.empty());      // but does find work to do
    }

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
    test_fec_geometry_under_loss();
    bench_fec_geometry_cost();
    test_nack_rescue_headroom();
    test_fec_rescue_nack();
    return check_report("=== ALL TESTS PASSED ===");
}
