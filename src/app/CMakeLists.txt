find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
qt_standard_project_setup()

add_executable(deskbeam
    main.cpp
)

target_link_libraries(deskbeam PRIVATE deskbeam_common Qt6::Core Qt6::Widgets)

# Client networking (cross-platform)
target_sources(deskbeam PRIVATE
    ${CMAKE_SOURCE_DIR}/src/client/net/video_receiver.cpp
    ${CMAKE_SOURCE_DIR}/src/client/net/client_session.cpp
)
target_include_directories(deskbeam PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

if(APPLE)
    # macOS client: Cocoa window + AVSampleBufferDisplayLayer
    target_sources(deskbeam PRIVATE
        ${CMAKE_SOURCE_DIR}/src/client/render/mac_video_view.mm
    )
    target_link_libraries(deskbeam PRIVATE
        "-framework Cocoa"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework VideoToolbox"
        "-framework QuartzCore"
    )
endif()

if(WIN32)
    # Host-side sources (Windows-only for now — macOS host comes later)
    target_sources(deskbeam PRIVATE
        ${CMAKE_SOURCE_DIR}/src/host/capture/dxgi_capture.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/amf_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/session/video_sender.cpp
        ${CMAKE_SOURCE_DIR}/src/host/session/host_session.cpp
        ${CMAKE_SOURCE_DIR}/src/host/input/input_injector.cpp
    )
    # Client decode/render (D3D11 — Windows-only)
    target_sources(deskbeam PRIVATE
        ${CMAKE_SOURCE_DIR}/src/client/decode/mf_decoder.cpp
        ${CMAKE_SOURCE_DIR}/src/client/render/d3d_renderer.cpp
        ${CMAKE_SOURCE_DIR}/src/client/render/stream_window.cpp
    )
    target_include_directories(deskbeam PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/amf
    )
    target_link_libraries(deskbeam PRIVATE
        d3d11
        dxgi
    )
endif()
