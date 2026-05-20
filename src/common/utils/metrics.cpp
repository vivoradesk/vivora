#include "common/utils/metrics.h"
#include "common/utils/log.h"

namespace vivora {

ScopedTimer::ScopedTimer(const char* tag, const char* operation)
    : tag_(tag), operation_(operation), start_(Clock::now()) {}

ScopedTimer::~ScopedTimer() {
    log::debug(tag_, "%s: %.2fms", operation_, elapsed_ms());
}

double ScopedTimer::elapsed_ms() const {
    auto elapsed = Clock::now() - start_;
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

} // namespace vivora
