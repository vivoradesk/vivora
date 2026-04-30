#pragma once

#include "common/protocol/packet.h"
#include <cstdint>
#include <vector>

namespace deskbeam::net {

class FrameFragmenter {
public:
    // Plaintext payload budget per UDP packet.  Transport AEAD adds 24 bytes
    // (8B nonce + 16B Poly1305 tag) of overhead on top, so the wire form is
    // up to 10B header + MAX_PAYLOAD + 24B crypto = 1360B — unchanged from
    // the pre-encryption budget and still safely under a typical MTU.
    static constexpr size_t MAX_PAYLOAD = 1326;
    static constexpr size_t FRAG_HEADER_SIZE = 4;  // FragIndex(2B) + FragCount(2B)
    static constexpr size_t DATA_PER_FRAGMENT = MAX_PAYLOAD - FRAG_HEADER_SIZE;

    // Fragment encoded frame data into packets ready for sending.
    // All fragments share the same seq_no (frame sequence number).
    // heartbeat=true tags every fragment with FLAG_HEARTBEAT so the client
    // can skip adaptive-framerate accounting for these frames.
    std::vector<protocol::Packet> fragment(
        const uint8_t* data, size_t data_len,
        uint16_t frame_seq_no, uint32_t timestamp,
        bool keyframe, bool heartbeat = false);
};

} // namespace deskbeam::net
