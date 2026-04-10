#pragma once

#include <algorithm>
#include <cstdint>

namespace deskbeam::codec {

// Hard limits for any computed/applied bitrate.
struct BitrateBounds {
    uint32_t min_bps = 1'000'000;     //   1 Mbps floor
    uint32_t max_bps = 100'000'000;   // 100 Mbps ceiling
};

// Heuristic default bitrate for a given resolution + frame rate.
//
// Tuned for HEVC remote desktop content (text-heavy, mostly static),
// where preserving small text edges costs more bits than typical
// camera footage. ~0.12 bits per pixel per frame.
//
//   720p60   →   ~6.6 Mbps
//   1080p60  →  ~14.9 Mbps
//   1440p60  →  ~26.5 Mbps
//   2160p60  →  ~59.7 Mbps
//   2880x1800@60 → ~37 Mbps
inline uint32_t default_bitrate_for(uint32_t width, uint32_t height, uint32_t fps) {
    constexpr double kBitsPerPixelPerFrame = 0.12;
    const double bps = static_cast<double>(width) * height * fps * kBitsPerPixelPerFrame;
    // Round to the nearest 100 kbps for cleaner logs.
    const uint64_t rounded = static_cast<uint64_t>((bps + 50'000.0) / 100'000.0) * 100'000ull;
    return static_cast<uint32_t>(rounded);
}

// Owns the encoder's "current" bitrate. Today the policy is trivial
// (default-from-resolution, optionally overridden by a manual target),
// but it accepts network-feedback inputs (RTT, loss, bandwidth estimate)
// so future congestion control can plug in without touching call sites.
//
// Usage:
//   BitrateController bc(default_bitrate_for(w, h, fps));
//   encoder.init({..., .bitrate_bps = bc.current()});
//   loop:
//     bc.on_rtt(session.rtt_ms());
//     bc.on_loss_ratio(...);
//     bool changed;
//     uint32_t br = bc.tick(&changed);
//     if (changed) encoder.set_bitrate(br);
class BitrateController {
public:
    BitrateController(uint32_t default_bps, BitrateBounds bounds = {})
        : bounds_(bounds), default_bps_(clamp(default_bps)), current_bps_(default_bps_) {}

    // Manual override (e.g., user-facing slider). Pass 0 to clear and
    // fall back to the default + automatic adaptation.
    void set_manual_target(uint32_t bps) {
        manual_target_ = bps == 0 ? 0 : clamp(bps);
    }
    uint32_t manual_target() const { return manual_target_; }

    // Network-feedback inputs (placeholders — not yet acted on).
    void on_rtt(double ms)                  { last_rtt_ms_ = ms; }
    void on_loss_ratio(double ratio)        { last_loss_ratio_ = ratio; }
    void on_estimated_bandwidth(uint32_t bps) { estimated_bw_bps_ = bps; }

    // Recompute the target bitrate. Returns the value to apply.
    // Sets *changed to true if it differs from the previously returned value.
    uint32_t tick(bool* changed = nullptr) {
        const uint32_t target = manual_target_ != 0 ? manual_target_ : default_bps_;
        // (Future: blend in adaptation from last_rtt_ms_ / last_loss_ratio_ /
        //  estimated_bw_bps_ here, with hysteresis and smoothing.)
        const uint32_t next = clamp(target);
        const bool diff = next != current_bps_;
        current_bps_ = next;
        if (changed) *changed = diff;
        return current_bps_;
    }

    uint32_t current()       const { return current_bps_; }
    uint32_t default_bps()   const { return default_bps_; }
    BitrateBounds bounds()   const { return bounds_; }

private:
    uint32_t clamp(uint32_t bps) const {
        return std::max(bounds_.min_bps, std::min(bounds_.max_bps, bps));
    }

    BitrateBounds bounds_;
    uint32_t default_bps_   = 0;
    uint32_t manual_target_ = 0;       // 0 = unset
    uint32_t current_bps_   = 0;
    // Network feedback (stored, not yet used).
    double   last_rtt_ms_      = 0.0;
    double   last_loss_ratio_  = 0.0;
    uint32_t estimated_bw_bps_ = 0;
};

} // namespace deskbeam::codec
