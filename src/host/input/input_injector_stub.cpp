// Linux input injector stub.  Host mode on Linux is L3 territory; for L2
// (Linux client only) we just need the symbol to exist so the cross-platform
// HostSession links.  Returning nullptr makes any accidental --host on
// Linux fail the platform-init check; main.cpp also short-circuits the
// host path with a clear error message before getting here.

#if defined(VIVORA_LINUX)

#include "host/input/input_injector.h"

namespace vivora::host {

std::unique_ptr<InputInjector> InputInjector::create() {
    return nullptr;
}

} // namespace vivora::host

#endif
