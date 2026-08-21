// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "common/utils/types.h"
#include <string>

namespace vivora {

// Simple RAII timer that logs elapsed time on destruction
class ScopedTimer {
public:
    ScopedTimer(const char* tag, const char* operation);
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

    // Get elapsed time without stopping
    double elapsed_ms() const;

private:
    const char* tag_;
    const char* operation_;
    TimePoint start_;
};

} // namespace vivora
