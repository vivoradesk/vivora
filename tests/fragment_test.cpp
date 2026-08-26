// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// fragment_test.cpp — unit test for FrameFragmenter + FrameAssembler
// Tests: small frame passthrough, large frame fragment/reassemble,
//        reverse order, gaps, duplicate fragments.

#include "common/net/frame_fragmenter.h"
#include "common/net/frame_assembler.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <numeric>
#include <random>
#include "check.h"

using namespace vivora::net;
using namespace vivora::protocol;

static std::vector<uint8_t> make_test_data(size_t size) {
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<uint8_t>(i & 0xFF);
    return data;
}

// Small frame (<= MAX_PAYLOAD) should not be fragmented
static void test_small_frame() {
    printf("  small frame passthrough...\n");
    FrameFragmenter frag;
    FrameAssembler asm_;

    auto data = make_test_data(500);
    auto packets = frag.fragment(data.data(), data.size(), 1, 1000, true);

    CHECK(packets.size() == 1);
    CHECK(packets[0].header.type == PacketType::Video);
    CHECK(packets[0].header.seq_no == 1);
    CHECK(packets[0].header.timestamp == 1000);
    CHECK((packets[0].header.flags & FLAG_KEYFRAME) != 0);
    CHECK((packets[0].header.flags & FLAG_FRAGMENT) == 0);
    CHECK(packets[0].payload == data);

    // Feed to assembler — should complete immediately
    bool complete = asm_.feed(packets[0]);
    CHECK(complete);

    AssembledFrame frame;
    CHECK(asm_.pop_frame(frame));
    CHECK(frame.data == data);
    CHECK(frame.seq_no == 1);
    CHECK(frame.timestamp == 1000);
    CHECK(frame.keyframe);
    CHECK(asm_.frames_completed() == 1);
    printf("    PASS\n");
}

// Large frame (200KB) should fragment and reassemble in order
static void test_large_frame_in_order() {
    printf("  large frame in-order reassembly...\n");
    FrameFragmenter frag;
    FrameAssembler asm_;

    auto data = make_test_data(200 * 1024); // 200KB
    auto packets = frag.fragment(data.data(), data.size(), 42, 5000, true);

    size_t expected_frags = (data.size() + FrameFragmenter::DATA_PER_FRAGMENT - 1)
                            / FrameFragmenter::DATA_PER_FRAGMENT;
    CHECK(packets.size() == expected_frags);
    printf("    %zu fragments for %zuB\n", packets.size(), data.size());

    // All fragments should have FLAG_FRAGMENT and same seq_no
    for (size_t i = 0; i < packets.size(); ++i) {
        CHECK(packets[i].header.seq_no == 42);
        CHECK((packets[i].header.flags & FLAG_FRAGMENT) != 0);
        if (i == 0)
            CHECK((packets[i].header.flags & FLAG_KEYFRAME) != 0);
        if (i == packets.size() - 1)
            CHECK((packets[i].header.flags & FLAG_LAST_FRAG) != 0);
    }

    // Feed all in order
    bool any_complete = false;
    for (auto& pkt : packets) {
        if (asm_.feed(pkt))
            any_complete = true;
    }
    CHECK(any_complete);

    AssembledFrame frame;
    CHECK(asm_.pop_frame(frame));
    CHECK(frame.data == data);
    CHECK(frame.seq_no == 42);
    CHECK(frame.keyframe);
    CHECK(asm_.frames_completed() == 1);
    printf("    PASS\n");
}

// Fragments arriving in reverse order
static void test_reverse_order() {
    printf("  reverse-order reassembly...\n");
    FrameFragmenter frag;
    FrameAssembler asm_;

    auto data = make_test_data(50 * 1024); // 50KB
    auto packets = frag.fragment(data.data(), data.size(), 10, 2000, false);

    std::reverse(packets.begin(), packets.end());

    bool any_complete = false;
    for (auto& pkt : packets) {
        if (asm_.feed(pkt))
            any_complete = true;
    }
    CHECK(any_complete);

    AssembledFrame frame;
    CHECK(asm_.pop_frame(frame));
    CHECK(frame.data == data);
    CHECK(frame.seq_no == 10);
    CHECK(!frame.keyframe);
    printf("    PASS\n");
}

// Random shuffle order
static void test_random_order() {
    printf("  random-order reassembly...\n");
    FrameFragmenter frag;
    FrameAssembler asm_;

    auto data = make_test_data(100 * 1024);
    auto packets = frag.fragment(data.data(), data.size(), 7, 3000, true);

    std::mt19937 rng(12345);
    std::shuffle(packets.begin(), packets.end(), rng);

    bool any_complete = false;
    for (auto& pkt : packets) {
        if (asm_.feed(pkt))
            any_complete = true;
    }
    CHECK(any_complete);

    AssembledFrame frame;
    CHECK(asm_.pop_frame(frame));
    CHECK(frame.data == data);
    printf("    PASS\n");
}

