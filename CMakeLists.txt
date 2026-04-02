cmake_minimum_required(VERSION 3.20)
project(DeskBeam VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Global output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Options
option(DESKBEAM_BUILD_TESTS "Build tests" ON)
option(DESKBEAM_BUILD_HOST "Build host application" ON)
option(DESKBEAM_BUILD_CLIENT "Build client application" ON)

# Platform detection
if(WIN32)
    add_compile_definitions(DESKBEAM_WINDOWS)
elseif(UNIX AND NOT APPLE)
    add_compile_definitions(DESKBEAM_LINUX)
elseif(APPLE)
    add_compile_definitions(DESKBEAM_MACOS)
endif()

# Common library (shared between host and client)
add_subdirectory(src/common)

if(DESKBEAM_BUILD_HOST)
    add_subdirectory(src/host)
endif()

if(DESKBEAM_BUILD_CLIENT)
    add_subdirectory(src/client)
endif()

if(DESKBEAM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
