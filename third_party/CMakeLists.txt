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
    # Inner path so `#include "speexdsp_config_types.h"` (no prefix) from
    # speexdsp_types.h resolves on GCC.  MSVC happens to find it via the
    # outer path due to its `"foo.h"` search heuristics, but GCC needs it
    # explicitly listed.
    ${CMAKE_CURRENT_BINARY_DIR}/speex_generated/speex
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

# --- Monocypher (crypto primitives for Noise_NK) ------------------------------
# Single-file public-domain implementation of Curve25519, ChaCha20-Poly1305
# (IETF 12-byte nonce variant via crypto_aead_lock), and BLAKE2b.  We pick
# Monocypher over libsodium for build economy — two source files vs ~500,
# no autoconf / feature-detection, same cryptographic properties.
add_library(monocypher STATIC
    monocypher/monocypher.c
)
target_include_directories(monocypher PUBLIC monocypher)
