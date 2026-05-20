#include "app/legacy_cli.h"

#ifdef VIVORA_WINDOWS
#include "app/gui/gui_main.h"
#endif

#include <cstring>

// Entry point: branch between headless CLI and the QML GUI shell.
//
// CLI mode is selected by the presence of --host or --view in argv (the
// existing scripted / server-style use case).  Anything else — including
// no args at all — launches the GUI on Windows.  On Linux / macOS where
// the GUI shell hasn't landed yet, no-arg falls back to printing CLI
// help so the binary stays useful.
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
    return cli_mode
        ? vivora::run_legacy_cli(argc, argv)
        : vivora::gui::run_gui(argc, argv);
#else
    return vivora::run_legacy_cli(argc, argv);
#endif
}
