#ifdef VIVORA_LINUX

#include "host/capture/wayland_outputs.h"

#ifndef VIVORA_HAVE_WAYLAND_CLIENT
// wayland-client not available at build time → no output enumeration; the
// host advertises a single display and the monitor panel stays a no-op.
namespace vivora::host {
std::vector<WaylandOutput> enumerate_wayland_outputs() { return {}; }
}
#else

#include "common/utils/log.h"

#include <wayland-client.h>
#include <cstring>
#include <map>

namespace vivora::host {

namespace {

constexpr const char* TAG = "WLOUT";

// Accumulator: one per bound wl_output, filled by the listener callbacks and
// harvested after two registry round-trips (geometry + mode arrive async).
struct OutputAcc {
    int32_t x = 0, y = 0;
    int32_t width = 0, height = 0;
    std::string name;
    bool got_geometry = false;
};

struct EnumState {
    wl_registry* registry = nullptr;
    std::map<wl_output*, OutputAcc> outputs;
    // xdg_output would give logical size/name more reliably, but wl_output
    // geometry+mode is enough for the list and needs no extra protocol.
};

void output_geometry(void* data, wl_output*, int32_t x, int32_t y,
                     int32_t /*phys_w*/, int32_t /*phys_h*/, int32_t /*subpixel*/,
                     const char* make, const char* model, int32_t /*transform*/) {
    auto* acc = static_cast<OutputAcc*>(data);
    acc->x = x;
    acc->y = y;
    // wl_output has no connector name; make/model is the closest label.
    if (model && *model) acc->name = model;
    else if (make && *make) acc->name = make;
    acc->got_geometry = true;
}

void output_mode(void* data, wl_output*, uint32_t flags,
                 int32_t width, int32_t height, int32_t /*refresh*/) {
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
    auto* acc = static_cast<OutputAcc*>(data);
    acc->width = width;
    acc->height = height;
}

void output_done(void*, wl_output*) {}
void output_scale(void*, wl_output*, int32_t) {}
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
void output_name(void* data, wl_output*, const char* name) {
    if (name && *name) static_cast<OutputAcc*>(data)->name = name;
}
void output_description(void*, wl_output*, const char*) {}
#endif

const wl_output_listener kOutputListener = {
    output_geometry,
    output_mode,
    output_done,
    output_scale,
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
    output_name,
    output_description,
#endif
};

void registry_global(void* data, wl_registry* reg, uint32_t id,
                     const char* iface, uint32_t version) {
    auto* st = static_cast<EnumState*>(data);
    if (std::strcmp(iface, wl_output_interface.name) == 0) {
        // Bind at a version that still carries geometry+mode (v2+); cap so we
        // don't request an interface newer than the compositor advertises.
        uint32_t bind_ver = version < 2 ? version : 2;
        auto* out = static_cast<wl_output*>(
            wl_registry_bind(reg, id, &wl_output_interface, bind_ver));
        auto& acc = st->outputs[out];
        wl_output_add_listener(out, &kOutputListener, &acc);
    }
}

void registry_global_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistryListener = {
    registry_global,
    registry_global_remove,
};

} // namespace

std::vector<WaylandOutput> enumerate_wayland_outputs() {
    std::vector<WaylandOutput> result;

    wl_display* dpy = wl_display_connect(nullptr);
    if (!dpy) {
        log::info(TAG, "no Wayland display — single-display host");
        return result;
    }

    EnumState st;
    st.registry = wl_display_get_registry(dpy);
    wl_registry_add_listener(st.registry, &kRegistryListener, &st);

    // First round-trip binds the outputs; second lets their geometry+mode
    // events (dispatched against the per-output accumulators) settle.
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);

    uint32_t idx = 0;
    for (auto& [out, acc] : st.outputs) {
        WaylandOutput wo;
        wo.x = acc.x;
        wo.y = acc.y;
        wo.width = acc.width;
        wo.height = acc.height;
        wo.name = acc.name;
        wo.primary = (acc.x == 0 && acc.y == 0);  // origin output — best-effort
        result.push_back(std::move(wo));
        wl_output_destroy(out);
        ++idx;
    }
    wl_registry_destroy(st.registry);
    wl_display_disconnect(dpy);

    log::info(TAG, "enumerated %zu Wayland output(s)", result.size());
    return result;
}

} // namespace vivora::host

#endif // VIVORA_HAVE_WAYLAND_CLIENT
#endif // VIVORA_LINUX
