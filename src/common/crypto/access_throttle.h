// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <unordered_map>

namespace vivora::crypto {

// Rate limit for wrong unattended-access passwords (VIV-151).
//
// Two limits, both on failures only:
//   - per source IP: `per_source_limit` failures inside `window_ms` lock that
//     address out for `source_lockout_ms`;
//   - global: `global_limit` failures inside `window_ms`, from anywhere, lock
//     password logins out for everyone for `global_lockout_ms`.  This is what
//     stops an attacker who rotates addresses.  It can be used to keep the
//     owner out for a while too, which is the price: the approval prompt and
//     trusted devices keep working during a lockout, only the password path
//     closes.
//
// Not thread-safe; HostSession calls it from its own thread.  Time is passed
// in so tests can drive it.
class AccessThrottle {
public:
    struct Config {
        int     per_source_limit  = 10;
        int     global_limit      = 30;
        int64_t window_ms         = 10 * 60 * 1000;
        int64_t source_lockout_ms = 15 * 60 * 1000;
        int64_t global_lockout_ms = 10 * 60 * 1000;
    };

    AccessThrottle() = default;
    explicit AccessThrottle(const Config& cfg) : cfg_(cfg) {}

    // 0 when a password attempt from `ip` may be tried now, otherwise how
    // many milliseconds until it may.
    int64_t blocked_for_ms(uint32_t ip, int64_t now_ms) const;

    // A wrong password from `ip`.  Returns true when this failure started a
    // lockout (per source or global), so the caller can log it once.
    bool record_failure(uint32_t ip, int64_t now_ms);

    // A correct password from `ip` clears that address's count.  The global
    // count is left alone: one good login must not hide a spray.
    void record_success(uint32_t ip);

private:
    struct Counter {
        int64_t window_start = 0;
        int     failures     = 0;
        int64_t locked_until = 0;
    };
    // Counts one failure into `c`; true when it reached `limit` and locked.
    bool bump(Counter& c, int limit, int64_t lockout_ms, int64_t now_ms) const;
    void prune(int64_t now_ms);

    Config cfg_;
    Counter global_;
    std::unordered_map<uint32_t, Counter> sources_;
};

} // namespace vivora::crypto
