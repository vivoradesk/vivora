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
    dec.feed(fec[0].data(), fec[0].size(), recovered);

    // Nothing recovered on feed (attempt_decode=false).  Tick forces decode.
    assert(recovered.empty());
    dec.tick(recovered);

    assert(recovered.size() == 1);
    assert(recovered[0] == wires[2]);
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
        std::uniform_int_distribution<int> loss_dist(0, m);
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

int main() {
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
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
