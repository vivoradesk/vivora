#include "app/legacy_cli.h"
#include "common/utils/log.h"

#if defined(VIVORA_WINDOWS) || defined(VIVORA_MACOS) || defined(VIVORA_LINUX)
#include "app/gui/gui_main.h"
#define VIVORA_HAVE_GUI 1
#endif

#ifdef VIVORA_WINDOWS
#include <windows.h>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#endif

#include <cstdlib>
#include <cstring>

#ifdef VIVORA_WINDOWS
namespace {

// Point a CRT stream at the OS handle the launcher passed us, whatever it is:
// console, file or pipe.  Returns false when there is no handle to bind.
//
// A /SUBSYSTEM:WINDOWS process starts with no CRT streams even when it has
// perfectly valid std handles, so _fileno(stdout) is -2 and _dup2 has nothing
// to write onto.  Overwriting the FILE object is the standard MSVC way out of
// that, and freopen() is no help here because a pipe has no path to reopen.
bool bind_std_handle(DWORD which, FILE* stream, const char* mode) {
    const HANDLE h = GetStdHandle(which);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_TEXT);
    if (fd < 0) return false;
    FILE* f = _fdopen(fd, mode);
    if (!f) return false;
    *stream = *f;
    std::setvbuf(stream, nullptr, _IONBF, 0);
    return true;
}

} // namespace
#endif

// Entry point: branch between headless CLI and the QML GUI shell.
//
// CLI mode is selected by the presence of --host or --view in argv (the
// existing scripted / server-style / headless use case).  Anything else —
// including no args at all — launches the QML GUI on Windows, macOS and
// Linux (VIV-5).  Headless Linux hosts keep using --host and stay on the
// CLI path (no X display needed).
int main(int argc, char* argv[]) {
    bool cli_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0
         || std::strcmp(argv[i], "--view") == 0
         || std::strcmp(argv[i], "--help") == 0
         || std::strcmp(argv[i], "-h")     == 0) {
            cli_mode = true;
            break;
        }
    }
#ifdef VIVORA_WINDOWS
    // Per-monitor DPI awareness, process-wide, BEFORE any DXGI/Qt init.
    // Without it IDXGIOutput5::DuplicateOutput1 rejects the FP16 format
    // with DXGI_ERROR_UNSUPPORTED (documented quirk), silently degrading
    // an HDR desktop to the washed-out BGRA-SDR fallback in CLI --host
    // mode.  The GUI host never hit this only because QApplication sets
    // PerMonitorV2 itself; setting it here first is what Qt6 defaults to
    // anyway, so the GUI path is unchanged (VIV-84).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // The Windows build is a /SUBSYSTEM:WINDOWS exe (so the GUI doesn't
    // spawn an empty console).  In CLI mode we still want logs to land
    // in the PowerShell / cmd that launched us — attach to that parent
    // console and reopen the C stdio handles against it.  When launched
    // from Explorer there is no parent console; AttachConsole simply
    // fails and the CLI runs silently (which is consistent with the
    // current --host server-style use case anyway).
    if (cli_mode) {
        // Redirection comes first.  `vivora --help > out.txt` or a pipe hands
        // us a perfectly good stdout handle, and jumping straight to
        // CONOUT$ threw it away: the text went to the console the user was
        // trying to capture out of, and under a shell with no console at all
        // (git-bash, CI) it went nowhere.  Bind the CRT streams to whatever
        // the launcher actually gave us, and only fall back to the parent
        // console when it gave us nothing.
        bool bound = bind_std_handle(STD_OUTPUT_HANDLE, stdout, "w")
                   | bind_std_handle(STD_ERROR_HANDLE,  stderr, "w");
        if (!bound && AttachConsole(ATTACH_PARENT_PROCESS)) {
            std::freopen("CONOUT$", "w", stdout);
            std::freopen("CONOUT$", "w", stderr);
            std::freopen("CONIN$",  "r", stdin);
            bound = true;
        }
        // Log defaults to nullptr on Windows (see log.cpp); now that stderr
        // is bound to something real, point logs at it.
        if (bound) vivora::log::use_stderr();
    }
#endif
    // Optional file sink for diagnostics — invaluable for the Windows GUI host
    // which otherwise has no console.  VIVORA_LOG_FILE=path routes all logs
    // there (overrides the console sink above).
    if (const char* lf = std::getenv("VIVORA_LOG_FILE")) {
        vivora::log::set_file(lf);
    }
#ifdef VIVORA_HAVE_GUI
    return cli_mode
        ? vivora::run_legacy_cli(argc, argv)
        : vivora::gui::run_gui(argc, argv);
#else
    return vivora::run_legacy_cli(argc, argv);
#endif
}
