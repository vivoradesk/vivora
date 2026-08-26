// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// clipboard_test.cpp — unit test for the VIV-22 clipboard wire message:
// ClipboardMessage round-trip (incl. UTF-8 / cyrillic / emoji payloads),
// truncation & oversize rejection, application-layer fragmentation and
// reassembly (in-order, out-of-order, duplicates, repeat delivery dedup,
// interleaved clips, stale-partial eviction).

#include "common/protocol/clipboard_message.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "check.h"

using namespace vivora::protocol;

static ClipboardMessage make_text(const std::string& s) {
    ClipboardMessage m;
    m.mime = ClipboardMessage::text_mime();
    m.data.assign(s.begin(), s.end());
    return m;
}

static void test_message_roundtrip() {
    printf("  message round-trip...\n");
    // Cyrillic ("mir") + rocket emoji + ASCII as explicit UTF-8 bytes —
    // exactly what the GUI ships.  Hex escapes keep the test independent of
    // the compiler's source-charset assumptions (no /utf-8 flag needed).
    const std::string text = "Hello, \xD0\xBC\xD0\xB8\xD1\x80! \xF0\x9F\x9A\x80 clipboard sync";
    ClipboardMessage msg = make_text(text);

    auto wire = msg.serialize();
    CHECK(wire.size() == 1 + msg.mime.size() + 4 + msg.data.size());

    ClipboardMessage out;
    CHECK(ClipboardMessage::deserialize(wire.data(), wire.size(), out));
    CHECK(out.mime == ClipboardMessage::text_mime());
    CHECK(out.data == msg.data);
    CHECK(std::string(out.data.begin(), out.data.end()) == text);
}

static void test_empty_payload_roundtrip() {
    printf("  empty payload round-trip...\n");
    ClipboardMessage msg = make_text("");
    auto wire = msg.serialize();
    ClipboardMessage out;
    CHECK(ClipboardMessage::deserialize(wire.data(), wire.size(), out));
    CHECK(out.data.empty());
    CHECK(out.mime == ClipboardMessage::text_mime());
}

static void test_truncated_rejected() {
    printf("  truncated message rejected...\n");
    ClipboardMessage msg = make_text("truncate me");
    auto wire = msg.serialize();
    ClipboardMessage out;
    CHECK(!ClipboardMessage::deserialize(wire.data(), wire.size() - 1, out));
    CHECK(!ClipboardMessage::deserialize(wire.data(), 0, out));
    CHECK(!ClipboardMessage::deserialize(nullptr, 0, out));
    // Header only (mime_len byte claims more than available).
    uint8_t bogus[2] = { 200, 'x' };
    CHECK(!ClipboardMessage::deserialize(bogus, sizeof(bogus), out));
}

static void test_oversize_rejected() {
    printf("  oversize payload rejected...\n");
    // A message whose declared data_len exceeds MAX_PAYLOAD must not
    // deserialize even if the buffer is large enough.
    ClipboardMessage big = make_text(std::string(ClipboardMessage::MAX_PAYLOAD + 1, 'a'));
    auto wire = big.serialize();
    ClipboardMessage out;
    CHECK(!ClipboardMessage::deserialize(wire.data(), wire.size(), out));
    // And the fragmenter refuses to ship it at all.
    CHECK(fragment_clipboard(big, 1).empty());
}

static void test_single_fragment_roundtrip() {
    printf("  single-fragment reassembly...\n");
    ClipboardMessage msg = make_text("small");
    auto frags = fragment_clipboard(msg, 7);
    CHECK(frags.size() == 1);
    // clip_id LE + idx/count in the fragment header.
    CHECK(frags[0][0] == 7 && frags[0][4] == 0 && frags[0][5] == 1);

    ClipboardReassembler r;
    ClipboardMessage out;
    CHECK(r.feed(frags[0].data(), frags[0].size(), out));
    CHECK(out.data == msg.data);

    // Repeat delivery (loss-resilience re-broadcast) must NOT complete again.
    CHECK(!r.feed(frags[0].data(), frags[0].size(), out));
}

