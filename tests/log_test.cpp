// Log sink tests: size rotation and the previous-session backup.
//
// The logger is the one module every other module calls, including from
// destructors, so its file handling is worth pinning down: an off-by-one in
// the rotation bookkeeping is a use-after-free waiting to happen, and losing
// the previous session's log breaks the most common support request.
//
// CHECK() rather than assert() on purpose — assert compiles out under
// NDEBUG, which is exactly the configuration we ship.

#include "common/utils/log.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_tests_run = 0;
int g_tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++g_tests_run;                                                         \
        if (!(expr)) {                                                         \
            std::fprintf(stderr,                                               \
                         "FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);       \
            ++g_tests_failed;                                                  \
        }                                                                      \
    } while (0)

std::string temp_dir() {
#ifdef _WIN32
    const char* t = std::getenv("TEMP");
    return t ? std::string(t) : std::string(".");
#else
    return "/tmp";
#endif
}

long file_size(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n;
}

bool file_contains(const std::string& path, const char* needle) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    std::fclose(f);
    return all.find(needle) != std::string::npos;
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

} // namespace

int main() {
    using namespace vivora;

    const std::string base   = temp_dir() + "/vivora_log_test.log";
    const std::string backup = base + ".1";
    std::remove(base.c_str());
    std::remove(backup.c_str());

    // Small cap so the test does not have to write 8 MiB, and force every
    // level through regardless of the build type's default.
    set_env("VIVORA_LOG_MAX_MB", "1");
    log::set_level(log::Level::Debug);

    // --- session 1 -------------------------------------------------------
    CHECK(log::set_file(base.c_str()));
    log::info("TEST", "session-one-marker");
    CHECK(file_contains(base, "session-one-marker"));
    CHECK(file_size(backup) < 0);   // nothing to back up on a first run

    // --- session 2: the previous log is preserved as <path>.1 ------------
    CHECK(log::set_file(base.c_str()));
    log::info("TEST", "session-two-marker");
    CHECK(file_contains(backup, "session-one-marker"));
    CHECK(file_contains(base, "session-two-marker"));
    CHECK(!file_contains(base, "session-one-marker"));

    // --- rotation on size ------------------------------------------------
    // ~1 KiB per line against a 1 MiB cap.  1300 lines trips the roll
    // exactly once, so .1 must still hold the marker written first — write
    // enough for several rolls and .1 would be an anonymous middle chunk.
    log::info("TEST", "pre-rotation-marker");
    const std::string filler(1000, 'x');
    for (int i = 0; i < 1300; ++i) log::info("TEST", "%s", filler.c_str());
    log::info("TEST", "post-rotation-marker");

    const long live = file_size(base);
    CHECK(live >= 0);
    // The live file was truncated at the roll, so it holds only what came
    // after it — a small fraction of the ~1.3 MiB we just wrote.
    CHECK(live < 1024 * 1024);
    CHECK(file_contains(base, "post-rotation-marker"));
    CHECK(file_contains(backup, "pre-rotation-marker"));
    CHECK(!file_contains(backup, "session-one-marker"));  // .1 was replaced

    // Move the sink somewhere harmless before the files go away.
    log::use_stderr();
    std::remove(base.c_str());
    std::remove(backup.c_str());

    std::printf("log_test: %d checks, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
