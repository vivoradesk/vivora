#include "common/utils/log.h"
#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <ctime>

namespace vivora::log {

static Level g_level = Level::Debug;
// Log sink — defaults to stderr but only if stderr has a real fd
// behind it.  Windows /SUBSYSTEM:WINDOWS builds launched without a
// console (the GUI .exe double-clicked from Explorer) have stderr's
// underlying handle set to INVALID_HANDLE_VALUE; fprintf to it may
// crash with STATUS_STACK_BUFFER_OVERRUN inside the CRT's UTF-8
// conversion path.  Detect that at startup and leave g_sink null
// until the GUI calls set_file() with a real path.
// Log sink — plain pointer (process exit reclaims the FD; we don't
// want the file closed mid-lifetime since other modules log from
// dtors).
//
// POSIX: defaults to stderr.  Always a real fd; safe to write to.
// Windows: starts null and the GUI shell calls set_file() during
// startup to point at a file in AppLocalDataLocation.  We can't use
// stderr by default on Windows because /SUBSYSTEM:WINDOWS binaries
// launched without a console have stderr in a degenerate state that
// triggers a CRT __fastfail (STATUS_STACK_BUFFER_OVERRUN) on the
// first fprintf.  The CLI mode in main.cpp manually calls set_file()
// (or attaches to parent console + sets stderr explicitly) before
// any logging happens.
#ifdef _WIN32
static std::FILE* g_sink = nullptr;
#else
static std::FILE* g_sink = stderr;
#endif

void set_level(Level level) { g_level = level; }
Level get_level() { return g_level; }

bool set_file(const char* path) {
    std::FILE* f = std::fopen(path, "w");
    if (!f) return false;
    // Unbuffered — every fprintf hits the OS write() directly.  log_impl
    // already fflushes per line so a crash doesn't lose history; we used
    // to ask for line-buffering via setvbuf(f, nullptr, _IOLBF, 0) but
    // MSVC's CRT rejects size=0 with __fastfail(FAST_FAIL_INVALID_ARG),
    // crashing the entire process on startup.
    std::setvbuf(f, nullptr, _IONBF, 0);
    g_sink = f;
    return true;
}

void use_stderr() { g_sink = stderr; }

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

    std::FILE* sink = g_sink;
    if (!sink) return;  // no console + no log file yet — silent
    std::fprintf(sink, "[%s][%s][%s] ", ts, level_str(level), tag);
    std::vfprintf(sink, fmt, args);
    std::fputc('\n', sink);
    // Flush per-line so a crash doesn't lose the last few seconds of
    // logs sitting in libc's block buffer.  The cost is one extra
    // write() per log line — negligible against the ~kHz log volume.
    std::fflush(sink);
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
