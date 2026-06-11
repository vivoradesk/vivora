#ifdef VIVORA_LINUX

#include "host/capture/x11_cursor.h"
#include "common/utils/log.h"

#include <algorithm>
#include <dlfcn.h>

namespace vivora::host {

namespace {
const char* TAG = "X11Cursor";

// ---- Minimal X11 / XFixes ABI (resolved via dlsym; no -dev headers) ------
typedef void  XDisplay;
typedef unsigned long XID;
typedef XID   Window;
typedef int   XBool;
typedef unsigned long XAtom;

// Layout must match X11/extensions/Xfixes.h's XFixesCursorImage.
struct XFixesCursorImage {
    short          x, y;
    unsigned short width, height;
    unsigned short xhot, yhot;
    unsigned long  cursor_serial;
    unsigned long* pixels;        // one ARGB pixel per long (low 32 bits)
    XAtom          atom;
    const char*    name;
};

typedef XDisplay* (*XOpenDisplay_t)(const char*);
typedef int       (*XCloseDisplay_t)(XDisplay*);
typedef Window    (*XDefaultRootWindow_t)(XDisplay*);
typedef int       (*XDefaultScreen_t)(XDisplay*);
typedef int       (*XDisplayWidth_t)(XDisplay*, int);
typedef int       (*XDisplayHeight_t)(XDisplay*, int);
typedef int       (*XFree_t)(void*);
typedef XFixesCursorImage* (*XFixesGetCursorImage_t)(XDisplay*);
} // namespace

struct X11Cursor::Impl {
    void* x11_lib  = nullptr;
    void* xfix_lib = nullptr;
    XDisplay* dpy  = nullptr;
    int screen_w = 1, screen_h = 1;

    unsigned long last_serial = 0;
    bool     have_serial = false;
    uint32_t next_id = 1;

    XCloseDisplay_t         XCloseDisplay = nullptr;
    XFree_t                 XFree = nullptr;
    XFixesGetCursorImage_t  XFixesGetCursorImage = nullptr;
};

X11Cursor::~X11Cursor() { shutdown(); }

bool X11Cursor::available() const { return impl_ && impl_->dpy; }

bool X11Cursor::init() {
    if (impl_) return available();
    impl_ = new Impl();

    impl_->x11_lib  = dlopen("libX11.so.6",     RTLD_NOW | RTLD_GLOBAL);
    if (!impl_->x11_lib) impl_->x11_lib = dlopen("libX11.so", RTLD_NOW | RTLD_GLOBAL);
    impl_->xfix_lib = dlopen("libXfixes.so.3",  RTLD_NOW);
    if (!impl_->xfix_lib) impl_->xfix_lib = dlopen("libXfixes.so", RTLD_NOW);
    if (!impl_->x11_lib || !impl_->xfix_lib) {
        log::info(TAG, "libX11/libXfixes unavailable — portal cursor will be used");
        shutdown();
        return false;
    }

    auto XOpenDisplay = reinterpret_cast<XOpenDisplay_t>(dlsym(impl_->x11_lib, "XOpenDisplay"));
    auto XDefaultScreen = reinterpret_cast<XDefaultScreen_t>(dlsym(impl_->x11_lib, "XDefaultScreen"));
    auto XDisplayWidth = reinterpret_cast<XDisplayWidth_t>(dlsym(impl_->x11_lib, "XDisplayWidth"));
    auto XDisplayHeight = reinterpret_cast<XDisplayHeight_t>(dlsym(impl_->x11_lib, "XDisplayHeight"));
    impl_->XCloseDisplay = reinterpret_cast<XCloseDisplay_t>(dlsym(impl_->x11_lib, "XCloseDisplay"));
    impl_->XFree = reinterpret_cast<XFree_t>(dlsym(impl_->x11_lib, "XFree"));
    impl_->XFixesGetCursorImage =
        reinterpret_cast<XFixesGetCursorImage_t>(dlsym(impl_->xfix_lib, "XFixesGetCursorImage"));
    if (!XOpenDisplay || !XDefaultScreen || !XDisplayWidth || !XDisplayHeight ||
        !impl_->XCloseDisplay || !impl_->XFree || !impl_->XFixesGetCursorImage) {
        log::warn(TAG, "X11/XFixes symbols missing — portal cursor will be used");
        shutdown();
        return false;
    }

    impl_->dpy = XOpenDisplay(nullptr);   // honours $DISPLAY
    if (!impl_->dpy) {
        log::info(TAG, "XOpenDisplay failed (no X session) — portal cursor will be used");
        shutdown();
        return false;
    }
    const int scr = XDefaultScreen(impl_->dpy);
    impl_->screen_w = std::max(1, XDisplayWidth(impl_->dpy, scr));
    impl_->screen_h = std::max(1, XDisplayHeight(impl_->dpy, scr));
    log::info(TAG, "X11 cursor source up (%dx%d screen)", impl_->screen_w, impl_->screen_h);
    return true;
}

void X11Cursor::shutdown() {
    if (!impl_) return;
    if (impl_->dpy && impl_->XCloseDisplay) impl_->XCloseDisplay(impl_->dpy);
    if (impl_->xfix_lib) dlclose(impl_->xfix_lib);
    if (impl_->x11_lib)  dlclose(impl_->x11_lib);
    delete impl_;
    impl_ = nullptr;
}

bool X11Cursor::poll(float& x_norm, float& y_norm, bool& visible,
                     bool& shape_changed, Shape& shape) {
    shape_changed = false;
    if (!available()) return false;

    XFixesCursorImage* img = impl_->XFixesGetCursorImage(impl_->dpy);
    if (!img) {                 // cursor currently hidden / no image
        visible = false;
        return true;
    }

    // img->x / img->y is the pointer hotspot location in screen coords.
    auto clamp01 = [](float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); };
    x_norm  = clamp01(float(img->x) / float(impl_->screen_w));
    y_norm  = clamp01(float(img->y) / float(impl_->screen_h));
    visible = true;

    if (!impl_->have_serial || img->cursor_serial != impl_->last_serial) {
        impl_->last_serial = img->cursor_serial;
        impl_->have_serial = true;
        shape.id        = impl_->next_id++;
        shape.width     = img->width;
        shape.height    = img->height;
        shape.hotspot_x = img->xhot;
        shape.hotspot_y = img->yhot;
        const size_t n = static_cast<size_t>(img->width) * img->height;
        shape.bgra.resize(n * 4);
        // XFixes pixels are premultiplied ARGB packed in the low 32 bits of
        // each `long`; repack to BGRA byte order for the wire/CursorShape.
        for (size_t i = 0; i < n; ++i) {
            const unsigned long p = img->pixels[i];
            shape.bgra[i * 4 + 0] = static_cast<uint8_t>(p & 0xFF);          // B
            shape.bgra[i * 4 + 1] = static_cast<uint8_t>((p >> 8) & 0xFF);   // G
            shape.bgra[i * 4 + 2] = static_cast<uint8_t>((p >> 16) & 0xFF);  // R
            shape.bgra[i * 4 + 3] = static_cast<uint8_t>((p >> 24) & 0xFF);  // A
        }
        shape_changed = true;
    }

    impl_->XFree(img);
    return true;
}

} // namespace vivora::host

#endif // VIVORA_LINUX
