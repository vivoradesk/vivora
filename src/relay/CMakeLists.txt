add_executable(deskbeam-rendezvous
    rendezvous_server.cpp
)

target_link_libraries(deskbeam-rendezvous PRIVATE deskbeam_common)
target_include_directories(deskbeam-rendezvous PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(deskbeam-rendezvous PRIVATE cxx_std_17)


add_executable(deskbeam-relay
    relay_server.cpp
)

target_link_libraries(deskbeam-relay PRIVATE deskbeam_common)
target_include_directories(deskbeam-relay PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(deskbeam-relay PRIVATE cxx_std_17)


add_executable(deskbeam-license-mint
    license_mint.cpp
)
target_link_libraries(deskbeam-license-mint PRIVATE deskbeam_common monocypher)
target_include_directories(deskbeam-license-mint PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/monocypher
)
target_compile_features(deskbeam-license-mint PRIVATE cxx_std_17)
