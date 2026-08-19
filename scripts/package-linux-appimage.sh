#!/usr/bin/env bash
# Build the Linux AppImage.
#
#   scripts/package-linux-appimage.sh [build-dir] [dist-dir]
#
# Meant to run inside the image from packaging/linux/Containerfile, which is
# Debian 11 (glibc 2.31) with Qt compiled from source. Running it on a newer
# distribution produces an AppImage that only works on distributions at least
# that new, which defeats the point.
#
# Three stages, and the split matters: linuxdeploy can produce an AppImage
# directly, but then there is no chance to remove the libraries it bundles
# that MUST come from the host instead.
set -euo pipefail

cd "$(dirname "$0")/.."
BUILD="${1:-build-appimage}"
DIST="${2:-dist}"
TOOLS="${VIVORA_APPIMAGE_TOOLS:-$HOME/.cache/vivora-appimage-tools}"
QT_PREFIX="${CMAKE_PREFIX_PATH:-/opt/qt}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Linux only." >&2; exit 1
fi

# ---------------------------------------------------------------- tools -----
mkdir -p "$TOOLS"
fetch() {
    local url="$1" out="$TOOLS/$2"
    if [[ ! -x "$out" ]]; then
        echo "==> fetching $2"
        curl -fsSL "$url" -o "$out"
        chmod +x "$out"
    fi
}
base="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous"
fetch "$base/linuxdeploy-x86_64.AppImage" linuxdeploy
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage" \
      linuxdeploy-plugin-qt
fetch "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" \
      appimagetool
# The classic type-2 runtime links libfuse2, which Ubuntu 22.04 and later do
# not install by default -- that is the notorious "dlopen(): error loading
# libfuse.so.2". This one is static and speaks fuse3.
if [[ ! -f "$TOOLS/runtime-x86_64" ]]; then
    echo "==> fetching type2 runtime"
    curl -fsSL "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64" \
        -o "$TOOLS/runtime-x86_64"
fi

# The tools are themselves AppImages, and we may well be inside a container
# with no /dev/fuse.
export APPIMAGE_EXTRACT_AND_RUN=1
LD="$TOOLS/linuxdeploy --appimage-extract-and-run"
AT="$TOOLS/appimagetool --appimage-extract-and-run"

# ---------------------------------------------------------------- build -----
echo "==> configuring"
cmake -S . -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    -DVIVORA_BUILD_TESTS=OFF
cmake --build "$BUILD"

# shellcheck disable=SC1090
. "$BUILD/artefact.env"
name="$VIVORA_ARTEFACT_BASENAME"

APPDIR="$BUILD/AppDir"
rm -rf "$APPDIR"
# The install rules already lay out bin/, share/applications, share/metainfo
# and share/icons/hicolor exactly where an AppDir wants them.
cmake --install "$BUILD" --component app --prefix "$APPDIR/usr" >/dev/null

# ------------------------------------------------------------- populate -----
echo "==> populating AppDir"
export QMAKE="$QT_PREFIX/bin/qmake6"
[[ -x "$QMAKE" ]] || QMAKE="$QT_PREFIX/bin/qmake"
export QML_SOURCES_PATHS="$PWD/qml"
# xcb only, deliberately.
#
# Native Wayland support lives in the separate qtwayland module, which the
# build image does not compile -- and shipping it would change behaviour
# rather than add to it: Qt prefers the wayland plugin when it exists, so a
# Wayland session would stop going through XWayland, which is the
# configuration every Linux test has run under. The client's relative-mouse
# capture is X11-only by design too (Wayland forbids the pointer warp it
# needs), so XWayland is the better target, not a fallback.
#
# On a Wayland desktop Qt finds no wayland plugin and picks xcb through
# XWayland on its own. Native Wayland is its own piece of work.
export QT_PLUGINS="platforms;platformthemes;imageformats;iconengines;xcbglintegrations;platforminputcontexts;tls;networkinformation"
export PATH="$TOOLS:$PATH"
# linuxdeploy resolves the binary's NEEDED entries through the normal loader
# search path, and Qt lives in a prefix that is not on it -- without this it
# reports "Could not find dependency: libQt6QuickControls2.so.6" and stops.
export LD_LIBRARY_PATH="$QT_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Qt 6.5+ loads libxcb-cursor at runtime for the xcb platform plugin -- it is
# not a NEEDED entry of anything, so nothing deploys it on its own, and
# without it Qt aborts with "Could not load the Qt platform plugin xcb" on any
# distribution that does not ship it.  Plenty do not.
XCB_CURSOR="$(ls /usr/lib/x86_64-linux-gnu/libxcb-cursor.so.0 2>/dev/null || true)"
[[ -n "$XCB_CURSOR" ]] || { echo "libxcb-cursor.so.0 missing from the build image" >&2; exit 1; }

