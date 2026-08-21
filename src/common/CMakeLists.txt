# SPDX-FileCopyrightText: 2026 Andrii Myronov
# SPDX-License-Identifier: AGPL-3.0-or-later

add_library(vivora_common STATIC
    utils/log.cpp
    utils/metrics.cpp
    utils/peer_code.cpp
    utils/thread_priority.cpp
    protocol/packet.cpp
    protocol/clipboard_message.cpp
    net/socket.cpp
    net/frame_fragmenter.cpp
    net/frame_assembler.cpp
    net/fec_codec.cpp
    net/stun_client.cpp
    net/rendezvous_protocol.cpp
    net/relay_protocol.cpp
    audio/audio_codec.cpp
    audio/resampler.cpp
    audio/jitter_buffer.cpp
    audio/audio_platform_stub.cpp
    crypto/random.cpp
    crypto/noise_nk.cpp
    crypto/host_identity.cpp
    crypto/license_token.cpp
    crypto/packet_crypto.cpp
    crypto/peer_pin.cpp
)

target_link_libraries(vivora_common PUBLIC opus speex_resampler monocypher)

if(WIN32)
    target_sources(vivora_common PRIVATE
        net/winsock_socket.cpp
        audio/wasapi_loopback.cpp
        audio/wasapi_output.cpp
    )
    target_link_libraries(vivora_common PUBLIC ws2_32 ole32 avrt bcrypt shell32)
else()
    target_sources(vivora_common PRIVATE net/posix_socket.cpp)
endif()

if(UNIX AND NOT APPLE)
    # Linux audio: PulseAudio output + loopback capture (PipeWire's
    # pulse-shim resolves both transparently on PW-based distros).
    # libpulse-simple: pa_simple playback sink; libpulse: async threaded
    # mainloop + pa_stream used by the loopback capture.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PULSE REQUIRED libpulse-simple libpulse)
    target_sources(vivora_common PRIVATE
        audio/pulse_audio_output.cpp
        audio/pulse_audio_capture.cpp
    )
    target_include_directories(vivora_common PUBLIC ${PULSE_INCLUDE_DIRS})
    target_link_libraries(vivora_common PUBLIC ${PULSE_LIBRARIES})
endif()

if(APPLE)
    target_sources(vivora_common PRIVATE
        audio/mac_audio_capture.mm
        audio/mac_audio_output.mm
    )
    target_link_libraries(vivora_common PUBLIC
        "-framework Foundation"
        "-framework CoreMedia"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework ScreenCaptureKit"
    )
endif()

target_include_directories(vivora_common PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(vivora_common PUBLIC cxx_std_17)
