add_executable(deskbeam-rendezvous
    rendezvous_server.cpp
)

target_link_libraries(deskbeam-rendezvous PRIVATE deskbeam_common)
target_include_directories(deskbeam-rendezvous PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(deskbeam-rendezvous PRIVATE cxx_std_17)