$LD --appdir "$APPDIR" \
    -l "$XCB_CURSOR" \
    -e "$APPDIR/usr/bin/vivora" \
    -d "$APPDIR/usr/share/applications/dev.vivora.app.desktop" \
    -i "$APPDIR/usr/share/icons/hicolor/256x256/apps/dev.vivora.app.png" \
    --plugin qt

# ---------------------------------------------------------------- prune -----
# Anything that talks to a daemon, a driver or the display server has to be
# the host's copy. A bundled libpipewire cannot reach the host's SPA plugins;
# a bundled libGL cannot drive the host's GPU; a bundled libstdc++ that is
# older than the host's Mesa breaks Mesa.
# Note on what is NOT here: libxkbcommon.  libxkbcommon-x11 is bundled --
# Qt needs it and not every distribution ships it -- and the two are one
# source package sharing internal structures.  Pruning only the core half
# left a Debian 11 -x11 running against the host 1.4.0, which segfaults
# inside xkb_x11_keymap_new_from_device() before Qt opens a window.
echo "==> pruning host-owned libraries"
for lib in \
    libpipewire-0.3.so.0 libdbus-1.so.3 \
    libpulse.so.0 libpulse-simple.so.0 libpulsecommon-*.so \
    libglib-2.0.so.0 libgobject-2.0.so.0 libgio-2.0.so.0 libgmodule-2.0.so.0 \
    libGL.so.1 libEGL.so.1 libGLdispatch.so.0 libGLX.so.0 libOpenGL.so.0 \
    libdrm.so.2 libgbm.so.1 \
    libX11.so.6 libX11-xcb.so.1 libXext.so.6 libXfixes.so.3 \
    libxcb.so.1 libxcb-shm.so.0 libxcb-render.so.0 libxcb-glx.so.0 \
    libxcb-dri2.so.0 libxcb-dri3.so.0 libxcb-present.so.0 libxcb-sync.so.1 \
    libxcb-xfixes.so.0 libxcb-randr.so.0 libxcb-shape.so.0 libxcb-xkb.so.1 \
    libxcb-icccm.so.4 libxcb-image.so.0 libxcb-keysyms.so.1 \
    libxcb-render-util.so.0 libxcb-util.so.1 libxcb-xinerama.so.0 \
    libwayland-client.so.0 libwayland-egl.so.1 \
    libfontconfig.so.1 libfreetype.so.6 \
    libstdc++.so.6 libgcc_s.so.1 libm.so.6 libc.so.6
do
    rm -f "$APPDIR"/usr/lib/$lib
done

echo "==> bundled libraries kept:"
ls "$APPDIR/usr/lib" | sort | sed 's/^/    /'

# ------------------------------------------------------------- validate -----
desktop-file-validate "$APPDIR/usr/share/applications/dev.vivora.app.desktop"
if command -v appstreamcli >/dev/null 2>&1; then
    appstreamcli validate --no-net "$APPDIR/usr/share/metainfo/dev.vivora.app.metainfo.xml"
fi
# ffmpeg has to be bundled: the .so major version differs on every
# distribution, so leaving it to the host means the AppImage runs only where
# it was built.
ls "$APPDIR"/usr/lib/libxcb-cursor.so.* >/dev/null 2>&1 \
    || { echo "FATAL: libxcb-cursor was not bundled; Qt will not start" >&2; exit 1; }
# libxkbcommon and libxkbcommon-x11 travel together or not at all.
if ls "$APPDIR"/usr/lib/libxkbcommon-x11.so.* >/dev/null 2>&1; then
    ls "$APPDIR"/usr/lib/libxkbcommon.so.* >/dev/null 2>&1 \
        || { echo "FATAL: libxkbcommon-x11 bundled without libxkbcommon" >&2; exit 1; }
fi
for must in libavcodec libavutil libswscale; do
    ls "$APPDIR"/usr/lib/$must.so.* >/dev/null 2>&1 \
        || { echo "FATAL: $must was not bundled" >&2; exit 1; }
done

# ---------------------------------------------------------------- pack ------
mkdir -p "$DIST"
out="$DIST/$name.AppImage"
rm -f "$out"
ARCH=x86_64 $AT --runtime-file "$TOOLS/runtime-x86_64" "$APPDIR" "$out"
chmod +x "$out"

echo "wrote $out"
( cd "$DIST" && sha256sum "$(basename "$out")" | tee -a SHA256SUMS.txt >/dev/null )
( cd "$DIST" && grep "$(basename "$out")" SHA256SUMS.txt | tail -1 )

echo "==> glibc floor:"
objdump -T "$APPDIR/usr/bin/vivora" 2>/dev/null \
    | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -3
