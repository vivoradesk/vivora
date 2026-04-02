add_executable(deskbeam_host
    main.cpp
)

target_link_libraries(deskbeam_host PRIVATE deskbeam_common)

if(WIN32)
    target_sources(deskbeam_host PRIVATE
        capture/dxgi_capture.cpp
    )
    target_link_libraries(deskbeam_host PRIVATE
        d3d11
        dxgi
    )
endif()
