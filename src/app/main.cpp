#include "app/legacy_cli.h"
#include "app/gui/gui_main.h"

#include <cstring>

// Entry point: branch between headless CLI and the QML GUI shell.
//
// CLI mode is selected by the presence of --host or --view in argv (the
// existing scripted / server-style use case).  Anything else — including
// no args at all — launches the GUI.  Both paths share the same
// underlying HostSession / ClientSession code; only the way they're
// driven differs.
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
    return cli_mode
        ? deskbeam::run_legacy_cli(argc, argv)
        : deskbeam::gui::run_gui(argc, argv);
}
