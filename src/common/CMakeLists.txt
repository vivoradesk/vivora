add_library(deskbeam_common STATIC
    utils/log.cpp
    utils/metrics.cpp
    utils/peer_code.cpp
    utils/thread_priority.cpp
    protocol/packet.cpp
    net/socket.cpp
    net/frame_fragmenter.cpp
    net/frame_assembler.cpp
    net/fec_codec.cpp
    net/stun_client.cpp
    net/rendezvous_protocol.cpp
    audio/audio_codec.cpp
    audio/resampler.cpp
    audio/jitter_buffer.cpp
    audio/audio_platform_stub.cpp
    crypto/random.cpp
    crypto/noise_nk.cpp
    crypto/host_identity.cpp
    crypto/packet_crypto.cpp
)

target_link_libraries(deskbeam_common PUBLIC opus speex_resampler monocypher)

if(WIN32)
    target_sources(deskbeam_common PRIVATE
        net/winsock_socket.cpp
        audio/wasapi_loopback.cpp
        audio/wasapi_output.cpp
    )
    target_link_libraries(deskbeam_common PUBLIC ws2_32 ole32 avrt bcrypt shell32)
else()
    target_sources(deskbeam_common PRIVATE net/posix_socket.cpp)
endif()

if(UNIX AND NOT APPLE)
    # Linux audio: PulseAudio output + loopback capture (PipeWire's
    # pulse-shim resolves both transparently on PW-based distros).
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PULSE REQUIRED libpulse-simple)
    target_sources(deskbeam_common PRIVATE
        audio/pulse_audio_output.cpp
        audio/pulse_audio_capture.cpp
    )
    target_include_directories(deskbeam_common PUBLIC ${PULSE_INCLUDE_DIRS})
    target_link_libraries(deskbeam_common PUBLIC ${PULSE_LIBRARIES})
endif()

if(APPLE)
    target_sources(deskbeam_common PRIVATE
        audio/mac_audio_capture.mm
        audio/mac_audio_output.mm
    )
    target_link_libraries(deskbeam_common PUBLIC
        "-framework Foundation"
        "-framework CoreMedia"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework ScreenCaptureKit"
    )
endif()

target_include_directories(deskbeam_common PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(deskbeam_common PUBLIC cxx_std_17)
