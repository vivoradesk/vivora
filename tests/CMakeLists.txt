if(WIN32)
    add_executable(capture_test
        capture_test.cpp
        ${CMAKE_SOURCE_DIR}/src/host/capture/dxgi_capture.cpp
    )
    target_link_libraries(capture_test PRIVATE deskbeam_common d3d11 dxgi)
    target_include_directories(capture_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME capture_test COMMAND capture_test)
endif()
