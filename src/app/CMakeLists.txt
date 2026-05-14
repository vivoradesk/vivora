if(UNIX AND NOT APPLE)
    # Linux client uses QOpenGLWidget (lives in Qt6::OpenGLWidgets, separate
    # from QtOpenGL which is the legacy QGLWidget module).
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets OpenGL OpenGLWidgets
                                        Qml Quick QuickControls2)
else()
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Qml Quick QuickControls2)
endif()
# qt_standard_project_setup() landed in Qt 6.3.  Ubuntu 22.04 ships Qt 6.2,
# so fall back to the equivalent CMake settings if the helper isn't there.
if(COMMAND qt_standard_project_setup)
    qt_standard_project_setup()
else()
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_INCLUDE_CURRENT_DIR ON)
endif()

add_executable(deskbeam
    main.cpp
    legacy_cli.cpp
    host_loop.cpp
    view_loop.cpp
    gui/gui_main.cpp
    gui/app_controller.cpp
    gui/settings.cpp
    gui/address_book.cpp
    gui/tray.cpp
    ${CMAKE_SOURCE_DIR}/qml/qml.qrc
)

target_link_libraries(deskbeam PRIVATE
    deskbeam_common
    Qt6::Core Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2
)

# Static Qt 6.x pulls Qt6Network in transitively from Qt6::Qml; QtNetwork
# in turn references OpenSSL 3 (TLS backend).  This particular pre-built
# Qt has the OpenSSL 1.1 import lib (libcrypto64MD.lib) baked into its
# link config but the source code references OpenSSL 3 symbols
# (OSSL_PARAM_*).  Mismatch.  Link our own OpenSSL 3 static libs first
# so the missing OSSL_PARAM symbols resolve, and pass /FORCE:MULTIPLE
# to swallow the duplicate-symbol errors from the stale 1.1 reference —
# we never call the QSslSocket APIs that would actually exercise either
# path so the runtime behaviour doesn't matter.
if(WIN32)
    set(_DESKBEAM_OPENSSL_DIR "C:/OpenSSL-WinUniversal/lib/VC/x64/MT")
    if(EXISTS "${_DESKBEAM_OPENSSL_DIR}/libssl_static.lib")
        target_link_libraries(deskbeam PRIVATE
            "${_DESKBEAM_OPENSSL_DIR}/libssl_static.lib"
            "${_DESKBEAM_OPENSSL_DIR}/libcrypto_static.lib"
            crypt32
        )
        target_link_options(deskbeam PRIVATE "/FORCE:MULTIPLE" "/IGNORE:4006" "/IGNORE:4088")
    else()
        message(WARNING "OpenSSL static libs not found at ${_DESKBEAM_OPENSSL_DIR}")
    endif()
endif()

# Client networking (cross-platform)
target_sources(deskbeam PRIVATE
    ${CMAKE_SOURCE_DIR}/src/client/net/video_receiver.cpp
    ${CMAKE_SOURCE_DIR}/src/client/net/client_session.cpp
)
# Host session + sender are cross-platform (pure C++).
target_sources(deskbeam PRIVATE
    ${CMAKE_SOURCE_DIR}/src/host/session/host_session.cpp
    ${CMAKE_SOURCE_DIR}/src/host/session/video_sender.cpp
    ${CMAKE_SOURCE_DIR}/src/host/audio/audio_sender.cpp
    ${CMAKE_SOURCE_DIR}/src/client/audio/audio_receiver.cpp
)
target_include_directories(deskbeam PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

if(APPLE)
    # macOS platform implementations (Objective-C++)
    target_sources(deskbeam PRIVATE
        mac_host_platform.mm
        mac_view_platform.mm
    )
    # macOS client: Cocoa window + AVSampleBufferDisplayLayer
    target_sources(deskbeam PRIVATE
        ${CMAKE_SOURCE_DIR}/src/client/render/mac_video_view.mm
    )
    # macOS host: ScreenCaptureKit + VideoToolbox + CGEvent input injection
    target_sources(deskbeam PRIVATE
        ${CMAKE_SOURCE_DIR}/src/host/capture/mac_screen_capture.mm
        ${CMAKE_SOURCE_DIR}/src/host/encode/mac_videotoolbox_encoder.mm
        ${CMAKE_SOURCE_DIR}/src/host/input/input_injector_mac.mm
    )
    target_link_libraries(deskbeam PRIVATE
        "-framework Cocoa"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreGraphics"
        "-framework VideoToolbox"
        "-framework QuartzCore"
        "-framework ScreenCaptureKit"
        "-framework ApplicationServices"
    )
endif()

if(UNIX AND NOT APPLE)
    # Linux platform.  L2 = client; L3 (in progress) = PipeWire capture
    # via xdg-desktop-portal + VAAPI HEVC encoder + uinput injection.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
        libavcodec libavutil libswscale)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    pkg_check_modules(DBUS     REQUIRED IMPORTED_TARGET dbus-1)

    target_sources(deskbeam PRIVATE
        linux_view_platform.cpp
        linux_host_platform.cpp
        ${CMAKE_SOURCE_DIR}/src/client/decode/ffmpeg_decoder.cpp
        ${CMAKE_SOURCE_DIR}/src/client/render/qt_gl_video_view.cpp
        # Linux host: PipeWire capture + VAAPI encoder + uinput injector,
        # wired into HostSession via LinuxHostPlatform.  Audio capture
        # via PulseAudio monitor is pending (L3 sub-task).
        ${CMAKE_SOURCE_DIR}/src/host/capture/pipewire_capture.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/vaapi_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/input/input_injector_linux.cpp
    )
    target_link_libraries(deskbeam PRIVATE
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        PkgConfig::LIBAV
        PkgConfig::PIPEWIRE
        PkgConfig::DBUS
    )
endif()

if(WIN32)
    # Windows platform implementations
    target_sources(deskbeam PRIVATE
        windows_host_platform.cpp
        windows_view_platform.cpp
    )
    # Host-side sources (Windows DXGI/AMF/NVENC/QSV + SendInput)
    target_sources(deskbeam PRIVATE
        ${CMAKE_SOURCE_DIR}/src/host/capture/dxgi_capture.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/amf_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/nvenc_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/qsv_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/encoder_factory.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/fallback_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/input/input_injector_windows.cpp
    )
    # Client decode/render (D3D11 — Windows-only)
    target_sources(deskbeam PRIVATE
        ${CMAKE_SOURCE_DIR}/src/client/decode/mf_decoder.cpp
        ${CMAKE_SOURCE_DIR}/src/client/render/d3d_renderer.cpp
        ${CMAKE_SOURCE_DIR}/src/client/render/stream_window.cpp
    )
    target_include_directories(deskbeam PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/amf
        ${CMAKE_SOURCE_DIR}/third_party/nvenc
        ${CMAKE_SOURCE_DIR}/third_party/onevpl/api
    )
    target_link_libraries(deskbeam PRIVATE
        d3d11
        dxgi
    )
endif()
