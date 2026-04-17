add_library(deskbeam_common STATIC
    utils/log.cpp
    utils/metrics.cpp
    protocol/packet.cpp
    net/frame_fragmenter.cpp
    net/frame_assembler.cpp
    net/fec_codec.cpp
    audio/audio_codec.cpp
    audio/resampler.cpp
    audio/jitter_buffer.cpp
    audio/audio_platform_stub.cpp
)

target_link_libraries(deskbeam_common PUBLIC opus speex_resampler)

if(WIN32)
    target_sources(deskbeam_common PRIVATE
        net/winsock_socket.cpp
        audio/wasapi_loopback.cpp
        audio/wasapi_output.cpp
    )
    target_link_libraries(deskbeam_common PUBLIC ws2_32 ole32 avrt)
else()
    target_sources(deskbeam_common PRIVATE net/posix_socket.cpp)
endif()

target_include_directories(deskbeam_common PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(deskbeam_common PUBLIC cxx_std_17)