static void test_multi_fragment_out_of_order() {
    printf("  multi-fragment out-of-order + duplicates...\n");
    // ~5KB → 5 fragments at 1200B chunks.
    std::string text;
    for (int i = 0; i < 5000; ++i) text.push_back(static_cast<char>('A' + (i % 26)));
    ClipboardMessage msg = make_text(text);

    auto frags = fragment_clipboard(msg, 42);
    CHECK(frags.size() == (msg.serialize().size() + CLIPBOARD_CHUNK_SIZE - 1)
                           / CLIPBOARD_CHUNK_SIZE);
    CHECK(frags.size() >= 4);

    ClipboardReassembler r;
    ClipboardMessage out;
    // Feed out of order: last, then dup of last, then the rest reversed.
    const size_t n = frags.size();
    CHECK(!r.feed(frags[n - 1].data(), frags[n - 1].size(), out));
    CHECK(!r.feed(frags[n - 1].data(), frags[n - 1].size(), out));  // duplicate
    for (size_t i = n - 1; i-- > 1;) {
        CHECK(!r.feed(frags[i].data(), frags[i].size(), out));
    }
    CHECK(r.feed(frags[0].data(), frags[0].size(), out));   // completes
    CHECK(out.data == msg.data);
    CHECK(out.mime == ClipboardMessage::text_mime());

    // Full repeat of the whole set: dedup'd, never completes again.
    for (const auto& f : frags) CHECK(!r.feed(f.data(), f.size(), out));
}

static void test_newer_clip_evicts_stale_partial() {
    printf("  newer clip evicts stale partial...\n");
    ClipboardMessage m1 = make_text(std::string(3000, 'x'));  // 3 fragments
    ClipboardMessage m2 = make_text("newer");
    auto f1 = fragment_clipboard(m1, 1);
    auto f2 = fragment_clipboard(m2, 2);
    CHECK(f1.size() >= 2 && f2.size() == 1);

    ClipboardReassembler r;
    ClipboardMessage out;
    CHECK(!r.feed(f1[0].data(), f1[0].size(), out));  // clip 1 partial
    CHECK(r.feed(f2[0].data(), f2[0].size(), out));   // clip 2 completes
    CHECK(std::string(out.data.begin(), out.data.end()) == "newer");
    // Clip 1's remaining fragments trickle in late: entry was evicted, and
    // even a "complete" re-feed of clip 1 must be treated as a fresh clip —
    // it completes only if ALL fragments arrive again.
    CHECK(!r.feed(f1[1].data(), f1[1].size(), out));
    if (f1.size() > 2) {
        for (size_t i = 2; i < f1.size(); ++i)
            CHECK(!r.feed(f1[i].data(), f1[i].size(), out));
    }
    CHECK(r.feed(f1[0].data(), f1[0].size(), out));
    CHECK(out.data == m1.data);
}

static void test_malformed_fragments() {
    printf("  malformed fragments rejected...\n");
    ClipboardReassembler r;
    ClipboardMessage out;
    // Too short for the fragment header.
    uint8_t tiny[3] = { 1, 2, 3 };
    CHECK(!r.feed(tiny, sizeof(tiny), out));
    CHECK(!r.feed(nullptr, 0, out));
    // frag_count == 0 and frag_idx >= frag_count.
    uint8_t bad1[6] = { 9, 0, 0, 0, /*idx*/0, /*count*/0 };
    CHECK(!r.feed(bad1, sizeof(bad1), out));
    uint8_t bad2[6] = { 9, 0, 0, 0, /*idx*/2, /*count*/2 };
    CHECK(!r.feed(bad2, sizeof(bad2), out));
}

static void test_max_payload_fragments() {
    printf("  256 KiB payload fragments under the 255 cap...\n");
    ClipboardMessage msg = make_text(std::string(ClipboardMessage::MAX_PAYLOAD, 'z'));
    auto frags = fragment_clipboard(msg, 99);
    CHECK(!frags.empty());
    CHECK(frags.size() <= 255);

    ClipboardReassembler r;
    ClipboardMessage out;
    bool done = false;
    for (const auto& f : frags) {
        done = r.feed(f.data(), f.size(), out);
    }
    CHECK(done);
    CHECK(out.data == msg.data);
}

int main() {
    printf("clipboard_test:\n");
    test_message_roundtrip();
    test_empty_payload_roundtrip();
    test_truncated_rejected();
    test_oversize_rejected();
    test_single_fragment_roundtrip();
    test_multi_fragment_out_of_order();
    test_newer_clip_evicts_stale_partial();
    test_malformed_fragments();
    test_max_payload_fragments();
    return check_report("All clipboard tests passed.");
}
