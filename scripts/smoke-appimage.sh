#!/usr/bin/env bash
# Run the AppImage on a distribution it was not built on.
#
#   scripts/smoke-appimage.sh dist/Vivora-0.1.0-linux-x64.AppImage [image]
#
# The point is the libraries we deliberately did NOT bundle: mesa, glib, dbus,
# fontconfig, the X11 and xcb stack. Those have to come from the host, and a
# Fedora container has a completely different set from the Debian 11 the
# AppImage was built in. If it starts there, the bundle/prune split is right.
#
# Default image is Fedora because it is the furthest from the build base that
# still matters. Pass another to widen the sweep:
#
#   scripts/smoke-appimage.sh dist/*.AppImage docker.io/library/rockylinux:9
#   scripts/smoke-appimage.sh dist/*.AppImage docker.io/library/ubuntu:20.04
set -euo pipefail

APPIMAGE="${1:?usage: $0 <AppImage> [container image]}"
IMAGE="${2:-docker.io/library/fedora:40}"
APPIMAGE="$(readlink -f "$APPIMAGE")"

command -v podman >/dev/null || { echo "podman not found" >&2; exit 1; }

# A GUI test needs a display to talk to. Without one we can still check that
# the binary loads and its CLI works, which catches every missing-library and
# glibc problem -- just not the rendering.
gui_args=()
if [[ -n "${DISPLAY:-}" && -S "/tmp/.X11-unix/X${DISPLAY#*:}" ]]; then
    gui_args+=( -e "DISPLAY=$DISPLAY" -v /tmp/.X11-unix:/tmp/.X11-unix )
    [[ -n "${XAUTHORITY:-}" ]] && gui_args+=( -v "$XAUTHORITY:/root/.Xauthority:ro" )
    echo "==> display $DISPLAY will be passed through"
else
    echo "==> no display; running the headless checks only"
fi
# The GPU nodes, so a bundled libva can find the host's driver.
[[ -d /dev/dri ]] && gui_args+=( --device /dev/dri )

cat > /tmp/vivora-smoke-inner.sh <<'INNER'
set -e
echo "== distribution =="
cat /etc/os-release | grep -E '^(PRETTY_NAME|VERSION_ID)=' || true
echo "== glibc =="
ldd --version | head -1

# Only what a *host* system is expected to provide. Anything missing here is
# something the AppImage should have bundled and did not.
echo "== installing host-side libraries =="
if command -v dnf >/dev/null; then
    dnf -y -q install mesa-libGL mesa-libEGL libglvnd-opengl libX11 libxcb \
        xcb-util-wm xcb-util-image xcb-util-keysyms xcb-util-renderutil \
        libxkbcommon-x11 fontconfig fribidi dbus-libs glib2 pulseaudio-libs \
        libva pipewire-libs >/dev/null
elif command -v apt-get >/dev/null; then
    apt-get -qq update >/dev/null
    apt-get -qq install -y --no-install-recommends libgl1 libegl1 libopengl0 \
        libx11-6 libxcb1 libxkbcommon-x11-0 libfontconfig1 libfribidi0 \
        libdbus-1-3 libglib2.0-0 libpulse0 libva2 libpipewire-0.3-0 >/dev/null
fi

cd /tmp
cp /appimage vivora.AppImage
chmod +x vivora.AppImage

# The bundled runtime speaks fuse3, but a container has no /dev/fuse at all,
# so extract instead of mounting. This is also the documented fallback we tell
# users about, so it is worth exercising.
echo "== extracting =="
./vivora.AppImage --appimage-extract >/dev/null
APP=squashfs-root/AppRun

echo "== dynamic linker check =="
if ldd squashfs-root/usr/bin/vivora | grep -i "not found"; then
    echo "FAIL: unresolved libraries"; exit 1
fi
echo "  all libraries resolve"

echo "== CLI =="
"$APP" --help | head -3

if [ -n "${DISPLAY:-}" ]; then
    echo "== GUI =="
    export VIVORA_NO_AUTOSTART=1
    timeout 25 "$APP" &
    pid=$!
    sleep 15
    log="$HOME/.local/share/Vivora/Vivora/vivora.log"
    if [ -f "$log" ]; then
        grep -E "GUI ready|No system tray|error" "$log" | tail -5
        # Which TLS backend came up.  cert-only means HTTPS is dead, and with
        # it the update check, announcements and account sign-in -- worth
        # failing on, because the app starts perfectly well without it.
        grep -E "TLS:" "$log" | tail -1
        if grep -q "supportsSsl=0" "$log"; then
            echo "FAIL: no working TLS backend"; kill $pid 2>/dev/null || true; exit 1
        fi
        if grep -q "GUI ready" "$log"; then echo "  GUI reached ready"; else
            echo "FAIL: GUI never reported ready"; kill $pid 2>/dev/null || true; exit 1; fi
    else
        echo "FAIL: no log written at $log"; kill $pid 2>/dev/null || true; exit 1
    fi
    kill $pid 2>/dev/null || true
fi
echo "== OK =="
INNER

podman run --rm \
    -v "$APPIMAGE:/appimage:ro" \
    -v /tmp/vivora-smoke-inner.sh:/smoke.sh:ro \
    "${gui_args[@]}" \
    "$IMAGE" bash /smoke.sh
