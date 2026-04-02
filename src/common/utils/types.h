#pragma once

#include <cstdint>
#include <chrono>

namespace deskbeam {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;

struct Resolution {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Rect {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

} // namespace deskbeam
