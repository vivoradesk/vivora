// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include "common/utils/log.h"

namespace vivora::codec {

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

    // Refresh the auto default when the real capture geometry becomes
    // known.  With the lazy encoder the resolution can be unknown (0x0)
    // when the controller is constructed at host-loop start — the default
    // then collapses to the 1 Mbps floor, and everything keyed off it
    // (warmup end, recovery ceiling) pins the session there.  Called on
    // the first-client path once dimensions are real.
    void update_default(uint32_t bps) {
        if (bps == 0) return;
        const uint32_t clamped = clamp(bps);
        if (clamped == default_bps_) return;
        log::info("BitrateCtl", "Auto default %u -> %u kbps (capture geometry known)",
                  default_bps_ / 1000, clamped / 1000);
        default_bps_ = clamped;
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
        last_cut_time_       = Clock::time_point{};
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
        // Hard cap: when the probe MEASURED real link headroom, allow the
        // recovery climb up to the resolution default — hard-capping at
        // WARMUP_CEILING pinned every session to 10 Mbps wire no matter the
        // link (probe said 91 Mbps, 3440x1440 wants ~35: still capped at 10 —
        // the last surviving piece of the VIV-79 ceiling, finished in VIV-84).
        // The gentle warmup still ramps only to WARMUP_CEILING; only the
        // small-step recovery climbs beyond it.  With no probe (0 bps) the
        // ceiling stays at the conservative WARMUP_CEILING as before.
        const uint32_t hard_cap = ceiling_override_bps_ > 0
                                ? ceiling_override_bps_
                                : std::max(WARMUP_CEILING_BPS, default_bps_);
        ceiling = std::min(ceiling, hard_cap);
        probe_ceiling_bps_ = std::max(ceiling, bounds_.min_bps);
        log::info("BitrateCtl", "Probe BW %u kbps -> ceiling %u kbps",
                  bps / 1000, probe_ceiling_bps_ / 1000);

        // The probe usually lands AFTER warmup has finished, and warmup
        // latched its recovery ceiling from whatever was known then -- which
        // was nothing, so it took the conservative WARMUP_CEILING.  Nothing
        // revisited that, so every session stayed pinned at 10 Mbps no matter
        // what the link turned out to be: measured here at 1.7 Gbps of
        // headroom on a LAN, with a 1920x1200 target of 16.6 Mbps, and the
        // picture capped at 10.
        //
        // Raise it, never lower it.  Lowering would fight the loss governors,
        // which own every downward move; this only restores the headroom the
        // probe was run to find in the first place, and the climb toward it
        // is still the small-step one under those same governors.
        if (recovery_ceiling_bps_ > 0 && probe_ceiling_bps_ > recovery_ceiling_bps_) {
            log::info("BitrateCtl",
                      "Recovery ceiling %u -> %u kbps (probe landed after warmup)",
                      recovery_ceiling_bps_ / 1000, probe_ceiling_bps_ / 1000);
            recovery_ceiling_bps_ = probe_ceiling_bps_;
        }
    }

    // Diagnostic: raise the hard ceiling above WARMUP_CEILING_BPS so a
    // probe with high measured BW can drive the ramp further. Pass 0 to
    // clear and restore the default 10 Mbps cap.
    void set_ceiling_override(uint32_t bps) {
        ceiling_override_bps_ = bps;
    }

