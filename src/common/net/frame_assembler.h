#pragma once

#include "common/protocol/packet.h"
#include "common/utils/types.h"
#include <cstdint>
#include <vector>
#include <queue>
#include <unordered_map>

namespace vivora::net {

struct AssembledFrame {
    std::vector<uint8_t> data;
    uint16_t seq_no = 0;
    uint32_t timestamp = 0;
    bool keyframe = false;
    bool heartbeat = false;     // host marked frame as keep-alive (FLAG_HEARTBEAT)
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

    // Drop every buffered frame and reset the delivery cursor. Called
    // after loss when we're about to request an IDR — anything still
    // pending references the missing frame and would feed corrupted
    // data to the decoder.
    void reset();

private:
    struct PendingFrame {
        uint16_t frag_count = 0;
        uint16_t received = 0;
        uint32_t timestamp = 0;
        bool keyframe = false;
        bool heartbeat = false;
        bool complete = false;
        // Zero double-copy: fragments are written directly into `assembled`
        // at slot offsets of DATA_PER_FRAGMENT.  `arrived` replaces the
        // previous per-fragment empty() check.  `last_frag_len` records
        // the tail fragment's real size (only the last fragment may be
        // shorter than DATA_PER_FRAGMENT), so finalize() can resize the
        // buffer to the true frame length without another pass through
        // per-fragment vectors.
        std::vector<uint8_t> assembled;
        std::vector<bool>    arrived;
        size_t               last_frag_len = 0;
        TimePoint first_arrival;
        // Last NACK request time per fragment index (0 = never requested).
        std::vector<TimePoint> nack_sent_at;
    };

    void expire_stale();
    void try_deliver();
    void finalize_frame(uint16_t seq, PendingFrame& pf);

    std::unordered_map<uint16_t, PendingFrame> pending_;
    std::queue<AssembledFrame> completed_;
    uint64_t frames_completed_ = 0;
    uint64_t frames_dropped_ = 0;
    uint16_t newest_seq_ = 0;
    bool has_seq_ = false;
    uint16_t next_deliver_seq_ = 0;
    bool has_deliver_seq_ = false;

    static constexpr int64_t FRAME_TIMEOUT_MS = 100;
    // Max total NACK fragments per collect_nacks() call to avoid flooding.
    static constexpr size_t MAX_NACK_PER_CYCLE = 60;
};

} // namespace vivora::net
