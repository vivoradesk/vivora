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
        pf.first_arrival = Clock::now();
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
