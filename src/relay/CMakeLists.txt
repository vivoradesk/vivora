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
