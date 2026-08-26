// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// One assertion macro for the test suites (VIV-140).
//
// The suites used to check everything with assert(), which compiles to nothing
// under NDEBUG -- and Release is both the configuration we ship and the first
// one CI runs. That made 293 checks silent there, and worse: several suites
// wrapped the call under test inside the assert, so in Release the operation
// itself never happened and the test passed by doing nothing.
//
// CHECK always evaluates its expression. A failure is reported with file, line
// and the expression text, and counted; the run continues so one broken case
// does not hide the rest. main() ends with `return check_report("...")`, which
// prints the suite's success line only when nothing failed and turns the count
// into the process exit code ctest reads.
//
// Two shapes are accepted, so suites that already carried their own macro keep
// their call sites:
//     CHECK(cond)
//     CHECK(cond, "what this means")

#include <cstdio>

// Each test is a single translation unit, so internal linkage is enough and
// there is nothing to link against.
static int g_check_failures = 0;
static int g_check_total    = 0;

#define VIVORA_CHECK_1(expr)                                                   \
    do {                                                                       \
        ++g_check_total;                                                       \
        if (!(expr)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__,       \
                         #expr);                                               \
            ++g_check_failures;                                                \
        }                                                                      \
    } while (0)

#define VIVORA_CHECK_2(expr, msg)                                              \
    do {                                                                       \
        ++g_check_total;                                                       \
        if (!(expr)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s  (%s)\n", __FILE__, __LINE__, \
                         #expr, (msg));                                        \
            ++g_check_failures;                                                \
        }                                                                      \
    } while (0)

// The extra expansion is for MSVC's traditional preprocessor, which otherwise
// passes __VA_ARGS__ to the picker as a single argument and always selects the
// two-argument form.
#define VIVORA_CHECK_PICK(_1, _2, NAME, ...) NAME
#define VIVORA_CHECK_EXPAND(x) x
#define CHECK(...)                                                             \
    VIVORA_CHECK_EXPAND(VIVORA_CHECK_PICK(__VA_ARGS__, VIVORA_CHECK_2,         \
                                          VIVORA_CHECK_1)(__VA_ARGS__))

// Print the outcome and return the exit code for main().
static int check_report(const char* passed_message) {
    if (g_check_failures == 0) {
        if (passed_message && *passed_message) std::printf("%s\n", passed_message);
        return 0;
    }
    std::fprintf(stderr, "%d of %d checks FAILED\n", g_check_failures,
                 g_check_total);
    return 1;
}
