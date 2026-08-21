#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andrii Myronov
# SPDX-License-Identifier: AGPL-3.0-or-later

# Build and package the self-host server tarball.
#
#   scripts/package-servers.sh [build-dir] [dist-dir]
#
# The servers need no Qt, no ffmpeg and no PipeWire, so this configures with
# VIVORA_BUILD_APP=OFF and gets a build that works on a bare VPS image.
#
# Linked with -static-libstdc++ -static-libgcc rather than fully static: those
# two are the libraries most likely to be too old on a server, while a fully
# static glibc breaks getaddrinfo/NSS, which the rendezvous server needs to
# resolve anything.
set -euo pipefail

cd "$(dirname "$0")/.."
BUILD="${1:-build-servers}"
DIST="${2:-dist}"

cmake -S . -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DVIVORA_BUILD_APP=OFF \
    -DVIVORA_BUILD_TESTS=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build "$BUILD"

# CMake owns the artefact naming; we only read it back.
# shellcheck disable=SC1090
. "$BUILD/artefact.env"
name="vivora-servers-${VIVORA_VERSION}${VIVORA_VERSION_SUFFIX}-${VIVORA_PLATFORM_TAG}"

stage="$BUILD/stage/$name"
rm -rf "$stage"
mkdir -p "$stage"

# Explicit staging through install(), never a tar of the source tree: the
# working tree holds license.sk.
cmake --install "$BUILD" --component servers --prefix "$stage" >/dev/null

# Flatten the FHS prefix into a bundle: this is something you unpack and run
# install.sh from, not a tree you copy over /usr/local.  install.sh looks for
# systemd/ and bin/ next to itself.
mv "$stage/share/vivora/systemd" "$stage/systemd"
mv "$stage/share/vivora/LICENSE" "$stage/LICENSE"
rm -rf "$stage/share"

install -m 0755 deploy/selfhost/install.sh "$stage/install.sh"
install -m 0644 deploy/README.md           "$stage/README.md"

for b in vivora-relay vivora-rendezvous; do
    [[ -x "$stage/bin/$b" ]]                  || { echo "missing bin/$b" >&2; exit 1; }
    [[ -f "$stage/systemd/$b.service" ]]      || { echo "missing systemd/$b.service" >&2; exit 1; }
done
[[ -x "$stage/install.sh" ]] || { echo "missing install.sh" >&2; exit 1; }

echo "== runtime dependencies =="
ldd "$stage/bin/vivora-relay" | sed 's/^[[:space:]]*//'

mkdir -p "$DIST"
tar czf "$DIST/$name.tar.gz" -C "$(dirname "$stage")" "$name"
echo "wrote $DIST/$name.tar.gz"

( cd "$DIST" && sha256sum "$name.tar.gz" | tee -a SHA256SUMS.txt >/dev/null )
( cd "$DIST" && grep "$name.tar.gz" SHA256SUMS.txt | tail -1 )
