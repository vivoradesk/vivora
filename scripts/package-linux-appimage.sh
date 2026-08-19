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
# Both platform plugins: Vivora's host path is portal/PipeWire based, so
# Wayland sessions are the primary target, but the client still runs under
# plenty of X11 desktops.
export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so"
export QT_PLUGINS="platforms;platformthemes;imageformats;iconengines;xcbglintegrations;platforminputcontexts;tls;networkinformation;wayland-shell-integration;wayland-graphics-integration-client"
export PATH="$TOOLS:$PATH"

$LD --appdir "$APPDIR" \
    -e "$APPDIR/usr/bin/vivora" \
    -d "$APPDIR/usr/share/applications/dev.vivora.app.desktop" \
    -i "$APPDIR/usr/share/icons/hicolor/256x256/apps/dev.vivora.app.png" \
    --plugin qt

# ---------------------------------------------------------------- prune -----
# Anything that talks to a daemon, a driver or the display server has to be
# the host's copy. A bundled libpipewire cannot reach the host's SPA plugins;
# a bundled libGL cannot drive the host's GPU; a bundled libstdc++ that is
# older than the host's Mesa breaks Mesa.
echo "==> pruning host-owned libraries"
for lib in \
    libpipewire-0.3.so.0 libdbus-1.so.3 \
    libpulse.so.0 libpulse-simple.so.0 libpulsecommon-*.so \
    libglib-2.0.so.0 libgobject-2.0.so.0 libgio-2.0.so.0 libgmodule-2.0.so.0 \
    libGL.so.1 libEGL.so.1 libGLdispatch.so.0 libGLX.so.0 libOpenGL.so.0 \
    libdrm.so.2 libgbm.so.1 \
    libX11.so.6 libX11-xcb.so.1 libxcb*.so.* libXext.so.6 libXfixes.so.3 \
    libwayland-client.so.0 libwayland-egl.so.1 libxkbcommon.so.0 \
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
