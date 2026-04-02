add_library(deskbeam_common STATIC
    utils/log.cpp
    utils/metrics.cpp
    protocol/packet.cpp
)

target_include_directories(deskbeam_common PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(deskbeam_common PUBLIC cxx_std_17)
