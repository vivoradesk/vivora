add_executable(vivora-rendezvous
    rendezvous_server.cpp
    metrics_emitter.cpp
)

target_link_libraries(vivora-rendezvous PRIVATE vivora_common)
target_include_directories(vivora-rendezvous PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(vivora-rendezvous PRIVATE cxx_std_17)


add_executable(vivora-relay
    relay_server.cpp
    metrics_emitter.cpp
)

target_link_libraries(vivora-relay PRIVATE vivora_common)
target_include_directories(vivora-relay PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(vivora-relay PRIVATE cxx_std_17)


add_executable(vivora-license-mint
    license_mint.cpp
)
target_link_libraries(vivora-license-mint PRIVATE vivora_common monocypher)
target_include_directories(vivora-license-mint PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/monocypher
)
target_compile_features(vivora-license-mint PRIVATE cxx_std_17)
