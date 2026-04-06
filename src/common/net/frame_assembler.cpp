#include "common/net/frame_assembler.h"
#include "common/net/frame_fragmenter.h"
#include <cstring>
#include <chrono>

namespace deskbeam::net {

bool FrameAssembler::feed(const protocol::Packet& packet) {
    if (packet.header.type != protocol::PacketType::Video)
        return false;

    expire_stale();

    uint16_t seq = packet.header.seq_no;
    bool is_fragment = (packet.header.flags & protocol::FLAG_FRAGMENT) != 0;

    if (!is_fragment) {
        // Non-fragmented frame — pass through directly
        AssembledFrame frame;
        frame.data = packet.payload;
        frame.seq_no = seq;
        frame.timestamp = packet.header.timestamp;
        frame.keyframe = (packet.header.flags & protocol::FLAG_KEYFRAME) != 0;
        completed_.push(std::move(frame));
        frames_completed_++;
        return true;
    }

    // Parse fragment header
    if (packet.payload.size() < FrameFragmenter::FRAG_HEADER_SIZE)
        return false;

    uint16_t frag_index = packet.payload[0] | (packet.payload[1] << 8);
    uint16_t frag_count = packet.payload[2] | (packet.payload[3] << 8);

    if (frag_count == 0 || frag_index >= frag_count)
        return false;

    auto& pf = pending_[seq];
    if (pf.frag_count == 0) {
        // New frame
        pf.frag_count = frag_count;
        pf.timestamp = packet.header.timestamp;
        pf.fragments.resize(frag_count);
        pf.nack_sent_at.assign(frag_count, TimePoint{});
        pf.first_arrival = Clock::now();
    }

    // Track newest seen sequence for NACK "newer frame has started" heuristic.
    // 16-bit wraparound aware: `seq - newest_seq_` treated as signed 16-bit.
    if (!has_seq_) {
        newest_seq_ = seq;
        has_seq_ = true;
    } else {
        int16_t delta = static_cast<int16_t>(seq - newest_seq_);
        if (delta > 0) newest_seq_ = seq;
    }

    if (packet.header.flags & protocol::FLAG_KEYFRAME)
        pf.keyframe = true;

    // Store fragment data (skip 4-byte frag header)
    if (pf.fragments[frag_index].empty()) {
        pf.received++;
        pf.fragments[frag_index].assign(
            packet.payload.begin() + FrameFragmenter::FRAG_HEADER_SIZE,
            packet.payload.end());
    }

    // Check if complete
    if (pf.received == pf.frag_count) {
        AssembledFrame frame;
        frame.seq_no = seq;
        frame.timestamp = pf.timestamp;
        frame.keyframe = pf.keyframe;

        // Concatenate all fragments
        size_t total = 0;
        for (auto& f : pf.fragments) total += f.size();
        frame.data.reserve(total);
        for (auto& f : pf.fragments)
            frame.data.insert(frame.data.end(), f.begin(), f.end());

        completed_.push(std::move(frame));
        frames_completed_++;
        pending_.erase(seq);
        return true;
    }

    return false;
}

bool FrameAssembler::pop_frame(AssembledFrame& frame) {
    if (completed_.empty()) return false;
    frame = std::move(completed_.front());
    completed_.pop();
    return true;
}

std::vector<NackBatch> FrameAssembler::collect_nacks(int64_t gap_ms, int64_t rate_limit_ms) {
    std::vector<NackBatch> out;
    if (pending_.empty()) return out;

    auto now = Clock::now();
    for (auto& kv : pending_) {
        uint16_t seq = kv.first;
        auto& pf = kv.second;
        if (pf.received >= pf.frag_count) continue;

        // Eligibility: newer frame has started, OR this frame is older than gap_ms.
        bool newer_frame_started = has_seq_ &&
            static_cast<int16_t>(newest_seq_ - seq) > 0;
        auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - pf.first_arrival).count();
        if (!newer_frame_started && age_ms < gap_ms) continue;

        NackBatch batch;
        batch.seq_no = seq;
        for (uint16_t i = 0; i < pf.frag_count; ++i) {
            if (!pf.fragments[i].empty()) continue; // already have it

            // Rate-limit: skip if we requested recently
            auto last_nack = pf.nack_sent_at[i];
            if (last_nack != TimePoint{}) {
                auto since_nack = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_nack).count();
                if (since_nack < rate_limit_ms) continue;
            }
            batch.frag_indices.push_back(i);
            pf.nack_sent_at[i] = now;

            // Protocol limits: 1 byte count field → max 255 per batch
            if (batch.frag_indices.size() >= 255) break;
        }
        if (!batch.frag_indices.empty()) {
            out.push_back(std::move(batch));
        }
    }
    return out;
}

void FrameAssembler::expire_stale() {
    auto now = Clock::now();
    auto it = pending_.begin();
    while (it != pending_.end()) {
        auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second.first_arrival).count();
        if (age_ms > FRAME_TIMEOUT_MS) {
            frames_dropped_++;
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace deskbeam::net
