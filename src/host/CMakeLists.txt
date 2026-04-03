add_executable(deskbeam_host
    main.cpp
)

target_link_libraries(deskbeam_host PRIVATE deskbeam_common)

if(WIN32)
    target_sources(deskbeam_host PRIVATE
        capture/dxgi_capture.cpp
        encode/amf_encoder.cpp
        session/video_sender.cpp
        session/host_session.cpp
    )
    target_include_directories(deskbeam_host PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/amf
    )
    target_link_libraries(deskbeam_host PRIVATE
        d3d11
        dxgi
    )
endif()
