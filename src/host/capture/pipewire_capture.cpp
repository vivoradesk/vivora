#ifdef VIVORA_LINUX

#include "host/capture/pipewire_capture.h"
#include "common/utils/log.h"

#include <dbus/dbus.h>

// Resolves libpipewire at runtime and redirects the pw_* calls below; see
// pipewire_dyn.h for why this is not a plain link (VIV-126).
#include "host/capture/pipewire_dyn.h"
#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>
#include <spa/utils/result.h>
#include <spa/pod/builder.h>
#include <spa/param/format.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace vivora::host {

std::string PipeWireCapture::unavailable_reason() {
    return pwdyn::unavailable_reason();
}

namespace {

constexpr const char* TAG = "PWCAP";

// Generate a random 8-byte hex token used as a per-request handle suffix
// (xdg-desktop-portal asks the caller to predict the Request object path
// so it can listen for the Response signal before issuing the call).
std::string random_token() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016lx", static_cast<unsigned long>(gen()));
    return std::string(buf);
}

// Convert an org.freedesktop.DBus.Bus name like "1.234" into the
// underscore-escaped form "1_234" that portal Request objects use.
std::string bus_name_to_path_suffix(const std::string& name) {
    std::string out = name;
    if (!out.empty() && out[0] == ':') out.erase(0, 1);
    for (auto& c : out) if (c == '.') c = '_';
    return out;
}

// Path of the saved portal restore_token.  The portal returns this in
// the Start response when persist_mode>=1 was requested in SelectSources;
// passing it back in the next SelectSources call skips the user dialog.
std::string restore_token_path() {
    std::string base;
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg) base = xdg;
    else if (const char* home = std::getenv("HOME"); home && *home) base = std::string(home) + "/.local/state";
    else return {};
    std::string dir = base + "/vivora";
    ::mkdir(base.c_str(), 0700);
    ::mkdir(dir.c_str(),  0700);
    return dir + "/portal_restore_token";
}

std::string load_restore_token() {
    auto p = restore_token_path();
    if (p.empty()) return {};
    std::ifstream f(p);
    if (!f) return {};
    std::string token;
    std::getline(f, token);
    return token;
}

void save_restore_token(const std::string& token) {
    auto p = restore_token_path();
    if (p.empty() || token.empty()) return;
    std::ofstream f(p, std::ios::trunc);
    if (f) f << token;
}

} // namespace

// ────────────────────────────────────────────────────────────────────
// Impl: holds DBus + PipeWire state.  Keeping it out of the header so
// the public API doesn't drag in libpipewire / libdbus headers.
// ────────────────────────────────────────────────────────────────────
struct PipeWireCapture::Impl {
    // ---- DBus ----
    DBusConnection* bus = nullptr;
    std::string sender_path_suffix;          // "1_234" form of unique name
    std::string session_handle;              // returned by CreateSession
    uint32_t    pipewire_node_id = 0;        // returned by Start
    int         pipewire_fd = -1;            // returned by OpenPipeWireRemote

    // ---- PipeWire ----
    pw_thread_loop* loop = nullptr;
    pw_context*     ctx = nullptr;
    pw_core*        core = nullptr;
    pw_stream*      stream = nullptr;
    spa_hook        stream_listener{};

    // ---- Frame state, accessed by static C callbacks ----
    PipeWireCapture::FrameCallback cb;
    // Stream geometry (filled when stream's param-changed fires).
    uint32_t neg_w = 0, neg_h = 0, neg_fmt = 0, neg_stride = 0;

    // Portal cursor_mode for SelectSources: true → EMBEDDED (2, portal draws
    // the cursor into the frame), false → HIDDEN (1, host paints it from X11).
    bool cursor_embedded = true;

    // Cursor metadata (filled by on_pw_process from SPA_META_Cursor).
    // Locked by cursor_mu so the host_loop main thread can read snapshots
    // independently of the PipeWire callback thread.
    mutable std::mutex            cursor_mu;
    PipeWireCapture::CursorState  cursor_state{};
    bool                          cursor_ever_seen = false;
    PipeWireCapture::CursorShape  pending_cursor_shape{};
    bool                          new_shape_pending = false;
    uint32_t                      next_shape_id = 1;
    uint64_t                      last_shape_hash = 0;

