// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "common/crypto/access_throttle.h"

#include <algorithm>

namespace vivora::crypto {

namespace {
// Bound the per-source table: a spray from many addresses must not grow it
// without limit.  The global counter still sees every failure.
constexpr size_t kMaxSources = 4096;
} // namespace

int64_t AccessThrottle::blocked_for_ms(uint32_t ip, int64_t now_ms) const {
    int64_t wait = std::max<int64_t>(0, global_.locked_until - now_ms);
    auto it = sources_.find(ip);
    if (it != sources_.end())
        wait = std::max(wait, it->second.locked_until - now_ms);
    return wait;
}

bool AccessThrottle::bump(Counter& c, int limit, int64_t lockout_ms,
                          int64_t now_ms) const {
    if (c.failures == 0 || now_ms - c.window_start >= cfg_.window_ms) {
        c.window_start = now_ms;
        c.failures     = 0;
    }
    if (++c.failures < limit) return false;
    // Start counting afresh once the lockout ends.
    c.locked_until = now_ms + lockout_ms;
    c.failures     = 0;
    return true;
}

bool AccessThrottle::record_failure(uint32_t ip, int64_t now_ms) {
    if (sources_.size() >= kMaxSources && !sources_.count(ip)) prune(now_ms);
    bool locked = bump(global_, cfg_.global_limit, cfg_.global_lockout_ms, now_ms);
    if (sources_.size() < kMaxSources || sources_.count(ip))
        locked |= bump(sources_[ip], cfg_.per_source_limit,
                       cfg_.source_lockout_ms, now_ms);
    return locked;
}

void AccessThrottle::record_success(uint32_t ip) {
    sources_.erase(ip);
}

void AccessThrottle::prune(int64_t now_ms) {
    for (auto it = sources_.begin(); it != sources_.end();) {
        const Counter& c = it->second;
        const bool idle = now_ms - c.window_start >= cfg_.window_ms
                       && c.locked_until <= now_ms;
        it = idle ? sources_.erase(it) : std::next(it);
    }
}

} // namespace vivora::crypto
