#pragma once

namespace deskbeam::gui {

// Entry point for the GUI shell.  Sets up QApplication, the QML engine,
// the system tray, and the AppController that bridges everything.  Runs
// the event loop and returns when the user explicitly quits from the
// tray.  Closing the main window only hides it.
//
// Called from main() when no --host / --view CLI flag was passed.
int run_gui(int argc, char** argv);

} // namespace deskbeam::gui
