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
set(VIVORA_ARTEFACT_BASENAME
    "Vivora-${PROJECT_VERSION}${VIVORA_VERSION_SUFFIX}-${VIVORA_PLATFORM_TAG}"
    CACHE STRING "Base name used for installer / dmg / AppImage filenames")

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT to match static Qt build (Windows only)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Global output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Options
option(VIVORA_BUILD_TESTS "Build tests" ON)

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

# Third-party dependencies (audio: libopus + speex resampler)
add_subdirectory(third_party)

# Common library
add_subdirectory(src/common)

# Main application (single binary: host + client)
add_subdirectory(src/app)

# Standalone rendezvous + relay servers (no Qt, no media — just signalling).
add_subdirectory(src/relay)

# Legacy targets (kept for test compilation that references sources directly)
# add_subdirectory(src/host)   # sources used directly by src/app and tests
# add_subdirectory(src/client) # sources used directly by src/app and tests

if(VIVORA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
