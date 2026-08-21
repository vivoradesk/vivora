# SPDX-FileCopyrightText: 2026 Andrii Myronov
# SPDX-License-Identifier: AGPL-3.0-or-later

cmake_minimum_required(VERSION 3.20)
project(Vivora VERSION 0.1.0 LANGUAGES C CXX)

# Distribution artefact naming.  The build produces vivora.exe / vivora /
# Vivora.app at build/bin/ regardless of platform — these strings drive
# the *packaging* artefact names (installer / .dmg / .AppImage / .zip)
# once the packaging targets land.  Bumping VIVORA_VERSION_SUFFIX (e.g.
# "-beta1", "-rc1") on a release branch yields names like
# Vivora-0.1.0-beta1-windows-x64.msi without touching the project()
# version, which is what feeds the in-app About dialog.
set(VIVORA_VERSION_SUFFIX "" CACHE STRING
    "Optional suffix appended to release artefact filenames (e.g. -beta1)")
if(WIN32)
    set(VIVORA_PLATFORM_TAG "windows-x64")
elseif(APPLE)
    set(VIVORA_PLATFORM_TAG "macos-universal")
elseif(UNIX)
    set(VIVORA_PLATFORM_TAG "linux-x64")
else()
    set(VIVORA_PLATFORM_TAG "unknown")
endif()
# Deliberately NOT a cache variable: a cached value would survive a
# project(VERSION) bump in an existing build directory and happily name
# 0.1.1 binaries "Vivora-0.1.0-...".  VIVORA_VERSION_SUFFIX above stays
# cached because it is a user-facing knob, not derived state.
set(VIVORA_ARTEFACT_BASENAME
    "Vivora-${PROJECT_VERSION}${VIVORA_VERSION_SUFFIX}-${VIVORA_PLATFORM_TAG}")

if(APPLE)
    enable_language(OBJCXX)
endif()
if(MSVC)
    # Needed for the VERSIONINFO / icon resource in src/app.  The Visual Studio
    # generator infers RC on its own; Ninja does not.
    enable_language(RC)
endif()

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT to match static Qt build (Windows only)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # Read sources as UTF-8 and emit UTF-8 (VIV-124).  The tree is UTF-8
    # without a BOM, and without this MSVC reads narrow literals in whatever
    # the build machine's ANSI codepage happens to be and re-encodes them into
    # it -- so every em dash in a log message, and the separators in the
    # monitor panel, compiled to something that depended on who built it.
    add_compile_options(/utf-8)
endif()

# Global output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Options
option(VIVORA_BUILD_TESTS "Build tests" ON)
# The servers and the cross-platform test suite depend on nothing but
# vivora_common, so they can be built without Qt, ffmpeg or PipeWire present.
# CI leans on this: the fast gate needs one apt package instead of a Qt build.
option(VIVORA_BUILD_APP   "Build the Qt GUI/CLI application" ON)

# Platform detection
if(WIN32)
    add_compile_definitions(VIVORA_WINDOWS)
elseif(UNIX AND NOT APPLE)
    add_compile_definitions(VIVORA_LINUX)
elseif(APPLE)
    add_compile_definitions(VIVORA_MACOS)
endif()

# App version string exposed to C++ (e.g. the update check, VIV-69).
add_compile_definitions(VIVORA_VERSION="${PROJECT_VERSION}")

# Packaging scripts read this instead of re-deriving the version themselves,
# so CMake stays the single source of truth for what an artefact is called.
configure_file(${CMAKE_SOURCE_DIR}/packaging/artefact.env.in
               ${CMAKE_BINARY_DIR}/artefact.env @ONLY)

# Third-party dependencies (audio: libopus + speex resampler)
add_subdirectory(third_party)

# Common library
add_subdirectory(src/common)

# Main application (single binary: host + client)
if(VIVORA_BUILD_APP)
    add_subdirectory(src/app)
endif()

# Standalone rendezvous + relay servers (no Qt, no media — just signalling).
add_subdirectory(src/relay)

# Legacy targets (kept for test compilation that references sources directly)
# add_subdirectory(src/host)   # sources used directly by src/app and tests
# add_subdirectory(src/client) # sources used directly by src/app and tests

if(VIVORA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
