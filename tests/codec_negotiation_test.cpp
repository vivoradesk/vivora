// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// codec_negotiation_test.cpp — unit test for the VIV-112 client-capability-
// driven codec negotiation: the capability bitmask helpers and the host's
// choose_codec() preference/ceiling logic (host encodes the best codec the
// client(s) can decode, never above the configured preference).

#include "common/codec/video_codec.h"
#include <cstdio>
#include "check.h"

using namespace vivora;

static void test_cap_helpers() {
    printf("  capability bit helpers...\n");
    // Bit layout mirrors the enum value: H264=bit0, HEVC=bit1.
    CHECK(codec_cap_bit(VideoCodec::H264) == 0x01);
    CHECK(codec_cap_bit(VideoCodec::HEVC) == 0x02);
    CHECK(CODEC_CAP_H264 == 0x01);
    CHECK(CODEC_CAP_HEVC == 0x02);
    CHECK(CODEC_CAP_ALL_KNOWN == 0x03);

    CHECK(caps_support(CODEC_CAP_ALL_KNOWN, VideoCodec::H264));
    CHECK(caps_support(CODEC_CAP_ALL_KNOWN, VideoCodec::HEVC));
    CHECK(caps_support(CODEC_CAP_H264, VideoCodec::H264));
    CHECK(!caps_support(CODEC_CAP_H264, VideoCodec::HEVC));
    CHECK(!caps_support(0, VideoCodec::H264));
}

static void test_negotiation() {
    printf("  choose_codec preference/ceiling...\n");
    const uint8_t host_both = CODEC_CAP_ALL_KNOWN;  // host encodes H.264 + HEVC

    // HEVC-preferring host, client decodes both → HEVC (no change).
    CHECK(choose_codec(CODEC_CAP_ALL_KNOWN, host_both, VideoCodec::HEVC)
           == VideoCodec::HEVC);

    // HEVC-preferring host, client can only decode H.264 → downgrade to H.264.
    CHECK(choose_codec(CODEC_CAP_H264, host_both, VideoCodec::HEVC)
           == VideoCodec::H264);

    // H.264-preferring host (ceiling), client decodes both → stays H.264,
    // NEVER upgrades to HEVC.
    CHECK(choose_codec(CODEC_CAP_ALL_KNOWN, host_both, VideoCodec::H264)
           == VideoCodec::H264);

    // HEVC-preferring host, HEVC-only client → HEVC (both can do it).
    CHECK(choose_codec(CODEC_CAP_HEVC, host_both, VideoCodec::HEVC)
           == VideoCodec::HEVC);

    // No overlap (client decodes nothing we can offer) → best-effort = pref;
    // the client's runtime fallback then takes over.
    CHECK(choose_codec(0, host_both, VideoCodec::HEVC) == VideoCodec::HEVC);

    // Host that can ONLY encode H.264, client prefers/decodes both → H.264.
    CHECK(choose_codec(CODEC_CAP_ALL_KNOWN, CODEC_CAP_H264, VideoCodec::HEVC)
           == VideoCodec::H264);
}

static void test_multi_client_intersection() {
    printf("  multi-client common-denominator...\n");
    // Two clients: one HEVC-capable, one H.264-only.  The shared encoder must
    // pick the AND of their caps.
    const uint8_t common = CODEC_CAP_ALL_KNOWN & CODEC_CAP_H264;  // = H.264 only
    CHECK(choose_codec(common, CODEC_CAP_ALL_KNOWN, VideoCodec::HEVC)
           == VideoCodec::H264);
    // Both HEVC-capable → HEVC survives.
    const uint8_t common2 = CODEC_CAP_ALL_KNOWN & CODEC_CAP_ALL_KNOWN;
    CHECK(choose_codec(common2, CODEC_CAP_ALL_KNOWN, VideoCodec::HEVC)
           == VideoCodec::HEVC);
}

int main() {
    printf("codec_negotiation_test:\n");
    test_cap_helpers();
    test_negotiation();
    test_multi_client_intersection();
    return check_report("All codec_negotiation tests passed.");
}
