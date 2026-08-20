#ifdef VIVORA_LINUX

#include "host/capture/pipewire_dyn.h"
#include "common/utils/log.h"

#include <dlfcn.h>

#include <mutex>

namespace vivora::host::pwdyn {

namespace {

constexpr const char* TAG = "PWDYN";

struct Loaded {
    void*       handle = nullptr;
    Api         api{};
    bool        ok = false;
    std::string error;
};

Loaded& state() {
    static Loaded s;
    static std::once_flag once;
    std::call_once(once, [] {
        // The versioned soname first: it is what every distribution ships and
        // what the linker would have recorded.  The bare name only exists
        // where a -dev package is installed.
        s.handle = ::dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!s.handle) s.handle = ::dlopen("libpipewire-0.3.so", RTLD_NOW | RTLD_LOCAL);
        if (!s.handle) {
            s.error = "libpipewire-0.3 is not installed";
            const char* dl = ::dlerror();
            log::info(TAG, "libpipewire not available: %s", dl ? dl : "not found");
            return;
        }

        const char* missing = nullptr;
        auto sym = [&](const char* name) -> void* {
            void* p = ::dlsym(s.handle, name);
            if (!p && !missing) missing = name;
            return p;
        };

        // reinterpret_cast rather than a C cast so a signature that stops
        // matching the header is a compile error here, not a crash later.
#define VIVORA_PW_SYM(field, name)                                             \
    s.api.field = reinterpret_cast<decltype(s.api.field)>(sym(name))

        VIVORA_PW_SYM(init,                   "pw_init");
        VIVORA_PW_SYM(context_new,            "pw_context_new");
        VIVORA_PW_SYM(context_destroy,        "pw_context_destroy");
        VIVORA_PW_SYM(context_connect_fd,     "pw_context_connect_fd");
        VIVORA_PW_SYM(core_disconnect,        "pw_core_disconnect");
        VIVORA_PW_SYM(properties_new,         "pw_properties_new");
        VIVORA_PW_SYM(stream_new,             "pw_stream_new");
        VIVORA_PW_SYM(stream_destroy,         "pw_stream_destroy");
        VIVORA_PW_SYM(stream_add_listener,    "pw_stream_add_listener");
        VIVORA_PW_SYM(stream_connect,         "pw_stream_connect");
        VIVORA_PW_SYM(stream_disconnect,      "pw_stream_disconnect");
        VIVORA_PW_SYM(stream_dequeue_buffer,  "pw_stream_dequeue_buffer");
        VIVORA_PW_SYM(stream_queue_buffer,    "pw_stream_queue_buffer");
        VIVORA_PW_SYM(stream_set_active,      "pw_stream_set_active");
        VIVORA_PW_SYM(stream_state_as_string, "pw_stream_state_as_string");
        VIVORA_PW_SYM(thread_loop_new,        "pw_thread_loop_new");
        VIVORA_PW_SYM(thread_loop_destroy,    "pw_thread_loop_destroy");
        VIVORA_PW_SYM(thread_loop_get_loop,   "pw_thread_loop_get_loop");
        VIVORA_PW_SYM(thread_loop_lock,       "pw_thread_loop_lock");
        VIVORA_PW_SYM(thread_loop_unlock,     "pw_thread_loop_unlock");
        VIVORA_PW_SYM(thread_loop_start,      "pw_thread_loop_start");
        VIVORA_PW_SYM(thread_loop_stop,       "pw_thread_loop_stop");

#undef VIVORA_PW_SYM

        if (missing) {
            // Every one of these has been in libpipewire since 0.3.0, so this
            // means something other than PipeWire answered to the name.
            s.error = std::string("libpipewire-0.3 is missing ") + missing;
            log::warn(TAG, "libpipewire loaded but %s is missing", missing);
            ::dlclose(s.handle);
            s.handle = nullptr;
            return;
        }
        s.ok = true;
        log::info(TAG, "libpipewire loaded");
    });
    return s;
}

} // namespace

const Api* load() {
    Loaded& s = state();
    return s.ok ? &s.api : nullptr;
}

const Api& api() { return state().api; }

std::string unavailable_reason() {
    Loaded& s = state();
    if (s.ok) return {};
    return "Screen sharing is unavailable: " + s.error +
           ". Install it with your package manager (Debian/Ubuntu: "
           "libpipewire-0.3-0, Fedora: pipewire-libs, Arch: libpipewire) and "
           "start Vivora again. Connecting to other machines works without it.";
}

} // namespace vivora::host::pwdyn

#endif // VIVORA_LINUX
