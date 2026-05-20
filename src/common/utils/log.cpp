#include "common/utils/log.h"
#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <ctime>

namespace vivora::log {

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

    // Wall-clock timestamp.  Local time (so the dev reading the log
    // doesn't have to translate UTC mentally) with millisecond precision
    // for ordering of fast events.  Format "YYYY-MM-DD HH:MM:SS.mmm".
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t secs = clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    char ts[32];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(ms));

    std::fprintf(stderr, "[%s][%s][%s] ", ts, level_str(level), tag);
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

} // namespace vivora::log
