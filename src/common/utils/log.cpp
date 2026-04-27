#include "common/utils/log.h"
#include <cstdio>
#include <cstdarg>
#include <chrono>

namespace deskbeam::log {

static Level g_level = Level::Debug;

void set_level(Level level) { g_level = level; }
Level get_level() { return g_level; }

static const char* level_str(Level l) {
    switch (l) {
        case Level::Debug: return "DBG";
        case Level::Info:  return "INF";
        case Level::Warn:  return "WRN";
        case Level::Error: return "ERR";
    }
    return "???";
}

static void log_impl(Level level, const char* tag, const char* fmt, va_list args) {
    if (level < g_level) return;

    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    std::fprintf(stderr, "[%lld][%s][%s] ", static_cast<long long>(ms), level_str(level), tag);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
    // Flush per-line so a crash doesn't lose the last few seconds of
    // logs sitting in libc's block buffer.  The cost is one extra
    // write() per log line — negligible against the ~kHz log volume.
    std::fflush(stderr);
}

void debug(const char* tag, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl(Level::Debug, tag, fmt, args);
    va_end(args);
}

void info(const char* tag, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl(Level::Info, tag, fmt, args);
    va_end(args);
}

void warn(const char* tag, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl(Level::Warn, tag, fmt, args);
    va_end(args);
}

void error(const char* tag, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl(Level::Error, tag, fmt, args);
    va_end(args);
}

} // namespace deskbeam::log