    // Hard clamp requested by a viewer (tightest cap across clients, from
    // PerfReport; 0 = none).  Applied inside clamp() so every adaptation
    // path — warmup, recovery, damage cuts — respects it, and the current
    // bitrate is pulled down immediately when the cap tightens.
    void set_client_cap(uint32_t bps) {
        if (client_cap_bps_ == bps) return;
        client_cap_bps_ = bps;
        if (bps != 0 && current_bps_ > bps) {
            current_bps_ = clamp(bps);
            log::info("BitrateCtl", "Client cap %u kbps -> bitrate lowered",
                      bps / 1000);
        }
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
    void on_rtt(double ms) {
        last_rtt_ms_ = ms;
        // Track the path's base RTT.  Slow upward decay (~+3%/min at 2Hz
        // ticks) lets the floor re-learn if the route genuinely changes.
        if (ms > 0.0) {
            if (rtt_floor_ <= 0.0 || ms < rtt_floor_) rtt_floor_ = ms;
            else rtt_floor_ *= 1.00025;
        }
    }
    void on_estimated_bandwidth(uint32_t bps) { estimated_bw_bps_ = bps; }

    // Actual wire throughput (host-side bytes really sent per second).
    // Gates the recovery climb — see tick().  0 = unknown (gate disabled).
    void on_wire_usage(uint32_t bps) { last_wire_bps_ = bps; }

    // Share of wire spent re-sending lost packets (NACK retx EWMA, 0..1).
    // High recovery traffic means the redundancy machinery is straining —
    // that gates the CLIMB but is never a reason to cut (recovered loss is
    // FEC/NACK working, not congestion — VIV-84).
    void on_recovery_traffic(double ratio) { recovery_traffic_ = ratio; }

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
            // The channel chokes on a fast warm-up jump (VIV-82: a 14→25 Mbps
            // step in one go made the link drop it).  Ramp the warm-up gently
            // to a MODEST ceiling only; the gradual per-cycle recovery below
            // then climbs from there to the full probe/override ceiling in
            // small steps the channel can actually follow.
            const uint32_t full_ceiling = probe_ceiling_bps_ > 0
                                        ? probe_ceiling_bps_
                                        : (ceiling_override_bps_ > 0
                                               ? ceiling_override_bps_
                                               : WARMUP_CEILING_BPS);
            const uint32_t warmup_end = std::min(base, WARMUP_CEILING_BPS);
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
            warmup_active_     = false;
            warmup_just_ended_ = true;
            // Recovery may climb to the FULL ceiling — gently, in small
            // steps under its own loss governors.  Do NOT min() this with
            // `base`: when warmup ends damaged (e.g. the BW probe's burst
            // cost the ramp its budget) or `base` is a floor-collapsed
            // auto default, locking recovery to it pins the whole session
            // at ~1 Mbps with no way back up.
            recovery_ceiling_bps_ = full_ceiling;
            log::info("BitrateCtl",
                      "Warmup done at %u kbps, recovery ceiling = %u kbps",
                      warmup_end / 1000, recovery_ceiling_bps_ / 1000);
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
            last_cut_time_ = now;
        };

        // Per-tick decision trace (VIVORA_BR_TRACE=1): every input and the
        // branch taken, ~2Hz.  The climb stalls have been misdiagnosed twice
        // from aggregate logs alone (VIV-84) — this shows the controller's
        // actual view of the world.
        static const bool trace = [] {
            const char* e = std::getenv("VIVORA_BR_TRACE");
            return e && e[0] == '1';
        }();
        const char* decision = "hold";
        const double loss_for_trace = loss_pending_;  // consumed below