    ~Impl() {
        if (stream) {
            pw_stream_disconnect(stream);
            pw_stream_destroy(stream);
        }
        if (core) pw_core_disconnect(core);
        if (ctx)  pw_context_destroy(ctx);
        if (loop) pw_thread_loop_destroy(loop);
        if (pipewire_fd >= 0) ::close(pipewire_fd);
        if (bus) {
            dbus_connection_close(bus);
            dbus_connection_unref(bus);
        }
    }
};

PipeWireCapture::PipeWireCapture() : impl_(std::make_unique<Impl>()) {}
PipeWireCapture::~PipeWireCapture() = default;

uint32_t PipeWireCapture::width()  const { return impl_->neg_w; }
uint32_t PipeWireCapture::height() const { return impl_->neg_h; }

void PipeWireCapture::set_cursor_embedded(bool embedded) {
    impl_->cursor_embedded = embedded;
}

bool PipeWireCapture::has_cursor() const {
    std::lock_guard<std::mutex> lk(impl_->cursor_mu);
    return impl_->cursor_ever_seen;
}

PipeWireCapture::CursorState PipeWireCapture::cursor_state() const {
    std::lock_guard<std::mutex> lk(impl_->cursor_mu);
    return impl_->cursor_state;
}

bool PipeWireCapture::take_new_cursor_shape(CursorShape& out) {
    std::lock_guard<std::mutex> lk(impl_->cursor_mu);
    if (!impl_->new_shape_pending) return false;
    out = std::move(impl_->pending_cursor_shape);
    impl_->pending_cursor_shape = {};
    impl_->new_shape_pending = false;
    return true;
}

// ────────────────────────────────────────────────────────────────────
// Portal request helper: dispatch one DBus method call, then pump the
// connection until the matching Response signal lands on the predicted
// Request object path.  Returns the response variant (a{sv} dictionary).
//
// The portal pattern:
//   1. Caller picks a random handle_token, predicts the Request path:
//      /org/freedesktop/portal/desktop/request/<sender_suffix>/<token>
//   2. Caller adds a match rule + listens for "Response" signal there.
//   3. Caller invokes the method (CreateSession / SelectSources / Start /
//      OpenPipeWireRemote) passing handle_token in options.
//   4. Portal eventually emits the Response signal carrying status + dict.
// ────────────────────────────────────────────────────────────────────

namespace {

bool dbus_pump_until_response(DBusConnection* conn,
                              const std::string& request_path,
                              uint32_t* out_status,
                              DBusMessage** out_response /* may be nullptr */)
{
    // Block on the connection up to ~5min waiting for a signal at
    // request_path.  Generous because the Start request raises a system
    // dialog the user has to confirm — we don't want to bail just because
    // they were tabbed out for a bit.
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now() + std::chrono::minutes(5);
    while (Clock::now() < deadline) {
        dbus_connection_read_write_dispatch(conn, 100);
        DBusMessage* msg;
        while ((msg = dbus_connection_pop_message(conn))) {
            const char* iface = dbus_message_get_interface(msg);
            const char* path  = dbus_message_get_path(msg);
            if (iface && path
                && std::strcmp(iface, "org.freedesktop.portal.Request") == 0
                && request_path == path)
            {
                // Response signal: u (status) a{sv} (results).
                DBusMessageIter it;
                if (dbus_message_iter_init(msg, &it)
                    && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32)
                {
                    dbus_uint32_t status = 0;
                    dbus_message_iter_get_basic(&it, &status);
                    if (out_status) *out_status = status;
                    if (out_response) {
                        *out_response = msg;  // caller unrefs
                    } else {
                        dbus_message_unref(msg);
                    }
                    return true;
                }
                dbus_message_unref(msg);
                return false;
            }
            dbus_message_unref(msg);
        }
    }
    log::error(TAG, "Portal Response timeout for %s", request_path.c_str());
    return false;
}

// Append (key=value) to a DBus a{sv} dict iter — common pattern in
// portal calls where every argument lives inside an options dict.
void dict_append_str(DBusMessageIter* dict, const char* key, const char* value) {
    DBusMessageIter entry, var;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &value);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(dict, &entry);
}

void dict_append_u32(DBusMessageIter* dict, const char* key, uint32_t value) {
    DBusMessageIter entry, var;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &value);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(dict, &entry);
}

