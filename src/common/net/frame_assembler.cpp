#include "common/net/frame_assembler.h"
#include "common/net/frame_fragmenter.h"
#include <cstring>
#include <chrono>

namespace vivora::net {

void FrameAssembler::finalize_frame(uint16_t /*seq*/, PendingFrame& pf) {
    // All fragments except the last carry exactly DATA_PER_FRAGMENT bytes;
    // the last fragment's real length was recorded on arrival.  The buffer
    // was overallocated to frag_count * DATA_PER_FRAGMENT — just resize
    // down to the true total.  shrink_to_fit() is intentionally skipped:
    // it would reallocate and copy, defeating the whole zero-copy point.
    if (pf.frag_count > 0) {
        size_t total = static_cast<size_t>(pf.frag_count - 1)
                       * FrameFragmenter::DATA_PER_FRAGMENT
                       + pf.last_frag_len;
        pf.assembled.resize(total);
    }
    pf.arrived.clear();
    pf.arrived.shrink_to_fit();
    pf.complete = true;
}

bool FrameAssembler::feed(const protocol::Packet& packet) {
    if (packet.header.type != protocol::PacketType::Video)
        return false;

    expire_stale();

    uint16_t seq = packet.header.seq_no;
    bool is_fragment = (packet.header.flags & protocol::FLAG_FRAGMENT) != 0;

    // Drop late-arriving fragments for seqs we've already delivered past.
    if (has_deliver_seq_) {
        int16_t delta = static_cast<int16_t>(seq - next_deliver_seq_);
        if (delta < 0) return false;
    }

    // Track newest seen sequence (wraparound-aware).
    if (!has_seq_) {
        newest_seq_ = seq;
        has_seq_ = true;
    } else {
        int16_t delta = static_cast<int16_t>(seq - newest_seq_);
        if (delta > 0) newest_seq_ = seq;
    }

    // Initialize delivery cursor on the very first frame we ever see.
    if (!has_deliver_seq_) {
        next_deliver_seq_ = seq;
        has_deliver_seq_ = true;
    }

    if (!is_fragment) {
        // Non-fragmented frame — create a synthetic complete PendingFrame
        // so it goes through in-order delivery.
        auto& pf = pending_[seq];
        if (pf.frag_count == 0) {
            pf.frag_count = 1;
            pf.timestamp = packet.header.timestamp;
            pf.first_arrival = Clock::now();
        }
        pf.keyframe = (packet.header.flags & protocol::FLAG_KEYFRAME) != 0;
        pf.heartbeat = (packet.header.flags & protocol::FLAG_HEARTBEAT) != 0;
        pf.assembled = packet.payload;
        pf.complete = true;
        try_deliver();
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
        // New frame — reserve the maximum possible buffer up front so
        // subsequent fragments can be written directly into their final
        // resting place.  Over-allocation is one `DATA_PER_FRAGMENT - tail`
        // bytes at most (~1.3KB), trimmed at finalize.
        pf.frag_count = frag_count;
        pf.timestamp = packet.header.timestamp;
        pf.assembled.assign(
            static_cast<size_t>(frag_count) * FrameFragmenter::DATA_PER_FRAGMENT,
            0);
        pf.arrived.assign(frag_count, false);
        pf.nack_sent_at.assign(frag_count, TimePoint{});
        pf.first_arrival = Clock::now();
    }

    if (packet.header.flags & protocol::FLAG_KEYFRAME)
        pf.keyframe = true;
    if (packet.header.flags & protocol::FLAG_HEARTBEAT)
        pf.heartbeat = true;

    // Store fragment data (skip 4-byte frag header).  Write once, directly
    // at the fragment's slot in the pre-allocated buffer — no intermediate
    // per-fragment vector.
    if (!pf.arrived[frag_index]) {
        pf.arrived[frag_index] = true;
        pf.received++;

        const uint8_t* src = packet.payload.data() + FrameFragmenter::FRAG_HEADER_SIZE;
        size_t frag_len = packet.payload.size() - FrameFragmenter::FRAG_HEADER_SIZE;
        size_t offset = static_cast<size_t>(frag_index)
                        * FrameFragmenter::DATA_PER_FRAGMENT;
        // Defensive clamp: a malformed sender couldn't overrun the buffer
        // past what we reserved for this slot.
        if (frag_len > FrameFragmenter::DATA_PER_FRAGMENT)
            frag_len = FrameFragmenter::DATA_PER_FRAGMENT;
        std::memcpy(pf.assembled.data() + offset, src, frag_len);

        // Only the last fragment's length matters for finalize() — earlier
        // fragments are always DATA_PER_FRAGMENT bytes.
        if (frag_index == pf.frag_count - 1) {
            pf.last_frag_len = frag_len;
        }
    }

    // Check if complete
    if (pf.received == pf.frag_count && !pf.complete) {
        finalize_frame(seq, pf);
        try_deliver();
        return true;
    }

    return false;
}

void FrameAssembler::try_deliver() {
    if (!has_deliver_seq_) return;

    while (true) {
        auto it = pending_.find(next_deliver_seq_);
        if (it == pending_.end()) {
            // Frame not in pending. Either never received or was expired.
            // Skip only if we know newer frames exist — otherwise wait.
            int16_t ahead = static_cast<int16_t>(newest_seq_ - next_deliver_seq_);
            if (ahead <= 0) return;
            // Missing frame — count it as a drop so the view layer can
            // trigger an IDR request. Without this, totally-lost frames
            // stay invisible to drop detection and the decoder keeps
            // consuming P-frames whose reference chain is broken.
            frames_dropped_++;
            next_deliver_seq_++;
            continue;
        }
        auto& pf = it->second;
        if (!pf.complete) return; // wait for this frame to finish

        AssembledFrame frame;
        frame.seq_no = next_deliver_seq_;
        frame.timestamp = pf.timestamp;
        frame.keyframe = pf.keyframe;
        frame.heartbeat = pf.heartbeat;
        frame.data = std::move(pf.assembled);
        completed_.push(std::move(frame));
        frames_completed_++;
        pending_.erase(it);
        next_deliver_seq_++;
    }
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

    size_t total_nacked = 0;
    auto now = Clock::now();
    for (auto& kv : pending_) {
        if (total_nacked >= MAX_NACK_PER_CYCLE) break;

        uint16_t seq = kv.first;
        auto& pf = kv.second;
        if (pf.complete) continue;
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
            if (total_nacked >= MAX_NACK_PER_CYCLE) break;
            if (pf.arrived[i]) continue; // already have it

            // Rate-limit: skip if we requested recently
            auto last_nack = pf.nack_sent_at[i];
            if (last_nack != TimePoint{}) {
                auto since_nack = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_nack).count();
                if (since_nack < rate_limit_ms) continue;
            }
            batch.frag_indices.push_back(i);
            pf.nack_sent_at[i] = now;
            total_nacked++;

            // Protocol limits: 1 byte count field → max 255 per batch
            if (batch.frag_indices.size() >= 255) break;
        }
        if (!batch.frag_indices.empty()) {
            out.push_back(std::move(batch));
        }
    }
    return out;
}

void FrameAssembler::reset() {
    pending_.clear();
    std::queue<AssembledFrame> empty;
    std::swap(completed_, empty);
    has_seq_ = false;
    has_deliver_seq_ = false;
    newest_seq_ = 0;
    next_deliver_seq_ = 0;
}

void FrameAssembler::expire_stale() {
    // NOTE: counting happens exclusively in try_deliver()'s skip-forward path.
    // Erasing here just removes the stale pending entry; try_deliver will
    // then see the gap, bump frames_dropped_ once, and advance the cursor.
    auto now = Clock::now();
    bool any_dropped = false;
    auto it = pending_.begin();
    while (it != pending_.end()) {
        auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second.first_arrival).count();
        if (!it->second.complete && age_ms > FRAME_TIMEOUT_MS) {
            any_dropped = true;
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    if (any_dropped) try_deliver();
}

} // namespace vivora::net