// Duplicate fragments should not break reassembly
static void test_duplicate_fragments() {
    printf("  duplicate fragment handling...\n");
    FrameFragmenter frag;
    FrameAssembler asm_;

    auto data = make_test_data(10 * 1024);
    auto packets = frag.fragment(data.data(), data.size(), 20, 4000, false);

    // Feed first fragment twice, then the rest
    asm_.feed(packets[0]);
    asm_.feed(packets[0]); // duplicate

    bool any_complete = false;
    for (size_t i = 1; i < packets.size(); ++i) {
        if (asm_.feed(packets[i]))
            any_complete = true;
    }
    CHECK(any_complete);

    AssembledFrame frame;
    CHECK(asm_.pop_frame(frame));
    CHECK(frame.data == data);
    printf("    PASS\n");
}

// Missing fragments — frame should never complete (but shouldn't crash)
static void test_missing_fragments() {
    printf("  missing fragments (incomplete frame)...\n");
    FrameFragmenter frag;
    FrameAssembler asm_;

    auto data = make_test_data(20 * 1024);
    auto packets = frag.fragment(data.data(), data.size(), 30, 5000, false);
    CHECK(packets.size() > 3);

    // Feed only first half
    for (size_t i = 0; i < packets.size() / 2; ++i) {
        bool complete = asm_.feed(packets[i]);
        CHECK(!complete); // should never complete
    }

    AssembledFrame frame;
    CHECK(!asm_.pop_frame(frame)); // nothing available
    printf("    PASS\n");
}

// Multiple frames interleaved
static void test_multiple_frames() {
    printf("  multiple interleaved frames...\n");
    FrameFragmenter frag;
    FrameAssembler asm_;

    auto data1 = make_test_data(30 * 1024);
    auto data2 = make_test_data(25 * 1024);
    // Make data2 different
    for (auto& b : data2) b = ~b;

    auto packets1 = frag.fragment(data1.data(), data1.size(), 100, 10000, true);
    auto packets2 = frag.fragment(data2.data(), data2.size(), 101, 10016, false);

    // Interleave: alternating packets from frame 1 and 2
    std::vector<Packet> interleaved;
    size_t i1 = 0, i2 = 0;
    while (i1 < packets1.size() || i2 < packets2.size()) {
        if (i1 < packets1.size()) interleaved.push_back(packets1[i1++]);
        if (i2 < packets2.size()) interleaved.push_back(packets2[i2++]);
    }

    int completed = 0;
    for (auto& pkt : interleaved) {
        if (asm_.feed(pkt))
            completed++;
    }
    CHECK(completed == 2);

    AssembledFrame f1, f2;
    CHECK(asm_.pop_frame(f1));
    CHECK(asm_.pop_frame(f2));
    // Frames should come out in completion order
    CHECK(f1.data == data1 || f1.data == data2);
    CHECK(f2.data == data1 || f2.data == data2);
    CHECK(f1.data != f2.data);
    CHECK(asm_.frames_completed() == 2);
    printf("    PASS\n");
}

// Exact boundary: frame size == MAX_PAYLOAD (should not fragment)
static void test_boundary_exact() {
    printf("  boundary: frame == MAX_PAYLOAD...\n");
    FrameFragmenter frag;

    auto data = make_test_data(FrameFragmenter::MAX_PAYLOAD);
    auto packets = frag.fragment(data.data(), data.size(), 1, 100, false);
    CHECK(packets.size() == 1);
    CHECK((packets[0].header.flags & FLAG_FRAGMENT) == 0);
    printf("    PASS\n");
}

// Boundary: frame size == MAX_PAYLOAD + 1 (should fragment into 2)
static void test_boundary_plus_one() {
    printf("  boundary: frame == MAX_PAYLOAD+1...\n");
    FrameFragmenter frag;
    FrameAssembler asm_;

    auto data = make_test_data(FrameFragmenter::MAX_PAYLOAD + 1);
    auto packets = frag.fragment(data.data(), data.size(), 2, 200, false);
    CHECK(packets.size() == 2);
    CHECK((packets[0].header.flags & FLAG_FRAGMENT) != 0);

    for (auto& pkt : packets) asm_.feed(pkt);
    AssembledFrame frame;
    CHECK(asm_.pop_frame(frame));
    CHECK(frame.data == data);
    printf("    PASS\n");
}

int main() {
    printf("=== Fragment/Assembler Tests ===\n");
    test_small_frame();
    test_large_frame_in_order();
    test_reverse_order();
    test_random_order();
    test_duplicate_fragments();
    test_missing_fragments();
    test_multiple_frames();
    test_boundary_exact();
    test_boundary_plus_one();
    return check_report("=== ALL TESTS PASSED ===");
}