// Walk a Response's results dict (a{sv}) looking for a string-typed key.
bool extract_str(DBusMessage* msg, const char* wanted_key, std::string& out) {
    DBusMessageIter it;
    if (!dbus_message_iter_init(msg, &it)) return false;
    dbus_message_iter_next(&it);  // skip status uint32
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return false;
    DBusMessageIter dict;
    dbus_message_iter_recurse(&it, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, var;
        dbus_message_iter_recurse(&dict, &entry);
        const char* key = nullptr;
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &var);
        if (key && std::strcmp(key, wanted_key) == 0
            && dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING)
        {
            const char* val = nullptr;
            dbus_message_iter_get_basic(&var, &val);
            if (val) { out = val; return true; }
        }
        dbus_message_iter_next(&dict);
    }
    return false;
}

// Pull the first stream's PipeWire node-ID from the Start response.
// Format: a(ua{sv})  — array of (node_id, props).  We pick streams[0].
bool extract_first_stream_node(DBusMessage* msg, uint32_t& node_id_out) {
    DBusMessageIter it;
    if (!dbus_message_iter_init(msg, &it)) return false;
    dbus_message_iter_next(&it);  // skip status
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return false;
    DBusMessageIter results_dict;
    dbus_message_iter_recurse(&it, &results_dict);
    while (dbus_message_iter_get_arg_type(&results_dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, var;
        dbus_message_iter_recurse(&results_dict, &entry);
        const char* key = nullptr;
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &var);
        if (key && std::strcmp(key, "streams") == 0
            && dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY)
        {
            DBusMessageIter streams_arr, stream_struct;
            dbus_message_iter_recurse(&var, &streams_arr);
            if (dbus_message_iter_get_arg_type(&streams_arr) != DBUS_TYPE_STRUCT)
                return false;
            dbus_message_iter_recurse(&streams_arr, &stream_struct);
            if (dbus_message_iter_get_arg_type(&stream_struct) != DBUS_TYPE_UINT32)
                return false;
            dbus_uint32_t nid = 0;
            dbus_message_iter_get_basic(&stream_struct, &nid);
            node_id_out = nid;
            return true;
        }
        dbus_message_iter_next(&results_dict);
    }
    return false;
}

} // namespace

// ────────────────────────────────────────────────────────────────────
// PipeWire stream callbacks: param_changed (latched format) + process
// (frame arrived).  Stage 1 just logs.
// ────────────────────────────────────────────────────────────────────

static void on_pw_state_changed(void* userdata, pw_stream_state old_state,
                                pw_stream_state new_state, const char* error)
{
    (void)userdata; (void)old_state;
    log::info(TAG, "PipeWire stream state: %s%s%s",
              pw_stream_state_as_string(new_state),
              error ? " — error: " : "",
              error ? error : "");
}

static void on_pw_param_changed(void* userdata, uint32_t id, const struct spa_pod* param)
{
    log::info(TAG, "param_changed id=%u param=%s",
              id, param ? "yes" : "null");
    auto* impl = static_cast<PipeWireCapture::Impl*>(userdata);
    if (!param || id != SPA_PARAM_Format) return;

    spa_video_info info{};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) return;
    if (info.media_type != SPA_MEDIA_TYPE_video) return;
    if (info.media_subtype != SPA_MEDIA_SUBTYPE_raw) return;
    if (spa_format_video_raw_parse(param, &info.info.raw) < 0) return;

    impl->neg_w      = info.info.raw.size.width;
    impl->neg_h      = info.info.raw.size.height;
    impl->neg_fmt    = info.info.raw.format;

    log::info(TAG, "Negotiated format: %ux%u fmt=%s",
              impl->neg_w, impl->neg_h,
              spa_debug_type_find_short_name(spa_type_video_format, impl->neg_fmt));
}

