# Tests are split into two groups:
#   1. Cross-platform unit tests — exercise only `vivora_common` (protocol,
#      codec, FEC, crypto, jitter, STUN client).  Build and run on every
#      target.
#   2. Windows-only integration tests — pull in DXGI capture, AMF encoder,
#      Media Foundation decoder, etc.  Gated on WIN32 until equivalent
#      Linux/macOS backends ship.

# --- Cross-platform unit tests -----------------------------------------------

add_executable(fragment_test fragment_test.cpp)
target_link_libraries(fragment_test PRIVATE vivora_common)
target_include_directories(fragment_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fragment_test COMMAND fragment_test)

add_executable(fec_test fec_test.cpp)
target_link_libraries(fec_test PRIVATE vivora_common)
target_include_directories(fec_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fec_test COMMAND fec_test)

add_executable(audio_test audio_test.cpp)
target_link_libraries(audio_test PRIVATE vivora_common)
target_include_directories(audio_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME audio_test COMMAND audio_test)

add_executable(stun_test stun_test.cpp)
target_link_libraries(stun_test PRIVATE vivora_common)
target_include_directories(stun_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME stun_test COMMAND stun_test)

add_executable(noise_test noise_test.cpp)
target_link_libraries(noise_test PRIVATE vivora_common)
target_include_directories(noise_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME noise_test COMMAND noise_test)

# Lock-free pipeline primitives (VIV-81): header-only, so the test only needs
# a threading library.  Threads::Threads keeps it correct on Linux/macOS too.
find_package(Threads REQUIRED)
add_executable(lockfree_test lockfree_test.cpp)
target_link_libraries(lockfree_test PRIVATE Threads::Threads)
target_include_directories(lockfree_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME lockfree_test COMMAND lockfree_test)

# Smoke test for vivora-relay — must be run manually with the relay
# server running on localhost:7100.  Not registered with ctest because the
# server isn't auto-started here; the test fails fast on missing server.
add_executable(relay_test relay_test.cpp)
target_link_libraries(relay_test PRIVATE vivora_common)
target_include_directories(relay_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

# --- Windows-only integration tests ------------------------------------------

if(WIN32)
    add_executable(capture_test
        capture_test.cpp
        ${CMAKE_SOURCE_DIR}/src/host/capture/dxgi_capture.cpp
    )
    target_link_libraries(capture_test PRIVATE vivora_common d3d11 dxgi)
    target_include_directories(capture_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME capture_test COMMAND capture_test)

    # The encoder factory pulls in every Windows backend (AMF/NVENC/QSV +
    # the fallback wrapper), so tests using IVideoEncoder::create need all of
    # them — the source list rotted when the factory grew (VIV-84 drive-by).
    set(WIN_ENCODER_SOURCES
        ${CMAKE_SOURCE_DIR}/src/host/encode/amf_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/nvenc_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/qsv_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/encoder_factory.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/fallback_encoder.cpp
    )
    set(WIN_ENCODER_INCLUDES
        ${CMAKE_SOURCE_DIR}/third_party/amf
        ${CMAKE_SOURCE_DIR}/third_party/nvenc
        ${CMAKE_SOURCE_DIR}/third_party/onevpl/api
    )

    add_executable(encode_test
        encode_test.cpp
        ${CMAKE_SOURCE_DIR}/src/host/capture/dxgi_capture.cpp
        ${WIN_ENCODER_SOURCES}
    )
    target_link_libraries(encode_test PRIVATE vivora_common d3d11 dxgi)
    target_include_directories(encode_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${WIN_ENCODER_INCLUDES}
    )
    add_test(NAME encode_test COMMAND encode_test)

    add_executable(sdr_white_test sdr_white_test.cpp)
    target_include_directories(sdr_white_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

    add_executable(capture_diag
        capture_diag.cpp
        ${CMAKE_SOURCE_DIR}/src/host/capture/dxgi_capture.cpp
    )
    target_link_libraries(capture_diag PRIVATE vivora_common d3d11 dxgi)
    target_include_directories(capture_diag PRIVATE ${CMAKE_SOURCE_DIR}/src)

    add_executable(transport_test
        transport_test.cpp
        ${CMAKE_SOURCE_DIR}/src/host/capture/dxgi_capture.cpp
        ${CMAKE_SOURCE_DIR}/src/host/session/video_sender.cpp
        ${CMAKE_SOURCE_DIR}/src/host/session/paced_sender.cpp
        ${CMAKE_SOURCE_DIR}/src/client/net/video_receiver.cpp
        ${WIN_ENCODER_SOURCES}
    )
    target_link_libraries(transport_test PRIVATE vivora_common d3d11 dxgi)
    target_include_directories(transport_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${WIN_ENCODER_INCLUDES}
    )
    add_test(NAME transport_test COMMAND transport_test)

    add_executable(decode_test
        decode_test.cpp
        ${CMAKE_SOURCE_DIR}/src/client/decode/mf_decoder.cpp
    )
    target_link_libraries(decode_test PRIVATE vivora_common d3d11)
    target_include_directories(decode_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME decode_test COMMAND decode_test)
endif()
