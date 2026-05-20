#pragma once

namespace vivora {

// Parse the legacy `--host` / `--view` flag form and run the matching
// host_loop / view_loop until exit.  Returns the process exit code.
// Called from main() when --host or --view is present in argv; otherwise
// main() launches the GUI shell instead.
int run_legacy_cli(int argc, char** argv);

} // namespace vivora
