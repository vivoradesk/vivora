#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace deskbeam::audio {

// Simple ring-based jitter buffer keyed by 16-bit sequence.
//
// - push()  — called from network thread on each received Opus packet.
// - pop()   — called from audio output thread at frame cadence (10ms).
//             Returns Data for available packet, Missing for a known gap
//             (caller should run PLC), Empty while prebuffering.
//
// Playback starts once `target_ms / frame_ms` packets have accumulated.
// After that, pop() always advances one frame per call (in seq order).

class JitterBuffer {
public:
    enum class Status { Data, Missing, Empty };

    // frame_ms:  size of each packet (10)
    // target_ms: target buffered latency at steady state (30)
    // capacity_ms: ring capacity (keep >= target_ms * 4)
    bool init(int frame_ms, int target_ms, int capacity_ms = 200);

    void reset();

    // data may be nullptr only with len==0 (ignored). Thread-safe.
    void push(uint16_t seq, const uint8_t* data, size_t len);

    // Writes packet payload into `out`. Thread-safe.
    Status pop(std::vector<uint8_t>& out, uint16_t& out_seq);

    size_t buffered_frames() const;

private:
    struct Slot {
        bool filled = false;
        uint16_t seq = 0;
        std::vector<uint8_t> data;
    };

    int frame_ms_ = 10;
    int target_frames_ = 3;
    size_t capacity_ = 20;

    mutable std::mutex mu_;
    std::vector<Slot> ring_;
    bool started_ = false;
    bool have_head_ = false;
    uint16_t next_seq_ = 0;  // next seq to pop
    size_t stored_ = 0;
};

} // namespace deskbeam::audio
