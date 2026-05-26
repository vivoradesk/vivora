#include "common/audio/jitter_buffer.h"
#include "common/utils/log.h"

#include <algorithm>

namespace vivora::audio {

static inline int16_t seq_diff(uint16_t a, uint16_t b) {
    return static_cast<int16_t>(a - b);
}

bool JitterBuffer::init(int frame_ms, int target_ms, int capacity_ms,
                        int min_target_ms, int max_target_ms) {
    if (frame_ms <= 0 || target_ms <= 0 || capacity_ms < max_target_ms) return false;
    if (min_target_ms <= 0 || max_target_ms < min_target_ms) return false;
    frame_ms_      = frame_ms;
    target_frames_ = std::max(1, target_ms / frame_ms);
    // Honour explicit caller target: widen the adaptive bounds to include
    // it rather than clamping it.  Tests routinely use 20ms targets which
    // are below the production 30ms floor; the floor is for live tuning,
    // not for clipping a deliberate caller choice.
    min_target_frames_ = std::min(target_frames_,
                                  std::max(1, min_target_ms / frame_ms));
    max_target_frames_ = std::max(target_frames_, max_target_ms / frame_ms);
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
    pop_window_ = 0;
    plc_window_ = 0;
    consecutive_full_windows_   = 0;
    growth_pause_remaining_     = 0;
}

int JitterBuffer::target_ms() const {
    std::lock_guard<std::mutex> lk(mu_);
    return target_frames_ * frame_ms_;
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
            // Packet is behind play head — late or reordered delivery.
            // Always drop; never reset.  Resetting on "far behind"
            // (which an earlier version did when diff < -capacity)
            // caused a wedge: clumsy / WiFi delays make some packets
            // arrive 200-500ms late, each one triggered a backwards
            // resync, the next normal packet then looked "far ahead"
            // and re-reset forwards, and the ping-pong kept started_
            // pinned to false so pop() returned Empty forever.
            return;
        }
        // If seq jumped far ahead of play head (> capacity), it's
        // either a legitimate stream restart or a single delayed
        // packet that we mistakenly trusted as "the new head".  Rate-
        // limit resyncs to once per RESET_MIN_INTERVAL — a real
        // restart still recovers in one tick, but a delay storm can
        // no longer keep retripping.
        if (static_cast<size_t>(diff) >= capacity_) {
            const auto now = std::chrono::steady_clock::now();
            // Rate-limit resets only while audio is playing — if we
            // already prebuffering (started_=false), we *need* the
            // reset to re-anchor next_seq_ to the live sender
            // position, otherwise the buffer stays stuck on a stale
            // next_seq_ that no incoming packet will ever match.
            if (started_ && now - last_reset_ < RESET_MIN_INTERVAL) {
                // Drop this outlier; pop marches forward via Missing.
                return;
            }
            last_reset_ = now;
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
                                       uint16_t& out_seq,
                                       std::vector<uint8_t>* fec_source) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return Status::Empty;

    // Adaptive growth: insert a PLC frame without advancing the head, so
    // the ring refills by one frame.  Caller's PLC fills the audible gap.
    if (growth_pause_remaining_ > 0) {
        growth_pause_remaining_--;
        out_seq = next_seq_;
        return Status::Missing;
    }

    pop_window_++;

    size_t idx = next_seq_ % capacity_;
    Slot& s = ring_[idx];
    out_seq = next_seq_;

    Status ret;
    if (s.filled && s.seq == next_seq_) {
        out = std::move(s.data);
        s.data.clear();
        s.filled = false;
        stored_--;
        next_seq_++;
        ret = Status::Data;
    } else {
        // Known gap. If the caller asked for FEC lookahead, expose the
        // next_seq+1 payload (without removing it) so it can try FEC-decode.
        if (fec_source) {
            fec_source->clear();
            const uint16_t peek_seq = static_cast<uint16_t>(next_seq_ + 1);
            const size_t peek_idx = peek_seq % capacity_;
            Slot& ns = ring_[peek_idx];
            if (ns.filled && ns.seq == peek_seq) {
                *fec_source = ns.data;
            }
        }
        next_seq_++;
        plc_window_++;
        ret = Status::Missing;
    }

    if (pop_window_ >= ADAPT_WINDOW_POPS) maybe_adapt_locked();
    return ret;
}

void JitterBuffer::maybe_adapt_locked() {
    const double plc_rate = static_cast<double>(plc_window_) / pop_window_;

    if (plc_rate > GROW_PLC_RATE && target_frames_ < max_target_frames_) {
        // Bursty losses suggest the buffer is too thin for current jitter.
        // Bump target by one frame (10ms) and pad one tick of PLC so the
        // ring actually grows by one frame.
        target_frames_++;
        growth_pause_remaining_ += 1;
        consecutive_full_windows_ = 0;
        log::info("JitterBuf",
                  "grow target -> %dms (plc_rate=%.1f%% in last %dms window)",
                  target_frames_ * frame_ms_, plc_rate * 100.0,
                  pop_window_ * frame_ms_);
    } else if (plc_rate < SHRINK_PLC_RATE
               && stored_ > static_cast<size_t>(target_frames_ + SHRINK_FULL_SLACK)) {
        if (++consecutive_full_windows_ >= SHRINK_FULL_WINDOWS
            && target_frames_ > min_target_frames_) {
            // Link is clean and the buffer keeps overshooting target —
            // shrink target and drop one stored frame to realise the new
            // latency immediately.  Drop the head packet (oldest); a single
            // 10ms drop is generally inaudible because we're in a quiet
            // / steady-state window.
            target_frames_--;
            consecutive_full_windows_ = 0;
            const size_t idx = next_seq_ % capacity_;
            Slot& s = ring_[idx];
            if (s.filled && s.seq == next_seq_) {
                s.filled = false;
                s.data.clear();
                stored_--;
            }
            next_seq_++;
            log::info("JitterBuf",
                      "shrink target -> %dms (plc_rate=%.2f%%, stored=%zu)",
                      target_frames_ * frame_ms_, plc_rate * 100.0, stored_);
        }
    } else {
        consecutive_full_windows_ = 0;
    }

    pop_window_ = 0;
    plc_window_ = 0;
}

size_t JitterBuffer::buffered_frames() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stored_;
}

} // namespace vivora::audio