static void on_pw_process(void* userdata)
{
    auto* impl = static_cast<PipeWireCapture::Impl*>(userdata);
    pw_buffer* b = pw_stream_dequeue_buffer(impl->stream);
    if (!b) return;

    spa_buffer* sb = b->buffer;
    PipeWireCapture::Frame f{};
    f.width  = impl->neg_w;
    f.height = impl->neg_h;
    f.format = impl->neg_fmt;
    f.pts_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    if (sb->n_datas > 0) {
        const spa_data& d = sb->datas[0];
        if (d.type == SPA_DATA_DmaBuf) {
            f.dmabuf_fd = static_cast<int>(d.fd);
        } else if (d.type == SPA_DATA_MemPtr || d.type == SPA_DATA_MemFd) {
            f.stride = d.chunk ? d.chunk->stride : 0;
            f.data   = static_cast<const uint8_t*>(d.data);
            f.size   = d.chunk ? d.chunk->size : 0;
        }
    }

    // Cursor metadata (cursor_mode=METADATA path).  PipeWire packs the
    // current cursor position + (when shape changed) bitmap into a
    // SPA_META_Cursor block on each buffer.  Position+visibility is
    // copied every frame; shape bitmap only when a new id arrives AND
    // its content hash differs from the previous one.
    spa_meta_cursor* cmeta = static_cast<spa_meta_cursor*>(
        spa_buffer_find_meta_data(sb, SPA_META_Cursor, sizeof(*cmeta)));
    if (cmeta && spa_meta_cursor_is_valid(cmeta)) {
        std::lock_guard<std::mutex> lk(impl->cursor_mu);
        const float cap_w = impl->neg_w > 0 ? float(impl->neg_w) : 1.0f;
        const float cap_h = impl->neg_h > 0 ? float(impl->neg_h) : 1.0f;
        impl->cursor_state.x_norm  = float(cmeta->position.x) / cap_w;
        impl->cursor_state.y_norm  = float(cmeta->position.y) / cap_h;
        impl->cursor_state.visible = true;
        impl->cursor_ever_seen     = true;

        spa_meta_bitmap* bmap = (cmeta->bitmap_offset > 0)
            ? SPA_PTROFF(cmeta, cmeta->bitmap_offset, spa_meta_bitmap)
            : nullptr;
        if (bmap && bmap->size.width > 0 && bmap->size.height > 0
            && bmap->offset > 0)
        {
            const uint8_t* pixels = SPA_PTROFF(bmap, bmap->offset, const uint8_t);
            const size_t w = bmap->size.width;
            const size_t h = bmap->size.height;
            const size_t row_bytes = bmap->stride > 0 ? size_t(bmap->stride) : w * 4;
            // FNV-1a over the first row + dims + hotspot — cheap dedup
            // so we don't re-send the same arrow bitmap every frame.
            uint64_t hash = 14695981039346656037ULL;
            auto mix = [&hash](const void* p, size_t n) {
                const uint8_t* b = static_cast<const uint8_t*>(p);
                for (size_t i = 0; i < n; ++i) { hash ^= b[i]; hash *= 1099511628211ULL; }
            };
            mix(&w, sizeof(w)); mix(&h, sizeof(h));
            mix(&cmeta->hotspot.x, sizeof(cmeta->hotspot.x));
            mix(&cmeta->hotspot.y, sizeof(cmeta->hotspot.y));
            mix(pixels, std::min<size_t>(row_bytes, 256));

            if (hash != impl->last_shape_hash) {
                impl->last_shape_hash = hash;
                impl->cursor_state.shape_id = impl->next_shape_id++;
                impl->pending_cursor_shape.id        = impl->cursor_state.shape_id;
                impl->pending_cursor_shape.width     = static_cast<uint16_t>(w);
                impl->pending_cursor_shape.height    = static_cast<uint16_t>(h);
                impl->pending_cursor_shape.hotspot_x = static_cast<uint16_t>(cmeta->hotspot.x);
                impl->pending_cursor_shape.hotspot_y = static_cast<uint16_t>(cmeta->hotspot.y);
                impl->pending_cursor_shape.bgra.resize(w * h * 4);
                for (size_t row = 0; row < h; ++row) {
                    std::memcpy(impl->pending_cursor_shape.bgra.data() + row * w * 4,
                                pixels + row * row_bytes, w * 4);
                }
                impl->new_shape_pending = true;
            }
        }
    }

    if (impl->cb) impl->cb(f);
    pw_stream_queue_buffer(impl->stream, b);
}

static const pw_stream_events kStreamEvents = {
    .version       = PW_VERSION_STREAM_EVENTS,
    .destroy       = nullptr,
    .state_changed = on_pw_state_changed,
    .control_info  = nullptr,
    .io_changed    = nullptr,
    .param_changed = on_pw_param_changed,
    .add_buffer    = nullptr,
    .remove_buffer = nullptr,
    .process       = on_pw_process,
    .drained       = nullptr,
    .command       = nullptr,
    .trigger_done  = nullptr,
};

