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

// A batch of fragments to request retransmission of for one frame.
struct NackBatch {
    uint16_t seq_no;
    std::vector<uint16_t> frag_indices;
};

class FrameAssembler {
public:
    // Feed a received Video packet. Returns true if a complete frame became available.
    bool feed(const protocol::Packet& packet);

    // Pop next complete frame. Returns false if none available.
    bool pop_frame(AssembledFrame& frame);

    // Collect fragments that need NACKing.
    // A fragment is eligible when either:
    //   (a) a newer frame has started arriving but this frame is still incomplete, or
    //   (b) first arrival of this frame was more than gap_ms ago and gaps remain.
    // Fragments already NACKed within rate_limit_ms are skipped (avoid duplicates
    // while a retransmit is still in flight).
    std::vector<NackBatch> collect_nacks(int64_t gap_ms, int64_t rate_limit_ms);

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
        // Last NACK request time per fragment index (0 = never requested).
        std::vector<TimePoint> nack_sent_at;
    };

    void expire_stale();

    std::unordered_map<uint16_t, PendingFrame> pending_;
    std::queue<AssembledFrame> completed_;
    uint64_t frames_completed_ = 0;
    uint64_t frames_dropped_ = 0;
    uint16_t newest_seq_ = 0;
    bool has_seq_ = false;

    static constexpr int64_t FRAME_TIMEOUT_MS = 100;
};

} // namespace deskbeam::net
