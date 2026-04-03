add_executable(deskbeam_client
    main.cpp
)

target_link_libraries(deskbeam_client PRIVATE deskbeam_common)

if(WIN32)
    target_sources(deskbeam_client PRIVATE
        net/video_receiver.cpp
        net/client_session.cpp
    )
endif()
