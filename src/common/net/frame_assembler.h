#pragma once

#include "common/protocol/packet.h"
#include "common/utils/types.h"
#include <cstdint>
#include <vector>
#include <queue>
#include <unordered_map>

namespace deskbeam::net {

struct AssembledFrame {
    std::vector<uint8_t> data;
    uint16_t seq_no = 0;
    uint32_t timestamp = 0;
    bool keyframe = false;
};

class FrameAssembler {
public:
    // Feed a received Video packet. Returns true if a complete frame became available.
    bool feed(const protocol::Packet& packet);

    // Pop next complete frame. Returns false if none available.
    bool pop_frame(AssembledFrame& frame);

    uint64_t frames_completed() const { return frames_completed_; }
    uint64_t frames_dropped() const { return frames_dropped_; }

private:
    struct PendingFrame {
        uint16_t frag_count = 0;
        uint16_t received = 0;
        uint32_t timestamp = 0;
        bool keyframe = false;
        std::vector<std::vector<uint8_t>> fragments;
        TimePoint first_arrival;
    };

    void expire_stale();

    std::unordered_map<uint16_t, PendingFrame> pending_;
    std::queue<AssembledFrame> completed_;
    uint64_t frames_completed_ = 0;
    uint64_t frames_dropped_ = 0;

    static constexpr int64_t FRAME_TIMEOUT_MS = 100;
};

} // namespace deskbeam::net
