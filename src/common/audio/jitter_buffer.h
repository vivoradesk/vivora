#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace vivora::audio {

// Ring-based jitter buffer keyed by 16-bit sequence with adaptive target.
//
// - push()  — called from network thread on each received Opus packet.
// - pop()   — called from audio output thread at frame cadence (10ms).
//             Returns Data for available packet, Missing for a known gap
//             (caller should run PLC), Empty while prebuffering.
//
// Playback starts once `target_ms / frame_ms` packets have accumulated.
// After that, pop() advances one frame per call (in seq order) and
// adapts target_frames_ within [min, max] based on observed PLC rate:
//   - PLC rate > 5% over the window → grow target by one frame (10ms)
//     and pause one tick (Missing return) to refill the buffer.
//   - PLC rate < 0.5% AND buffer consistently fuller than target+slack
//     → shrink target by one frame and drop one stored frame.
// Bounds default to 30..150 ms so the runtime never strays outside what
// a viewer can tolerate either way.

class JitterBuffer {
public:
    enum class Status { Data, Missing, Empty };

    // frame_ms:    size of each packet (10)
    // target_ms:   initial target buffered latency at steady state
    // capacity_ms: ring capacity (keep >= max_target_ms * 4)
    // min_target_ms / max_target_ms: clamp range for adaptive growth.
    //   Defaults pick 30..200 so a 120 ms initial target has room to
    //   shrink when the link is clean and grow when it's bursty.
    bool init(int frame_ms, int target_ms, int capacity_ms = 800,
              int min_target_ms = 30, int max_target_ms = 200);

    void reset();

    // data may be nullptr only with len==0 (ignored). Thread-safe.
    void push(uint16_t seq, const uint8_t* data, size_t len);

    // Writes packet payload into `out`. Thread-safe.
    //
    // If `fec_source` is non-null and the head slot is Missing, and the
    // slot immediately after (next_seq+1) is filled, copies that next
    // packet's payload into *fec_source. The caller can feed it to
    // opus_decode with decode_fec=1 to reconstruct the missed frame from
    // the in-band FEC redundancy Opus embeds in each packet.
    Status pop(std::vector<uint8_t>& out, uint16_t& out_seq,
               std::vector<uint8_t>* fec_source = nullptr);

    size_t buffered_frames() const;

    // Current adaptive target in milliseconds (frames * frame_ms).
    int    target_ms() const;

private:
    void maybe_adapt_locked();

    struct Slot {
        bool filled = false;
        uint16_t seq = 0;
        std::vector<uint8_t> data;
    };

    int frame_ms_ = 10;
    int target_frames_ = 3;
    int min_target_frames_ = 3;
    int max_target_frames_ = 20;
    size_t capacity_ = 80;

    mutable std::mutex mu_;
    std::vector<Slot> ring_;
    bool started_ = false;
    bool have_head_ = false;
    uint16_t next_seq_ = 0;  // next seq to pop
    size_t stored_ = 0;

    // Adaptation state.  Window-based: count pops + Missing over a fixed
    // number of pops, then evaluate.  growth_pause_remaining_ ticks return
    // Missing without advancing the head so the ring refills by N frames.
    int pop_window_   = 0;
    int plc_window_   = 0;
    int consecutive_full_windows_ = 0;
    int growth_pause_remaining_   = 0;

    // Rate-limit "far ahead" resyncs.  Under heavy loss + reorder
    // (Clumsy 50% + delay) packets can arrive out of band in a way
    // that constantly trips the reset path — each reset flips
    // started_ back to false and the buffer never recovers.  Cap
    // resets to one per 500ms so a single genuine stream restart
    // still works but storms can't pin the buffer.
    std::chrono::steady_clock::time_point last_reset_ = {};
    static constexpr std::chrono::milliseconds RESET_MIN_INTERVAL{500};
    static constexpr int ADAPT_WINDOW_POPS = 100;          // ~1s at 10ms ticks
    static constexpr double GROW_PLC_RATE  = 0.05;         // 5%
    static constexpr double SHRINK_PLC_RATE = 0.005;       // 0.5%
    static constexpr int    SHRINK_FULL_WINDOWS = 5;       // 5s of clean + full
    static constexpr int    SHRINK_FULL_SLACK   = 3;       // stored > target + 3
};

} // namespace vivora::audio