// ────────────────────────────────────────────────────────────────────
// init(): open DBus session bus, drive the portal Create/Select/Start/
// OpenPipeWireRemote chain, then bring up the PipeWire client.
// ────────────────────────────────────────────────────────────────────

bool PipeWireCapture::init(FrameCallback cb) {
    // Before the portal flow, not after it: libpipewire is resolved at
    // runtime (VIV-126), and a machine without it should be told so instead
    // of being walked through a screen-capture permission dialog for a
    // session nothing can consume.
    if (!pwdyn::load()) {
        log::error(TAG, "%s", pwdyn::unavailable_reason().c_str());
        return false;
    }

    impl_->cb = std::move(cb);

    DBusError err;
    dbus_error_init(&err);
    impl_->bus = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!impl_->bus) {
        log::error(TAG, "DBus connect failed: %s", err.message);
        dbus_error_free(&err);
        return false;
    }
    const char* unique = dbus_bus_get_unique_name(impl_->bus);
    if (!unique) { log::error(TAG, "DBus unique name not available"); return false; }
    impl_->sender_path_suffix = bus_name_to_path_suffix(unique);

    // Subscribe to all Response signals from the portal — we filter by
    // the request_path inside dbus_pump_until_response().
    dbus_bus_add_match(impl_->bus,
        "type='signal',interface='org.freedesktop.portal.Request'",
        &err);
    dbus_connection_flush(impl_->bus);

    // ── 1. CreateSession ──
    {
        const std::string handle_token = random_token();
        const std::string session_token = random_token();
        const std::string request_path =
            "/org/freedesktop/portal/desktop/request/" + impl_->sender_path_suffix + "/" + handle_token;

        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "CreateSession");
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(msg, &args);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dict_append_str(&dict, "handle_token", handle_token.c_str());
        dict_append_str(&dict, "session_handle_token", session_token.c_str());
        dbus_message_iter_close_container(&args, &dict);

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(impl_->bus, msg, 5000, &err);
        dbus_message_unref(msg);
        if (!reply) {
            log::error(TAG, "CreateSession call failed: %s", err.message);
            dbus_error_free(&err);
            return false;
        }
        dbus_message_unref(reply);

        uint32_t status = 0;
        DBusMessage* response = nullptr;
        if (!dbus_pump_until_response(impl_->bus, request_path, &status, &response)) return false;
        if (status != 0) {
            log::error(TAG, "CreateSession Response status=%u (denied?)", status);
            if (response) dbus_message_unref(response);
            return false;
        }
        std::string sh;
        if (!extract_str(response, "session_handle", sh)) {
            log::error(TAG, "CreateSession Response missing session_handle");
            dbus_message_unref(response);
            return false;
        }
        dbus_message_unref(response);
        impl_->session_handle = sh;
        log::info(TAG, "Portal session: %s", impl_->session_handle.c_str());
    }

    // ── 2. SelectSources ──
    {
        const std::string handle_token = random_token();
        const std::string request_path =
            "/org/freedesktop/portal/desktop/request/" + impl_->sender_path_suffix + "/" + handle_token;

        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "SelectSources");
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(msg, &args);
        const char* sh_c = impl_->session_handle.c_str();
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh_c);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dict_append_str(&dict, "handle_token", handle_token.c_str());
        dict_append_u32(&dict, "types", 1);          // 1 = monitor (whole screen)
        // cursor_mode 2 = EMBEDDED: portal draws the cursor straight into
        // the captured pixels.  The "right" architecture is METADATA (4),
        // which would let the client paint its own cursor at OS speed —
        // but xdg-desktop-portal < 1.18 (Ubuntu 22.04 ships older) silently
        // ignores METADATA and our SPA_META_Cursor blocks come back empty.
        // EMBEDDED is the lowest-friction fallback that "just works":
        // single visible cursor on the client (since stream_window
        // defaults to BlankCursor over the stream area), at the cost of
        // one round-trip of perceived cursor-move latency.
        //
        // When the host can paint the cursor itself from X11 (X11Cursor,
        // VIV-66) we instead request HIDDEN (1) so the portal leaves the
        // cursor out of the frame — otherwise EMBEDDED-that-doesn't-composite
        // plus the X11 overlay would fight / double up.
        dict_append_u32(&dict, "cursor_mode", impl_->cursor_embedded ? 2u : 1u);
        // Ask the portal to remember this grant across restarts and hand
        // back a restore_token in the Start response.  Replaying that
        // token on the next SelectSources skips the permission dialog.
        dict_append_u32(&dict, "persist_mode", 2);
        std::string saved_token = load_restore_token();
        if (!saved_token.empty()) {
            dict_append_str(&dict, "restore_token", saved_token.c_str());
        }
        dbus_message_iter_close_container(&args, &dict);

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(impl_->bus, msg, 5000, &err);
        dbus_message_unref(msg);
        if (!reply) {
            log::error(TAG, "SelectSources call failed: %s", err.message);
            dbus_error_free(&err);
            return false;
        }
        dbus_message_unref(reply);

        uint32_t status = 0;
        if (!dbus_pump_until_response(impl_->bus, request_path, &status, nullptr)) return false;
        if (status != 0) {
            log::error(TAG, "SelectSources Response status=%u", status);
            return false;
        }
    }

    // ── 3. Start (user permission dialog appears here on first run) ──
    {
        const std::string handle_token = random_token();
        const std::string request_path =
            "/org/freedesktop/portal/desktop/request/" + impl_->sender_path_suffix + "/" + handle_token;

        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "Start");
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(msg, &args);
        const char* sh_c = impl_->session_handle.c_str();
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh_c);
        const char* parent = "";  // no parent window — Vivora is headless on host
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dict_append_str(&dict, "handle_token", handle_token.c_str());
        dbus_message_iter_close_container(&args, &dict);

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(impl_->bus, msg, 5000, &err);
        dbus_message_unref(msg);
        if (!reply) {
            log::error(TAG, "Start call failed: %s", err.message);
            dbus_error_free(&err);
            return false;
        }
        dbus_message_unref(reply);

        log::info(TAG, "Waiting for user to grant screen capture permission…");
        uint32_t status = 0;
        DBusMessage* response = nullptr;
        // User dialog can stay up indefinitely — this dispatch loop has a
        // 30s deadline.  Bump if you expect long approvals.
        if (!dbus_pump_until_response(impl_->bus, request_path, &status, &response)) return false;
        if (status != 0) {
            log::error(TAG, "Start Response status=%u (user denied?)", status);
            if (response) dbus_message_unref(response);
            return false;
        }
        if (!extract_first_stream_node(response, impl_->pipewire_node_id)) {
            log::error(TAG, "Start Response missing streams[0] node id");
            dbus_message_unref(response);
            return false;
        }
        // The portal places the new restore_token in the Start results
        // dict (one of the few places — not in SelectSources response).
        std::string new_token;
        if (extract_str(response, "restore_token", new_token) && !new_token.empty()) {
            save_restore_token(new_token);
            log::info(TAG, "Saved restore_token (skip-dialog on next launch)");
        }
        dbus_message_unref(response);
        log::info(TAG, "Granted PipeWire node id=%u", impl_->pipewire_node_id);
    }

    // ── 4. OpenPipeWireRemote — synchronous reply (UnixFD) ──
    {
        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "OpenPipeWireRemote");
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(msg, &args);
        const char* sh_c = impl_->session_handle.c_str();
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh_c);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dbus_message_iter_close_container(&args, &dict);

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(impl_->bus, msg, 5000, &err);
        dbus_message_unref(msg);
        if (!reply) {
            log::error(TAG, "OpenPipeWireRemote failed: %s", err.message);
            dbus_error_free(&err);
            return false;
        }
        DBusMessageIter rit;
        if (!dbus_message_iter_init(reply, &rit)
            || dbus_message_iter_get_arg_type(&rit) != DBUS_TYPE_UNIX_FD)
        {
            log::error(TAG, "OpenPipeWireRemote: unexpected reply signature");
            dbus_message_unref(reply);
            return false;
        }
        int fd = -1;
        dbus_message_iter_get_basic(&rit, &fd);
        // dbus dups the fd into the reply; we own it now and must close.
        impl_->pipewire_fd = fd;
        dbus_message_unref(reply);
        log::info(TAG, "PipeWire remote fd=%d", impl_->pipewire_fd);
    }

    // ── 5. PipeWire client setup ──
    pw_init(nullptr, nullptr);
    impl_->loop = pw_thread_loop_new("vivora-capture", nullptr);
    if (!impl_->loop) { log::error(TAG, "pw_thread_loop_new failed"); return false; }

    pw_thread_loop_lock(impl_->loop);
    impl_->ctx = pw_context_new(pw_thread_loop_get_loop(impl_->loop), nullptr, 0);
    if (!impl_->ctx) {
        pw_thread_loop_unlock(impl_->loop);
        log::error(TAG, "pw_context_new failed");
        return false;
    }
    impl_->core = pw_context_connect_fd(impl_->ctx, impl_->pipewire_fd, nullptr, 0);
    if (!impl_->core) {
        pw_thread_loop_unlock(impl_->loop);
        log::error(TAG, "pw_context_connect_fd failed");
        return false;
    }
    // The fd is now owned by PipeWire — clear our copy so ~Impl doesn't
    // close it twice.
    impl_->pipewire_fd = -1;

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE,     "Screen",
        nullptr);
    impl_->stream = pw_stream_new(impl_->core, "vivora-capture", props);
    if (!impl_->stream) {
        pw_thread_loop_unlock(impl_->loop);
        log::error(TAG, "pw_stream_new failed");
        return false;
    }
    pw_stream_add_listener(impl_->stream, &impl_->stream_listener, &kStreamEvents, impl_.get());

    // Negotiate raw video — accept BGRA / RGBA / NV12 / RGBx as a
    // first-cut; we'll pin to whatever VAAPI prefers in Stage 3.
    // C++ rejects taking the address of compound-literal rvalues
    // (SPA_RECTANGLE/SPA_FRACTION expand to those), so stash them in
    // named locals first.
    spa_rectangle size_def  = SPA_RECTANGLE(640, 480);
    spa_rectangle size_min  = SPA_RECTANGLE(1, 1);
    spa_rectangle size_max  = SPA_RECTANGLE(8192, 8192);
    spa_fraction  rate_def  = SPA_FRACTION(60, 1);
    spa_fraction  rate_min  = SPA_FRACTION(0, 1);
    spa_fraction  rate_max  = SPA_FRACTION(120, 1);

    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod* params[2];
    params[0] = (const spa_pod*)spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        // GNOME-shell screencast typically offers BGRx / RGBx / BGRA;
        // KDE KWin offers RGBA / NV12.  Pad the enum to cover both so
        // we don't fail negotiation on whichever compositor is in use.
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(8,
            SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_RGBA,
            SPA_VIDEO_FORMAT_BGR,
            SPA_VIDEO_FORMAT_RGB,
            SPA_VIDEO_FORMAT_NV12),
        SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(&size_def, &size_min, &size_max),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&rate_def, &rate_min, &rate_max));

    // SPA_META_Cursor request would go here when we re-enable METADATA
    // cursor mode (portal 1.18+).  Today we use EMBEDDED so no metadata
    // request is needed — the cursor is in the pixel buffer.
    int rc = pw_stream_connect(impl_->stream,
        PW_DIRECTION_INPUT,
        impl_->pipewire_node_id,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT
                                  | PW_STREAM_FLAG_MAP_BUFFERS),
        params, 1);
    pw_thread_loop_unlock(impl_->loop);
    if (rc < 0) {
        log::error(TAG, "pw_stream_connect failed: %s", spa_strerror(rc));
        return false;
    }

    log::info(TAG, "PipeWire stream connected to node %u", impl_->pipewire_node_id);
    return true;
}

bool PipeWireCapture::start() {
    if (!impl_->loop) return false;
    int rc = pw_thread_loop_start(impl_->loop);
    if (rc != 0) {
        log::error(TAG, "pw_thread_loop_start failed: %s", spa_strerror(rc));
        return false;
    }
    // PipeWire enters "paused" after connect; explicitly activate the
    // stream so it transitions to "streaming" and starts producing buffers.
    pw_thread_loop_lock(impl_->loop);
    pw_stream_set_active(impl_->stream, true);
    pw_thread_loop_unlock(impl_->loop);
    log::info(TAG, "Capture thread started");
    return true;
}

void PipeWireCapture::stop() {
    if (impl_->loop) pw_thread_loop_stop(impl_->loop);
}

} // namespace vivora::host

#endif // VIVORA_LINUX
