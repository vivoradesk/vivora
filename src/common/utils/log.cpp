#include "common/utils/log.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <ctime>
#include <mutex>
#include <string>

namespace vivora::log {

// Default verbosity.  Debug in developer builds, Info in shipped ones:
// a Debug-by-default release writes megabytes of per-frame chatter into
// the user's log file for no benefit, and the interesting lines drown.
// VIVORA_LOG_LEVEL=debug|info|warn|error overrides either way, so support
// can ask for a verbose run without shipping a special build.
static Level default_level() {
#ifdef NDEBUG
    Level lvl = Level::Info;
#else
    Level lvl = Level::Debug;
#endif
    const char* env = std::getenv("VIVORA_LOG_LEVEL");
    if (env) {
        if      (std::strcmp(env, "debug") == 0) lvl = Level::Debug;
        else if (std::strcmp(env, "info")  == 0) lvl = Level::Info;
        else if (std::strcmp(env, "warn")  == 0) lvl = Level::Warn;
        else if (std::strcmp(env, "error") == 0) lvl = Level::Error;
    }
    return lvl;
}

static Level g_level = default_level();
// Log sink — defaults to stderr but only if stderr has a real fd
// behind it.  Windows /SUBSYSTEM:WINDOWS builds launched without a
// console (the GUI .exe double-clicked from Explorer) have stderr's
// underlying handle set to INVALID_HANDLE_VALUE; fprintf to it may
// crash with STATUS_STACK_BUFFER_OVERRUN inside the CRT's UTF-8
// conversion path.  Detect that at startup and leave g_sink null
// until the GUI calls set_file() with a real path.
// Log sink.  Never closed on the normal path (process exit reclaims the
// FD; other modules still log from dtors) — the one exception is size
// rotation, which is why access is serialised by g_mu below.
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

// Guards g_sink and the rotation bookkeeping below.  Rotation has to
// fclose() the sink, so every reader of g_sink must hold this — otherwise
// a thread logging from a dtor can write through a closed FILE*.  One
// uncontended mutex per line is free next to the fflush we already do.
static std::mutex  g_mu;
static std::string g_path;        // empty when the sink is stderr
static long long   g_written = 0; // bytes written to the current file

// Size cap for the on-disk log.  A remote-desktop session logs steadily
// for hours; without a cap a long-lived tray process grows an unbounded
// file in the user's profile.  On overflow we keep exactly one previous
// generation (<path>.1), so worst case on disk is 2x the cap.
static long long max_bytes() {
    long long mb = 8;
    if (const char* env = std::getenv("VIVORA_LOG_MAX_MB")) {
        const long long v = std::atoll(env);
        if (v > 0) mb = v;
    }
    return mb * 1024 * 1024;
}

void set_level(Level level) { g_level = level; }
Level get_level() { return g_level; }

// Caller must hold g_mu.
static std::FILE* open_log_file(const char* path) {
    std::FILE* f = std::fopen(path, "w");
    if (!f) return nullptr;
    // Unbuffered — every fprintf hits the OS write() directly.  log_impl
    // already fflushes per line so a crash doesn't lose history; we used
    // to ask for line-buffering via setvbuf(f, nullptr, _IOLBF, 0) but
    // MSVC's CRT rejects size=0 with __fastfail(FAST_FAIL_INVALID_ARG),
    // crashing the entire process on startup.
    std::setvbuf(f, nullptr, _IONBF, 0);
    return f;
}

// Caller must hold g_mu.  Closes the current file, moves it aside as
// <path>.1 and reopens an empty one.  On any failure we keep writing to
// the freshly opened file if we got one, else the sink goes quiet rather
// than dangling — losing logs beats a use-after-free.
static void rotate_locked() {
    if (g_path.empty() || !g_sink) return;
    const std::string prev = g_path + ".1";
    std::fclose(g_sink);
    g_sink = nullptr;
    std::remove(prev.c_str());
    std::rename(g_path.c_str(), prev.c_str());
    g_sink = open_log_file(g_path.c_str());
    g_written = 0;
}

bool set_file(const char* path) {
    std::lock_guard<std::mutex> lk(g_mu);

    // Release any file we already hold first.  Windows refuses to rename a
    // file that is still open, so the backup below silently did nothing if
    // an earlier set_file() left a handle on the same path.
    if (!g_path.empty() && g_sink) {
        std::fclose(g_sink);
        g_sink = nullptr;
        g_path.clear();
    }

    // Keep the previous session's log as <path>.1 before truncating.  The
    // common support request is "it misbehaved, send me the log", and by
    // the time the user gets around to it they have usually restarted the
    // app — which used to overwrite the only evidence.
    {
        std::FILE* prev = std::fopen(path, "rb");
        if (prev) {
            std::fseek(prev, 0, SEEK_END);
            const long sz = std::ftell(prev);
            std::fclose(prev);
            if (sz > 0) {
                const std::string backup = std::string(path) + ".1";
                std::remove(backup.c_str());
                std::rename(path, backup.c_str());
            }
        }
    }

    std::FILE* f = open_log_file(path);
    if (!f) return false;   // sink stays null rather than dangling
    g_sink = f;
    g_path = path;
    g_written = 0;
    return true;
}

void use_stderr() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_sink = stderr;
    g_path.clear();
    g_written = 0;
}

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

    std::lock_guard<std::mutex> lk(g_mu);
    std::FILE* sink = g_sink;
    if (!sink) return;  // no console + no log file yet — silent
    int n = std::fprintf(sink, "[%s][%s][%s] ", ts, level_str(level), tag);
    const int m = std::vfprintf(sink, fmt, args);
    std::fputc('\n', sink);
    // Flush per-line so a crash doesn't lose the last few seconds of
    // logs sitting in libc's block buffer.  The cost is one extra
    // write() per log line — negligible against the ~kHz log volume.
    std::fflush(sink);

    // Roll the file over once it outgrows the cap.  Only ever true for a
    // real file sink; g_path is empty when we are writing to stderr.
    if (!g_path.empty()) {
        if (n < 0) n = 0;
        g_written += n + (m > 0 ? m : 0) + 1;
        if (g_written >= max_bytes()) rotate_locked();
    }
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
