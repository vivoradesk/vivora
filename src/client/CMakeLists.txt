add_executable(vivora_client
    main.cpp
)

target_link_libraries(vivora_client PRIVATE vivora_common)

if(WIN32)
    target_sources(vivora_client PRIVATE
        net/video_receiver.cpp
        net/client_session.cpp
    )
endif()
