cmake_minimum_required(VERSION 3.20)
project(Vivora VERSION 0.1.0 LANGUAGES C CXX)

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
