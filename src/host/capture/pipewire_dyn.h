#pragma once

#ifdef VIVORA_LINUX

#include <pipewire/pipewire.h>

#include <string>

// libpipewire, resolved at runtime instead of linked (VIV-126).
//
// Linking it made `libpipewire-0.3.so.0` a DT_NEEDED entry, so its absence
// stopped Vivora from starting *at all* — not "cannot share a screen", cannot
// run.  That is the wrong trade for a viewer-only machine, which never touches
// screen capture: Ubuntu 20.04 has no PipeWire in its default repositories,
// and a minimal Fedora container refused to load the 0.1 AppImage until
// pipewire-libs was installed.
//
// Bundling the library instead is not an option: it dlopens SPA plugins from
// the host and speaks a versioned protocol to the local daemon, so a copy we
// ship cannot connect.  Resolving it at runtime keeps the host path exactly as
// it was and turns the viewer-only case into a message the user can act on.
//
// Same shape as the libcuda / libnvidia-encode loading in
// nvenc_linux_encoder.cpp.
namespace vivora::host::pwdyn {

// Signatures come from the real declarations above, so this table cannot
// drift from the headers the host was built against.
struct Api {
    decltype(&::pw_init)                    init;
    decltype(&::pw_context_new)             context_new;
    decltype(&::pw_context_destroy)         context_destroy;
    decltype(&::pw_context_connect_fd)      context_connect_fd;
    decltype(&::pw_core_disconnect)         core_disconnect;
    decltype(&::pw_properties_new)          properties_new;
    decltype(&::pw_stream_new)              stream_new;
    decltype(&::pw_stream_destroy)          stream_destroy;
    decltype(&::pw_stream_add_listener)     stream_add_listener;
    decltype(&::pw_stream_connect)          stream_connect;
    decltype(&::pw_stream_disconnect)       stream_disconnect;
    decltype(&::pw_stream_dequeue_buffer)   stream_dequeue_buffer;
    decltype(&::pw_stream_queue_buffer)     stream_queue_buffer;
    decltype(&::pw_stream_set_active)       stream_set_active;
    decltype(&::pw_stream_state_as_string)  stream_state_as_string;
    decltype(&::pw_thread_loop_new)         thread_loop_new;
    decltype(&::pw_thread_loop_destroy)     thread_loop_destroy;
    decltype(&::pw_thread_loop_get_loop)    thread_loop_get_loop;
    decltype(&::pw_thread_loop_lock)        thread_loop_lock;
    decltype(&::pw_thread_loop_unlock)      thread_loop_unlock;
    decltype(&::pw_thread_loop_start)       thread_loop_start;
    decltype(&::pw_thread_loop_stop)        thread_loop_stop;
};

// Loads libpipewire on the first call and caches the result, success or
// failure.  Returns nullptr if the library is missing or too old to carry a
// symbol we need; unavailable_reason() then says which.
const Api* load();

// Empty while capture is usable; otherwise one sentence naming the package to
// install.  Safe to call without load() — it loads on demand.
std::string unavailable_reason();

// Only valid after load() returned non-null.  The capture backend checks once
// on entry, so the call sites below stay as readable as direct calls.
const Api& api();

} // namespace vivora::host::pwdyn

// The call sites keep spelling the PipeWire names.  Only functions are
// redirected — pw_stream, pw_context and friends are types and must stay
// exactly what the headers declared.
#define pw_init                    (::vivora::host::pwdyn::api().init)
#define pw_context_new             (::vivora::host::pwdyn::api().context_new)
#define pw_context_destroy         (::vivora::host::pwdyn::api().context_destroy)
#define pw_context_connect_fd      (::vivora::host::pwdyn::api().context_connect_fd)
#define pw_core_disconnect         (::vivora::host::pwdyn::api().core_disconnect)
#define pw_properties_new          (::vivora::host::pwdyn::api().properties_new)
#define pw_stream_new              (::vivora::host::pwdyn::api().stream_new)
#define pw_stream_destroy          (::vivora::host::pwdyn::api().stream_destroy)
#define pw_stream_add_listener     (::vivora::host::pwdyn::api().stream_add_listener)
#define pw_stream_connect          (::vivora::host::pwdyn::api().stream_connect)
#define pw_stream_disconnect       (::vivora::host::pwdyn::api().stream_disconnect)
#define pw_stream_dequeue_buffer   (::vivora::host::pwdyn::api().stream_dequeue_buffer)
#define pw_stream_queue_buffer     (::vivora::host::pwdyn::api().stream_queue_buffer)
#define pw_stream_set_active       (::vivora::host::pwdyn::api().stream_set_active)
#define pw_stream_state_as_string  (::vivora::host::pwdyn::api().stream_state_as_string)
#define pw_thread_loop_new         (::vivora::host::pwdyn::api().thread_loop_new)
#define pw_thread_loop_destroy     (::vivora::host::pwdyn::api().thread_loop_destroy)
#define pw_thread_loop_get_loop    (::vivora::host::pwdyn::api().thread_loop_get_loop)
#define pw_thread_loop_lock        (::vivora::host::pwdyn::api().thread_loop_lock)
#define pw_thread_loop_unlock      (::vivora::host::pwdyn::api().thread_loop_unlock)
#define pw_thread_loop_start       (::vivora::host::pwdyn::api().thread_loop_start)
#define pw_thread_loop_stop        (::vivora::host::pwdyn::api().thread_loop_stop)

#endif // VIVORA_LINUX
