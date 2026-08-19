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


# NOTE: the license minting CLI used to live here.  It moved to the closed
# vivora-cloud repository (cmd/mint), which already produced wire-identical
# 95-byte tokens.  Keeping a signing tool next to the .gitignore entry for
# license.sk was an accident waiting to happen in a public repository.
# Token *verification* stays here and is public by design:
# common/crypto/license_token.{h,cpp} + the embedded public key.
