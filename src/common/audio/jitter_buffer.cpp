#include "common/audio/jitter_buffer.h"

#include <algorithm>

namespace deskbeam::audio {

static inline int16_t seq_diff(uint16_t a, uint16_t b) {
    return static_cast<int16_t>(a - b);
}

bool JitterBuffer::init(int frame_ms, int target_ms, int capacity_ms) {
    if (frame_ms <= 0 || target_ms <= 0 || capacity_ms < target_ms) return false;
    frame_ms_      = frame_ms;
    target_frames_ = std::max(1, target_ms / frame_ms);
    capacity_      = static_cast<size_t>(capacity_ms / frame_ms);
    reset();
    return true;
}

void JitterBuffer::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    ring_.assign(capacity_, Slot{});
    started_    = false;
    have_head_  = false;
    next_seq_   = 0;
    stored_     = 0;
}

void JitterBuffer::push(uint16_t seq, const uint8_t* data, size_t len) {
    if (len == 0 || !data) return;
    std::lock_guard<std::mutex> lk(mu_);

    if (!have_head_) {
        next_seq_  = seq;
        have_head_ = true;
    } else {
        int16_t diff = seq_diff(seq, next_seq_);
        if (diff < 0) {
            // Packet is behind play head.  If it's far behind (PLC ran away
            // while source was silent), resync to this packet's seq.
            if (diff < -static_cast<int16_t>(capacity_)) {
                ring_.assign(capacity_, Slot{});
                stored_    = 0;
                started_   = false;
                next_seq_  = seq;
            } else {
                // Mildly stale — drop.
                return;
            }
        }
        // If seq jumped far ahead of play head (> capacity), the stream
        // had a gap (e.g. audio source restart).  Reset and resync.
        if (static_cast<size_t>(diff) >= capacity_) {
            ring_.assign(capacity_, Slot{});
            stored_    = 0;
            started_   = false;
            next_seq_  = seq;
        }
    }

    size_t idx = seq % capacity_;
    Slot& s = ring_[idx];
    if (s.filled && s.seq == seq) {
        return; // duplicate
    }
    if (s.filled && s.seq != seq) {
        // Ring overwrite: stale slot being replaced by newer seq. Adjust count.
        stored_--;
    }
    s.filled = true;
    s.seq    = seq;
    s.data.assign(data, data + len);
    stored_++;

    if (!started_ && stored_ >= static_cast<size_t>(target_frames_)) {
        started_ = true;
    }
}

JitterBuffer::Status JitterBuffer::pop(std::vector<uint8_t>& out,
                                       uint16_t& out_seq) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return Status::Empty;

    size_t idx = next_seq_ % capacity_;
    Slot& s = ring_[idx];
    out_seq = next_seq_;

    if (s.filled && s.seq == next_seq_) {
        out = std::move(s.data);
        s.data.clear();
        s.filled = false;
        stored_--;
        next_seq_++;
        return Status::Data;
    }

    // Known gap: advance play head, signal PLC.
    next_seq_++;
    return Status::Missing;
}

size_t JitterBuffer::buffered_frames() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stored_;
}

} // namespace deskbeam::audio
