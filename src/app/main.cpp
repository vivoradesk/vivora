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
    // The Windows build is a /SUBSYSTEM:WINDOWS exe (so the GUI doesn't
    // spawn an empty console).  In CLI mode we still want logs to land
    // in the PowerShell / cmd that launched us — attach to that parent
    // console and reopen the C stdio handles against it.  When launched
    // from Explorer there is no parent console; AttachConsole simply
    // fails and the CLI runs silently (which is consistent with the
    // current --host server-style use case anyway).
    if (cli_mode && AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
        std::freopen("CONIN$",  "r", stdin);
        // Log defaults to nullptr on Windows (see log.cpp); now that
        // stderr is bound to the parent console, point logs at it.
        vivora::log::use_stderr();
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
