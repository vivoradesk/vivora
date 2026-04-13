#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include "common/utils/log.h"

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
    using Clock = std::chrono::steady_clock;

    BitrateController(uint32_t default_bps, BitrateBounds bounds = {})
        : bounds_(bounds), default_bps_(clamp(default_bps)), current_bps_(default_bps_) {}

    // Manual override (e.g., user-facing slider). Pass 0 to clear and
    // fall back to the default + automatic adaptation.
    void set_manual_target(uint32_t bps) {
        manual_target_ = bps == 0 ? 0 : clamp(bps);
    }
    uint32_t manual_target() const { return manual_target_; }

    // Called when a (new) client connects. Arms the warm-up ramp:
    // starts bitrate at WARMUP_START_BPS and linearly climbs to
    // default_bps_ over WARMUP_MS. Motivation: when a client first
    // connects, the CPU/GPU/WiFi radio on both sides are in idle
    // power states and can't absorb a full-bitrate IDR storm —
    // frequencies need a few seconds to ramp up. A linear ramp lets
    // everything warm up gradually instead of slamming max cold.
    void notify_client_connected() {
        warmup_start_time_   = Clock::now();
        current_bps_         = clamp(WARMUP_START_BPS);
        warmup_active_       = true;
        warmup_just_ended_   = false;
        recovery_divisor_    = RECOVERY_DIVISOR_BASE;
        recover_delay_       = RECOVER_DELAY_BASE;
        had_growth_          = false;
        stable_cycles_       = 0;
        loss_pending_        = 0.0;
        probe_ceiling_bps_   = 0;
    }

    // One-shot flag: true on the tick where the warm-up ramp ends.
    // Caller polls it to re-baseline external counters (e.g. retx
    // samples) so warm-up bursts don't pollute the first post-warmup
    // adaptation sample.
    bool consume_grace_ended_flag() {
        bool v = warmup_just_ended_;
        warmup_just_ended_ = false;
        return v;
    }

    // True while the warm-up ramp is active.
    bool in_warmup() const { return warmup_active_; }

    // Set measured bandwidth from probe. Overrides WARMUP_CEILING_BPS
    // with measured_bw * 0.75 (leave headroom for FEC + retx overhead).
    void set_probe_bandwidth(uint32_t bps) {
        if (bps == 0 || probe_ceiling_bps_ > 0) return;
        uint32_t ceiling = static_cast<uint32_t>(bps * 0.75);
        ceiling = std::min(ceiling, WARMUP_CEILING_BPS);
        probe_ceiling_bps_ = std::max(ceiling, bounds_.min_bps);
        log::info("BitrateCtl", "Probe BW %u kbps -> ceiling %u kbps",
                  bps / 1000, probe_ceiling_bps_ / 1000);
    }

    // Force a specific bitrate (e.g. when client count changes and the
    // current bitrate is already too high for the new N).
    void force_bitrate(uint32_t bps) {
        current_bps_ = clamp(bps);
        stable_cycles_ = 0;
        loss_pending_ = 0.0;
    }

    // Tell the controller how many clients share the outbound link.
    // The recovery ceiling is divided by this count so that total wire
    // rate (bitrate × N) stays within the channel capacity.
    void set_client_count(size_t n) {
        client_count_ = n > 0 ? static_cast<uint32_t>(n) : 1;
    }

    // Network-feedback inputs.
    void on_rtt(double ms)                  { last_rtt_ms_ = ms; }
    void on_estimated_bandwidth(uint32_t bps) { estimated_bw_bps_ = bps; }

    // Feed a new loss report. Accumulates max loss since last adaptation.
    void on_loss_ratio(double ratio) {
        if (ratio > loss_pending_) loss_pending_ = ratio;
    }

    // Recompute the target bitrate. Returns the value to apply.
    // Sets *changed to true if it differs from the previously returned value.
    //
    // Loss-adaptive policy (wall-clock gated, ~2Hz):
    //   loss >  8% → ×0.5  (aggressive cut, severe congestion)
    //   loss >  4% → ×0.7  (strong cut, FEC already overloaded)
    //   loss >  2% → ×0.85 (early gentle cut — start backing off before
    //                       loss spirals out of control)
    //   loss < 1.5% for ~5 sec → additive increase (+5% of default per cycle)
    // Floor: 1/3 of default (lowered from 1/2 — deep WiFi congestion
    // needs room to shrink further before unwatchable-quality kicks in).
    // Manual target disables adaptation.
    uint32_t tick(bool* changed = nullptr) {
        const uint32_t base = manual_target_ != 0 ? manual_target_ : default_bps_;

        // Wall-clock rate limiter: adapt every ADAPT_MS, regardless of loop speed.
        auto now = Clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_adapt_time_).count();
        if (elapsed < ADAPT_MS) {
            if (changed) *changed = false;
            return current_bps_;
        }
        last_adapt_time_ = now;

        // Warm-up ramp: linearly climb from WARMUP_START_BPS → default
        // over WARMUP_MS. Disables cut logic during ramp; the ramp itself
        // is what limits bitrate, not loss feedback.
        if (warmup_active_) {
            auto warmup_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - warmup_start_time_).count();
            loss_pending_ = 0.0;
            stable_cycles_ = 0;
            // Ramp target is capped: on real-world WiFi the full default
            // is usually unreachable, and ramping all the way to it just
            // produces a post-warmup cascade of cuts. Cap at measured BW
            // from probe (if available), else use static ceiling.
            const uint32_t ceiling = probe_ceiling_bps_ > 0
                                   ? probe_ceiling_bps_ : WARMUP_CEILING_BPS;
            const uint32_t warmup_end = std::min(base, ceiling);
            if (warmup_elapsed < WARMUP_MS) {
                const double t = static_cast<double>(warmup_elapsed)
                               / static_cast<double>(WARMUP_MS);
                const uint32_t start = clamp(WARMUP_START_BPS);
                const double delta = static_cast<double>(warmup_end)
                                   - static_cast<double>(start);
                const uint32_t next = static_cast<uint32_t>(
                    static_cast<double>(start) + delta * t);
                const uint32_t clamped = clamp(next);
                const bool diff = clamped != current_bps_;
                current_bps_ = clamped;
                if (changed) *changed = diff;
                return current_bps_;
            }
            // Ramp finished: snap to the capped end, raise flag.
            // Lock post-warmup recovery to the same ceiling — the full
            // default_bps_ is usually unreachable on WiFi and recovering
            // toward it just causes congestion oscillation.
            warmup_active_     = false;
            warmup_just_ended_ = true;
            recovery_ceiling_bps_ = warmup_end;
            log::info("BitrateCtl", "Warmup done, recovery ceiling = %u kbps",
                      warmup_end / 1000);
            const bool diff = warmup_end != current_bps_;
            current_bps_ = warmup_end;
            if (changed) *changed = diff;
            return current_bps_;
        }

        // Recovery target: if we came through warmup, cap at the ceiling
        // we discovered (probe or static). Recovering toward the full
        // default on constrained links (WiFi, hotspot) just causes
        // repeated congestion → cut → recovery → congestion oscillation.
        // With N clients, total wire rate = bitrate × N. Divide the
        // ceiling so the aggregate stays within channel capacity.
        const uint32_t raw_ceiling = recovery_ceiling_bps_ > 0
                                    ? std::min(base, recovery_ceiling_bps_)
                                    : base;
        const uint32_t recover_cap = std::max(bounds_.min_bps,
                                              raw_ceiling / client_count_);

        // Absolute floor: prefer blocky picture over full freezes on bad WiFi.
        // Must not exceed recover_cap — otherwise a cut floors above the
        // ceiling and recovery can never bring it back down.
        const uint32_t adapt_floor = std::min(
            std::max(bounds_.min_bps, 6'000'000u / client_count_),
            recover_cap);

        // Helper lambda: a cut just happened. If we were previously
        // growing toward the ceiling, this is the "up-then-down" pattern
        // — the channel can't actually hold recovered bitrate. Halve the
        // recovery step AND double the post-cut cooldown so next climb
        // is both slower and starts later, killing short oscillations.
        auto on_cut = [&]() {
            if (had_growth_) {
                if (recovery_divisor_ < RECOVERY_DIVISOR_MAX)
                    recovery_divisor_ *= 2;
                if (recover_delay_ < RECOVER_DELAY_MAX)
                    recover_delay_ *= 2;
            }
            had_growth_    = false;
            loss_pending_  = 0.0;
            stable_cycles_ = 0;
        };

        uint32_t next;
        if (manual_target_ != 0) {
            next = base;
        } else if (loss_pending_ > 0.08) {
            // Severe loss (>8%) — aggressive cut.
            next = static_cast<uint32_t>(current_bps_ * 0.5);
            if (next < adapt_floor) next = adapt_floor;
            on_cut();
        } else if (loss_pending_ > 0.04) {
            // Heavy loss (4-8%) — FEC overloaded, strong cut.
            next = static_cast<uint32_t>(current_bps_ * 0.7);
            if (next < adapt_floor) next = adapt_floor;
            on_cut();
        } else if (loss_pending_ > 0.02) {
            // Early loss (2-4%) — FEC still handling it, but start
            // backing off before retx storm and cascading degradation.
            next = static_cast<uint32_t>(current_bps_ * 0.85);
            if (next < adapt_floor) next = adapt_floor;
            on_cut();
        } else {
            // Loss ≤ 2% — channel healthy, hold or recover.
            // Only count as "stable" for recovery if loss < 1.5%.
            if (loss_pending_ < 0.015) ++stable_cycles_;
            else stable_cycles_ = 0;
            loss_pending_ = 0.0;

            if (stable_cycles_ >= recover_delay_ && current_bps_ < recover_cap) {
                // +1/recovery_divisor of cap per cycle.
                // Base is 50 (+2%). After each up-then-down it doubles:
                // 50→100→200→400 (2% → 1% → 0.5% → 0.25%). A long stable
                // run (RECOVER_RESET_CYCLES) resets back to base.
                uint32_t step = recover_cap / recovery_divisor_;
                if (step == 0) step = 1;
                next = std::min(recover_cap, current_bps_ + step);
                had_growth_ = true;
            } else {
                next = current_bps_;
            }

            if (stable_cycles_ >= RECOVER_RESET_CYCLES) {
                recovery_divisor_ = RECOVERY_DIVISOR_BASE;
                recover_delay_    = RECOVER_DELAY_BASE;
            }
        }

        next = clamp(next);
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

    static constexpr int64_t ADAPT_MS = 500;             // run adaptation every 500ms
    static constexpr uint32_t RECOVER_DELAY_BASE   = 10; // ~5 sec of low loss before recovery
    static constexpr uint32_t RECOVER_DELAY_MAX    = 60; // cap at ~30 sec post-cut cooldown
    static constexpr uint32_t RECOVER_RESET_CYCLES = 60; // ~30 sec stable → reset recovery step
    static constexpr int64_t  WARMUP_MS            = 5000;       // linear ramp duration on connect
    static constexpr uint32_t WARMUP_START_BPS     = 7'000'000;  // cold-start bitrate
    static constexpr uint32_t WARMUP_CEILING_BPS   = 10'000'000; // safe upper bound for the ramp
    static constexpr uint32_t RECOVERY_DIVISOR_BASE = 50;  // +2% of default per cycle
    static constexpr uint32_t RECOVERY_DIVISOR_MAX  = 400; // floor at +0.25%

    BitrateBounds bounds_;
    uint32_t default_bps_   = 0;
    uint32_t manual_target_ = 0;       // 0 = unset
    uint32_t current_bps_   = 0;
    uint32_t stable_cycles_ = 0;       // consecutive adapt cycles with loss < 3%
    double   loss_pending_  = 0.0;     // max loss since last adaptation; consumed after cut
    Clock::time_point last_adapt_time_   = Clock::now();
    Clock::time_point warmup_start_time_ = Clock::now();
    uint32_t recovery_divisor_           = RECOVERY_DIVISOR_BASE;
    uint32_t recover_delay_              = RECOVER_DELAY_BASE;
    bool     had_growth_                 = false;
    bool     warmup_active_              = false;
    bool     warmup_just_ended_          = false;
    uint32_t probe_ceiling_bps_          = 0;
    uint32_t recovery_ceiling_bps_       = 0;  // post-warmup cap for additive recovery
    uint32_t client_count_               = 1;
    // Network feedback.
    double   last_rtt_ms_      = 0.0;
    uint32_t estimated_bw_bps_ = 0;
};

} // namespace deskbeam::codec
