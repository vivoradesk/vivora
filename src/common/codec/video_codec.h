#pragma once

#include <cstdint>

namespace vivora {

enum class VideoCodec : uint8_t {
    H264 = 0,
    HEVC = 1,
};

// Decoder-capability bitmask (VIV-112).  The client advertises which codecs
// it can actually decode in its HELLO; the host negotiates the best it can
// encode from that set.  Bit position mirrors the VideoCodec enum value:
// bit N (1<<N) means "codec whose enum value is N is supported".  This keeps
// the mapping mechanical and leaves the high bits free for future codecs
// (AV1, HEVC Main10, …) without a second lookup table.
enum VideoCodecCaps : uint8_t {
    CODEC_CAP_H264 = 1u << static_cast<uint8_t>(VideoCodec::H264),  // 0x01
    CODEC_CAP_HEVC = 1u << static_cast<uint8_t>(VideoCodec::HEVC),  // 0x02
    // 1u<<2 reserved for AV1, 1u<<3 for HEVC Main10, etc.
    CODEC_CAP_ALL_KNOWN = CODEC_CAP_H264 | CODEC_CAP_HEVC,
};

// The capability bit for a given codec.
inline constexpr uint8_t codec_cap_bit(VideoCodec c) {
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(c));
}

// Whether a capability bitmask advertises decode support for `c`.
inline constexpr bool caps_support(uint8_t caps, VideoCodec c) {
    return (caps & codec_cap_bit(c)) != 0;
}

// VIV-112 codec negotiation.  Pick the codec to encode given the codecs every
// connected client can decode (`common_caps` = AND of their caps), what this
// host can encode (`host_enc`), and the host's configured preference/ceiling
// (`pref`).  The configured codec is a preference/ceiling — this never upgrades
// above `pref`; it only downgrades to H.264 when a client can't decode the
// preferred codec.  Falls back to `pref` (best effort — the client's runtime
// fallback backstops it) when nothing is in common.
inline constexpr VideoCodec choose_codec(uint8_t common_caps, uint8_t host_enc,
                                         VideoCodec pref) {
    if (caps_support(common_caps, pref) && (host_enc & codec_cap_bit(pref)))
        return pref;                                   // everyone can decode it
    if (caps_support(common_caps, VideoCodec::H264) && (host_enc & CODEC_CAP_H264))
        return VideoCodec::H264;                        // universal downgrade
    return pref;                                        // no overlap — best effort
}

} // namespace vivora