        uint32_t next;
        if (manual_target_ != 0) {
            next = base;
        } else if (loss_pending_ > 0.02) {
            // Loss above the gentle floor.  A SINGLE high-loss cycle is usually
            // a transient WiFi burst that FEC/NACK already recovered (drop stays
            // ~0) — cratering the bitrate 15→6 on it, then slowly climbing back
            // into the next burst, is exactly the "doesn't hold high" behaviour
            // (VIV-82).  So the first high-loss cycle only trims gently and
            // keeps the climb alive; the full cut + recovery penalty applies
            // only once loss is SUSTAINED across cycles (real congestion).
            ++high_loss_streak_;
            double mult;
            if (high_loss_streak_ < HIGH_LOSS_SUSTAIN) {
                mult = 0.92;                      // transient burst — gentle trim
            } else if (loss_pending_ > 0.08) {
                mult = 0.5;                       // sustained severe
            } else if (loss_pending_ > 0.04) {
                mult = 0.7;                       // sustained heavy
            } else {
                mult = 0.85;                      // sustained early
            }
            next = static_cast<uint32_t>(current_bps_ * mult);
            if (next < adapt_floor) next = adapt_floor;
            if (high_loss_streak_ >= HIGH_LOSS_SUSTAIN) {
                decision = "CUT";
                on_cut();                         // penalize recovery: real congestion
            } else {
                decision = "trim";
                // Transient: consume and KEEP the climb armed.  Zeroing
                // stable_cycles_ here paused the climb for recover_delay
                // (~5s) after every stray micro-burst — with bursts every
                // ~15-30s on WiFi the bitrate sawtoothed just under the
                // ceiling forever instead of finding the link's real rate
                // (VIV-84).  If the loss persists, the next cycle reaches
                // HIGH_LOSS_SUSTAIN and takes the real cut path anyway.
                loss_pending_  = 0.0;
            }
        } else {
            // Loss ≤ 2% — channel healthy, hold or recover.
            high_loss_streak_ = 0;
            // Count as "stable" for recovery for any sub-cut loss.  A WiFi link
            // with ~1.5-1.8% baseline loss that FEC fully recovers (failed=0)
            // used to sit in a dead zone — above the old 1.5% stable threshold
            // but below the 2% cut threshold — so recovery never accumulated
            // and the bitrate stuck at ~6 Mbps wire (VIV-82).  Recoverable loss
            // below the cut threshold must not block the climb.
            if (loss_pending_ < 0.02) ++stable_cycles_;
            else stable_cycles_ = 0;
            loss_pending_ = 0.0;

            // Utilization gate: only raise the target when the wire actually
            // CARRIES close to the current target.  Static content uses a
            // fraction of the target (heartbeats, tiny P-frames), so the
            // channel is never exercised and produces no loss signal — the
            // target then climbs on fantasy ("static grew to 30 Mbps") and
            // the first dynamic scene slams the full target onto a radio
            // that never proved it, bursting losses and cratering to the
            // floor (VIV-84).  Holding until utilization ≥60% keeps the
            // target within ~1.7× of PROVEN throughput; content transitions
            // then start from a rate the link has actually carried.
            const bool wire_proven = last_wire_bps_ == 0  // unknown → no gate
                || static_cast<uint64_t>(last_wire_bps_) * 10
                   >= static_cast<uint64_t>(current_bps_) * 6;
            // Early-warning climb guards (never cut, only pause the climb):
            //  * recovery traffic — the link is already leaning on NACK retx
            //    to deliver; growing now deepens the strain (VIV-84);
            //  * RTT inflation over the path's base — queues are building,
            //    the classic delay signal that precedes packet damage.
            const bool recovery_calm = recovery_traffic_ < 0.15;
            const bool rtt_calm = rtt_floor_ <= 0.0 || last_rtt_ms_ <= 0.0
                || last_rtt_ms_ <= rtt_floor_ + RTT_CLIMB_HEADROOM_MS;
            if (!wire_proven)                          decision = "gated";
            else if (!recovery_calm)                   decision = "retx-hot";
            else if (!rtt_calm)                        decision = "rtt-hot";
            else if (current_bps_ >= recover_cap)      decision = "at-cap";
            else if (stable_cycles_ < recover_delay_)  decision = "warming";
            if (stable_cycles_ >= recover_delay_ && current_bps_ < recover_cap
                && wire_proven && recovery_calm && rtt_calm) {
                decision = "climb";
                // +1/recovery_divisor of cap per cycle.
                // Base is 50 (+2%). After each up-then-down it doubles:
                // 50→100→200→400 (2% → 1% → 0.5% → 0.25%). A long stable
                // run (RECOVER_RESET_CYCLES) resets back to base.
                uint32_t step = recover_cap / recovery_divisor_;
                // Cap the absolute climb rate: the channel reacts badly to fast
                // bitrate jumps, and a big step overshoots the sustainable rate
                // and then crashes (VIV-82).  ≤MAX_RECOVER_STEP per 500ms cycle
                // = ~0.5 Mbps/s, regardless of how high the ceiling is.
                if (step > MAX_RECOVER_STEP) step = MAX_RECOVER_STEP;
                if (step == 0) step = 1;
                next = std::min(recover_cap, current_bps_ + step);
                had_growth_ = true;
            } else {
                next = current_bps_;
            }

            // Reset recovery step aggressiveness back to base only after a
            // long stable run AND enough wall-clock distance from the last
            // cut. Without the wall-clock guard a repeating 60s cycle (cut
            // → 30s stable → reset → fast climb → cut) keeps thrashing the
            // channel. 60s of no-cut history proves the channel really is
            // the capacity we think it is.
            const auto since_cut_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_cut_time_).count();
            if (stable_cycles_ >= RECOVER_RESET_CYCLES && since_cut_ms >= CUT_COOLDOWN_MS) {
                recovery_divisor_ = RECOVERY_DIVISOR_BASE;
                recover_delay_    = RECOVER_DELAY_BASE;
            }
        }

        if (trace) {
            log::info("BRTRACE",
                "%s: cur=%u -> next=%u | eff-loss=%.2f%% streak=%d retx=%.1f%% "
                "| wire=%u (%.0f%% of cur) | stable=%u/%u div=%u | cap=%u ceil=%u "
                "| rtt=%.1f (base %.1f)",
                decision, current_bps_ / 1000, clamp(next) / 1000,
                loss_for_trace * 100.0, high_loss_streak_,
                recovery_traffic_ * 100.0,
                last_wire_bps_ / 1000,
                current_bps_ ? 100.0 * last_wire_bps_ / current_bps_ : 0.0,
                stable_cycles_, recover_delay_, recovery_divisor_,
                recover_cap / 1000, recovery_ceiling_bps_ / 1000,
                last_rtt_ms_, rtt_floor_);
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
        uint32_t hi = bounds_.max_bps;
        if (client_cap_bps_ != 0 && client_cap_bps_ < hi) hi = client_cap_bps_;
        return std::max(bounds_.min_bps, std::min(hi, bps));
    }

    static constexpr int64_t ADAPT_MS = 500;             // run adaptation every 500ms
    static constexpr uint32_t RECOVER_DELAY_BASE   = 10; // ~5 sec of low loss before recovery
    static constexpr uint32_t RECOVER_DELAY_MAX    = 60; // cap at ~30 sec post-cut cooldown
    static constexpr uint32_t RECOVER_RESET_CYCLES = 60; // ~30 sec stable → reset recovery step
    static constexpr int64_t  WARMUP_MS            = 5000;       // linear ramp duration on connect
    static constexpr uint32_t WARMUP_START_BPS     = 7'000'000;  // cold-start bitrate
    static constexpr uint32_t WARMUP_CEILING_BPS   = 10'000'000; // safe upper bound for the ramp
    static constexpr uint32_t RECOVERY_DIVISOR_BASE = 100; // +1% of default per cycle
    static constexpr uint32_t RECOVERY_DIVISOR_MAX  = 400; // floor at +0.25%
    static constexpr int64_t  CUT_COOLDOWN_MS       = 60000; // 60s quarantine before resetting recovery step
    // 3 cycles (~1.5s) of continuous high loss before a full cut.  Real
    // congestion easily lasts that long; a one-off keyframe burst + its NACK
    // recovery smears across two adjacent 500ms windows and used to be
    // misread as "sustained" at 2 (VIV-84).
    static constexpr int      HIGH_LOSS_SUSTAIN     = 3;
    static constexpr uint32_t MAX_RECOVER_STEP      = 250'000; // ≤0.25M/cycle = ~0.5 Mbps/s climb cap
    static constexpr double   RTT_CLIMB_HEADROOM_MS = 20.0;    // climb only while RTT ≤ base + this

    BitrateBounds bounds_;
    uint32_t default_bps_   = 0;
    uint32_t manual_target_ = 0;       // 0 = unset
    int      high_loss_streak_ = 0;    // consecutive >2% loss cycles (transient vs sustained)
    uint32_t current_bps_   = 0;
    uint32_t stable_cycles_ = 0;       // consecutive adapt cycles with loss < 3%
    double   loss_pending_  = 0.0;     // max loss since last adaptation; consumed after cut
    Clock::time_point last_adapt_time_   = Clock::now();
    Clock::time_point warmup_start_time_ = Clock::now();
    Clock::time_point last_cut_time_     = Clock::time_point{};  // epoch = never cut yet
    uint32_t recovery_divisor_           = RECOVERY_DIVISOR_BASE;
    uint32_t recover_delay_              = RECOVER_DELAY_BASE;
    bool     had_growth_                 = false;
    bool     warmup_active_              = false;
    bool     warmup_just_ended_          = false;
    uint32_t probe_ceiling_bps_          = 0;
    uint32_t recovery_ceiling_bps_       = 0;  // post-warmup cap for additive recovery
    uint32_t ceiling_override_bps_       = 0;  // diagnostic: raise hard cap above WARMUP_CEILING_BPS
    uint32_t client_cap_bps_             = 0;  // viewer-requested hard cap (0 = none)
    uint32_t client_count_               = 1;
    // Network feedback.
    double   last_rtt_ms_      = 0.0;
    double   rtt_floor_        = 0.0;  // path base RTT (min, slow decay); 0 = unknown
    double   recovery_traffic_ = 0.0;  // NACK retx share of wire (EWMA)
    uint32_t estimated_bw_bps_ = 0;
    uint32_t last_wire_bps_    = 0;   // actual sent throughput; 0 = unknown
};

} // namespace vivora::codec
