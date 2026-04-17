# Third-party audio dependencies: libopus + speexdsp resampler.

# --- libopus ------------------------------------------------------------------
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING        OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS       OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE   OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
if(MSVC)
    # Match our /MT static CRT.
    set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(opus EXCLUDE_FROM_ALL)

# --- speexdsp resampler (vendored minimal build) ------------------------------
# Upstream speexdsp uses autotools; we only need the resampler.
set(SPEEXDSP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/speexdsp_src)

# Generate speexdsp_config_types.h from template.
set(INCLUDE_STDINT "#include <stdint.h>")
set(SIZE16  "int16_t")
set(USIZE16 "uint16_t")
set(SIZE32  "int32_t")
set(USIZE32 "uint32_t")
configure_file(
    ${SPEEXDSP_SRC_DIR}/include/speex/speexdsp_config_types.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/speex_generated/speex/speexdsp_config_types.h
    @ONLY
)

add_library(speex_resampler STATIC
    ${SPEEXDSP_SRC_DIR}/libspeexdsp/resample.c
)
target_include_directories(speex_resampler PUBLIC
    ${SPEEXDSP_SRC_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/speex_generated
)
target_include_directories(speex_resampler PRIVATE
    ${SPEEXDSP_SRC_DIR}/libspeexdsp
)
target_compile_definitions(speex_resampler PRIVATE
    FLOATING_POINT
    EXPORT=
)
if(MSVC)
    target_compile_options(speex_resampler PRIVATE /wd4244 /wd4267 /wd4305)
endif()
