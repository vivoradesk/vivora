add_library(deskbeam_common STATIC
    utils/log.cpp
    utils/metrics.cpp
    protocol/packet.cpp
    net/frame_fragmenter.cpp
    net/frame_assembler.cpp
    net/fec_codec.cpp
)

if(WIN32)
    target_sources(deskbeam_common PRIVATE net/winsock_socket.cpp)
    target_link_libraries(deskbeam_common PUBLIC ws2_32)
else()
    target_sources(deskbeam_common PRIVATE net/posix_socket.cpp)
endif()

target_include_directories(deskbeam_common PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(deskbeam_common PUBLIC cxx_std_17)
