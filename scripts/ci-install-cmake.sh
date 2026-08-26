#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andrii Myronov
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Install a CMake new enough to configure this project inside the Debian 11
# container the CI and release jobs build in.
#
# Bullseye ships CMake 3.18 and the project asks for 3.20 (Qt 6.8 wants 3.21+),
# so `apt-get install cmake` there produces a container that cannot configure
# the tree at all — the job dies at the first cmake invocation. The release
# image solves this by unpacking Kitware's own build; see
# packaging/linux/Containerfile, which pins the same version. Any job that runs
# on bare debian:11 rather than that image needs this script.
#
# Requires curl and ca-certificates.
set -eu

CMAKE_VERSION="${CMAKE_VERSION:-3.28.6}"
ARCH="$(uname -m)"

url="https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${ARCH}.tar.gz"
curl -fsSL "$url" | tar -xz -C /opt
for tool in cmake ctest cpack; do
    ln -sf "/opt/cmake-${CMAKE_VERSION}-linux-${ARCH}/bin/${tool}" "/usr/local/bin/${tool}"
done

cmake --version
