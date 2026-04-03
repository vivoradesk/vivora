#pragma once

#include "common/protocol/packet.h"
#include <cstdint>
#include <vector>

namespace deskbeam::net {

class FrameFragmenter {
public:
    static constexpr size_t MAX_PAYLOAD = 1350;
    static constexpr size_t FRAG_HEADER_SIZE = 4;  // FragIndex(2B) + FragCount(2B)
    static constexpr size_t DATA_PER_FRAGMENT = MAX_PAYLOAD - FRAG_HEADER_SIZE;

    // Fragment encoded frame data into packets ready for sending.
    // All fragments share the same seq_no (frame sequence number).
    std::vector<protocol::Packet> fragment(
        const uint8_t* data, size_t data_len,
        uint16_t frame_seq_no, uint32_t timestamp,
        bool keyframe);
};

} // namespace deskbeam::net
