if(UNIX AND NOT APPLE)
    # Linux is CLI-only for now (P5 GUI is Windows-first).  Ubuntu 22.04
    # jammy ships only Qt 6.2 base — no qt6-declarative-dev — so requiring
    # Qml/Quick/QuickControls2 here would force every Linux contributor to
    # build Qt from source.  Pull them in once we wire the Linux GUI.
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets OpenGL OpenGLWidgets)
else()
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Qml Quick QuickControls2)
endif()
# qt_standard_project_setup() landed in Qt 6.3.  Ubuntu 22.04 ships Qt 6.2,
# so fall back to the equivalent CMake settings if the helper isn't there.
if(COMMAND qt_standard_project_setup)
    qt_standard_project_setup()
endif()
# qt_standard_project_setup() turns on AUTOMOC/AUTOUIC but is fuzzy on
# AUTORCC across Qt versions — set it explicitly so .qrc files (the QML
# bundle) are always compiled into the binary.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Bundle vivora as Vivora.app on macOS so the OS treats it as a real
# app (tray icon, accessibility prompts, Info.plist).  The CLI binary
# still lives at Vivora.app/Contents/MacOS/vivora — argv parsing in
# main.cpp routes --host/--view to the CLI path, no args to the GUI.
if(APPLE)
    set(VIVORA_MAC_APP_BUNDLE TRUE)
else()
    set(VIVORA_MAC_APP_BUNDLE FALSE)
endif()

add_executable(vivora
    main.cpp
    legacy_cli.cpp
    host_loop.cpp
    view_loop.cpp
)
if(WIN32)
    # Build as a /SUBSYSTEM:WINDOWS exe so launching the GUI from
    # Explorer / Start menu / tray doesn't spawn an empty black
    # console window.  CLI mode (`--host` / `--view`) re-attaches to
    # the parent console at runtime in main.cpp so stdout/stderr
    # still land in the calling PowerShell.  WIN32_EXECUTABLE makes
    # MSVC link with WinMainCRTStartup; Qt6's qtmain shim bridges
    # that back to plain `int main(...)`.
    set_target_properties(vivora PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
if(VIVORA_MAC_APP_BUNDLE)
    set_target_properties(vivora PROPERTIES
        MACOSX_BUNDLE TRUE
        # Bundle display name: Vivora.app (CMake derives from OUTPUT_NAME).
        OUTPUT_NAME "Vivora"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.vivora.app"
        MACOSX_BUNDLE_BUNDLE_NAME "Vivora"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
    # Ad-hoc codesign with a stable identifier so the OS sees the same
    # "app" across rebuilds and the Screen Recording / Accessibility
    # grants in TCC survive iteration.  Without this, every rebuild
    # changes the signature and macOS treats the binary as a "new" app
    # — the toggle still shows ON in System Settings but the grant
    # actually belongs to the old signature and events get rejected.
    # `--sign -` is the ad-hoc identity (no Apple Developer cert
    # required); for distribution we'll swap this for a real Developer
    # ID Application cert in a separate packaging step.
    add_custom_command(TARGET vivora POST_BUILD
        COMMAND codesign --force --sign - --identifier dev.vivora.app
                $<TARGET_BUNDLE_DIR:vivora>
        COMMENT "Ad-hoc codesigning Vivora.app for stable TCC grants")
    # Ad-hoc signatures don't preserve TCC grants across rebuilds —
    # the cdhash changes every build and macOS treats the binary as
    # "different".  The Settings toggle keeps showing ON but events
    # are silently rejected and the system re-prompts on the next
    # capture / inject attempt.  Reset cleanly so the next launch
    # gets a fresh prompt that actually registers.  Real fix is a
    # Developer ID Application cert (then the designated requirement
    # is identifier+team, not cdhash) — pending Apple Developer
    # enrolment; see [[project_post_rename_plan]].  || true so a
    # not-yet-granted state doesn't fail the build.
    add_custom_command(TARGET vivora POST_BUILD
        COMMAND tccutil reset ScreenCapture dev.vivora.app || true
        COMMAND tccutil reset Accessibility dev.vivora.app || true
        COMMENT "Resetting TCC grants for ad-hoc signed dev.vivora.app")
endif()

# GUI sources land on Windows AND macOS — both use Qt 6 + QML + the same
# AppController / HostWorker / ViewSession backend.  Linux stays CLI-only
# until Ubuntu 24.04+ becomes the build floor (Qt 6.5+ in the apt repo).
if(WIN32 OR APPLE)
    target_sources(vivora PRIVATE
        gui/gui_main.cpp
        gui/app_controller.cpp
        gui/host_worker.cpp
        gui/view_session.cpp
        gui/settings.cpp
        gui/address_book.cpp
        gui/tray.cpp
        gui/app_icon.cpp
        ${CMAKE_SOURCE_DIR}/qml/qml.qrc
    )
endif()

target_link_libraries(vivora PRIVATE
    vivora_common
    Qt6::Core Qt6::Widgets
)
if(WIN32 OR APPLE)
    target_link_libraries(vivora PRIVATE
        Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
endif()

# Static Qt: every QML plugin is a separate static library.  Q_IMPORT_PLUGIN
# (in gui_main.cpp) anchors the metadata symbol but the plugin .lib itself
# also has to be on the link line.  qt_import_qml_plugins() only works when
# the QML is registered via qt_add_qml_module(); we use a plain .qrc so we
# list the libs explicitly.  Paths come from the prebuilt Qt 6.9 install.
if(WIN32)
    set(_VIVORA_QT_QML_DIR  "C:/qt6/build/qtbase/qml")
    set(_VIVORA_QT_LIB_DIR  "C:/qt6/build/qtbase/lib")
    set(_VIVORA_QT_DECL_DIR "C:/qt6/build/qtdeclarative/src")
    set(_VIVORA_QT_BASE_DIR "C:/qt6/build/qtbase/src")
    # Static QML resources (.qmltypes, the .qml files of the Basic style,
    # SG shaders, etc.) are compiled into individual qrc_*_init.cpp.obj
    # files that aren't packed into any .lib — qmake/CMake adds them
    # straight to the executable's link line.  We have to do the same
    # manually since we don't use qt_add_qml_module.
    file(GLOB _DB_SG_SHADER_OBJS
        "${_VIVORA_QT_DECL_DIR}/quick/CMakeFiles/Quick_resources_*.dir/Release/.qt/rcc/qrc_*_init.cpp.obj")
    set(_VIVORA_QML_RES_OBJS
        ${_DB_SG_SHADER_OBJS}
        "${_VIVORA_QT_DECL_DIR}/qml/CMakeFiles/Qml_resources_1.dir/Release/.qt/rcc/qrc_qmake_QML_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/qmlmeta/CMakeFiles/QmlMeta_resources_1.dir/Release/.qt/rcc/qrc_qmake_QtQml_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/qmlmodels/CMakeFiles/QmlModels_resources_1.dir/Release/.qt/rcc/qrc_qmake_QtQml_Models_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/qmlworkerscript/CMakeFiles/QmlWorkerScript_resources_1.dir/Release/.qt/rcc/qrc_qmake_QtQml_WorkerScript_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quick/window/CMakeFiles/quickwindow_resources_1.dir/Release/.qt/rcc/qrc_qmake_QtQuick_Window_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quicklayouts/CMakeFiles/QuickLayouts_resources_1.dir/Release/.qt/rcc/qrc_qmake_QtQuick_Layouts_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quicktemplates/CMakeFiles/QuickTemplates2_resources_1.dir/Release/.qt/rcc/qrc_qmake_QtQuick_Templates_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quickcontrols/CMakeFiles/QuickControls2_resources_1.dir/Release/.qt/rcc/qrc_qmake_QtQuick_Controls_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quickcontrols/CMakeFiles/qtquickcontrols2plugin_resources_1.dir/Release/.qt/rcc/qrc_indirectBasic_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quickcontrolsimpl/CMakeFiles/QuickControls2Impl_resources_1.dir/Release/.qt/rcc/qrc_qmake_QtQuick_Controls_impl_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quickcontrols/basic/CMakeFiles/QuickControls2Basic_resources_1.dir/Release/.qt/rcc/qrc_qmake_QtQuick_Controls_Basic_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quickcontrols/basic/CMakeFiles/QuickControls2Basic_resources_2.dir/Release/.qt/rcc/qrc_QuickControls2Basic_raw_qml_0_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quickcontrols/basic/CMakeFiles/QuickControls2Basic_resources_3.dir/Release/.qt/rcc/qrc_qtquickcontrols2basicstyle_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quickcontrols/basic/impl/CMakeFiles/QuickControls2BasicStyleImpl_resources_1.dir/Release/.qt/rcc/qrc_qmake_QtQuick_Controls_Basic_impl_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quickdialogs/quickdialogs/CMakeFiles/QuickDialogs2_resources_1.dir/Release/.qt/rcc/qrc_qmake_QtQuick_Dialogs_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quickdialogs/quickdialogsquickimpl/CMakeFiles/QuickDialogs2QuickImpl_resources_1.dir/Release/.qt/rcc/qrc_qmake_QtQuick_Dialogs_quickimpl_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quickdialogs/quickdialogsquickimpl/CMakeFiles/QuickDialogs2QuickImpl_resources_2.dir/Release/.qt/rcc/qrc_QuickDialogs2QuickImpl_raw_qml_0_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quickdialogs/quickdialogsquickimpl/CMakeFiles/QuickDialogs2QuickImpl_resources_3.dir/Release/.qt/rcc/qrc_QuickDialogs2QuickImpl_init.cpp.obj"
        "${_VIVORA_QT_DECL_DIR}/quickdialogs/quickdialogsquickimpl/CMakeFiles/QuickDialogs2QuickImpl_resources_4.dir/Release/.qt/rcc/qrc_QuickDialogs2QuickImplShaders_init.cpp.obj"
        "${_VIVORA_QT_BASE_DIR}/gui/CMakeFiles/Gui_resources_1.dir/Release/.qt/rcc/qrc_qpdf_init.cpp.obj"
        "${_VIVORA_QT_BASE_DIR}/gui/CMakeFiles/Gui_resources_2.dir/Release/.qt/rcc/qrc_gui_shaders_init.cpp.obj"
    )
    # Each QML plugin .lib is just metadata + the Q_PLUGIN_METADATA hook;
    # the actual qml_register_types_X / qInitResources_X / theme/init
    # symbols live in companion non-plugin libs (Qt6QuickLayouts.lib etc.).
    # Both halves must be on the link line.
    target_link_libraries(vivora PRIVATE
        "${_VIVORA_QT_QML_DIR}/QtQml/qmlplugin.lib"
        "${_VIVORA_QT_QML_DIR}/QtQml/Models/modelsplugin.lib"
        "${_VIVORA_QT_QML_DIR}/QtQml/WorkerScript/workerscriptplugin.lib"
        "${_VIVORA_QT_QML_DIR}/QtQuick/qtquick2plugin.lib"
        "${_VIVORA_QT_QML_DIR}/QtQuick/Window/quickwindowplugin.lib"
        "${_VIVORA_QT_QML_DIR}/QtQuick/Layouts/qquicklayoutsplugin.lib"
        "${_VIVORA_QT_QML_DIR}/QtQuick/Templates/qtquicktemplates2plugin.lib"
        "${_VIVORA_QT_QML_DIR}/QtQuick/Controls/qtquickcontrols2plugin.lib"
        "${_VIVORA_QT_QML_DIR}/QtQuick/Controls/impl/qtquickcontrols2implplugin.lib"
        "${_VIVORA_QT_QML_DIR}/QtQuick/Controls/Basic/qtquickcontrols2basicstyleplugin.lib"
        "${_VIVORA_QT_QML_DIR}/QtQuick/Controls/Basic/impl/qtquickcontrols2basicstyleimplplugin.lib"
        "${_VIVORA_QT_QML_DIR}/QtQuick/Dialogs/qtquickdialogsplugin.lib"
        "${_VIVORA_QT_QML_DIR}/QtQuick/Dialogs/quickimpl/qtquickdialogs2quickimplplugin.lib"

        "${_VIVORA_QT_LIB_DIR}/Qt6QmlModels.lib"
        "${_VIVORA_QT_LIB_DIR}/Qt6QmlWorkerScript.lib"
        "${_VIVORA_QT_LIB_DIR}/Qt6QuickLayouts.lib"
        "${_VIVORA_QT_LIB_DIR}/Qt6QuickTemplates2.lib"
        "${_VIVORA_QT_LIB_DIR}/Qt6QuickControls2.lib"
        "${_VIVORA_QT_LIB_DIR}/Qt6QuickControls2Impl.lib"
        "${_VIVORA_QT_LIB_DIR}/Qt6QuickControls2Basic.lib"
        "${_VIVORA_QT_LIB_DIR}/Qt6QuickControls2BasicStyleImpl.lib"
        "${_VIVORA_QT_LIB_DIR}/Qt6QuickDialogs2.lib"
        "${_VIVORA_QT_LIB_DIR}/Qt6QuickDialogs2QuickImpl.lib"
        "${_VIVORA_QT_LIB_DIR}/Qt6QuickDialogs2Utils.lib"
        ${_VIVORA_QML_RES_OBJS}
    )
endif()

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
    set(_VIVORA_OPENSSL_DIR "C:/OpenSSL-WinUniversal/lib/VC/x64/MT")
    if(EXISTS "${_VIVORA_OPENSSL_DIR}/libssl_static.lib")
        target_link_libraries(vivora PRIVATE
            "${_VIVORA_OPENSSL_DIR}/libssl_static.lib"
            "${_VIVORA_OPENSSL_DIR}/libcrypto_static.lib"
            crypt32
        )
        target_link_options(vivora PRIVATE "/FORCE:MULTIPLE" "/IGNORE:4006" "/IGNORE:4088")
    else()
        message(WARNING "OpenSSL static libs not found at ${_VIVORA_OPENSSL_DIR}")
    endif()

    # Qt6's openssl TLS plugin tries to dlopen OpenSSL 1.1 at runtime
    # (legacy hardcoded import).  Copy the matching DLLs next to the
    # exe so QApplication startup doesn't blow up.  Long-term we want to
    # rebuild Qt6 without the openssl TLS plugin (Schannel-only) and
    # drop these copies entirely.
    set(_VIVORA_OPENSSL11_BIN "C:/OpenSSL/bin")
    foreach(_dll libcrypto-1_1-x64.dll libssl-1_1-x64.dll)
        if(EXISTS "${_VIVORA_OPENSSL11_BIN}/${_dll}")
            add_custom_command(TARGET vivora POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_VIVORA_OPENSSL11_BIN}/${_dll}"
                        "$<TARGET_FILE_DIR:vivora>/${_dll}")
        endif()
    endforeach()
endif()

# Client networking (cross-platform)
target_sources(vivora PRIVATE
    ${CMAKE_SOURCE_DIR}/src/client/net/video_receiver.cpp
    ${CMAKE_SOURCE_DIR}/src/client/net/client_session.cpp
)
# Host session + sender are cross-platform (pure C++).
target_sources(vivora PRIVATE
    ${CMAKE_SOURCE_DIR}/src/host/session/host_session.cpp
    ${CMAKE_SOURCE_DIR}/src/host/session/video_sender.cpp
    ${CMAKE_SOURCE_DIR}/src/host/audio/audio_sender.cpp
    ${CMAKE_SOURCE_DIR}/src/client/audio/audio_receiver.cpp
)
target_include_directories(vivora PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

if(APPLE)
    # macOS platform implementations (Objective-C++)
    target_sources(vivora PRIVATE
        mac_host_platform.mm
        mac_view_platform.mm
    )
    # macOS client: Cocoa window + AVSampleBufferDisplayLayer
    target_sources(vivora PRIVATE
        ${CMAKE_SOURCE_DIR}/src/client/render/mac_video_view.mm
    )
    # macOS host: ScreenCaptureKit + VideoToolbox + CGEvent input injection
    target_sources(vivora PRIVATE
        ${CMAKE_SOURCE_DIR}/src/host/capture/mac_screen_capture.mm
        ${CMAKE_SOURCE_DIR}/src/host/encode/mac_videotoolbox_encoder.mm
        ${CMAKE_SOURCE_DIR}/src/host/input/input_injector_mac.mm
    )
    target_link_libraries(vivora PRIVATE
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

    target_sources(vivora PRIVATE
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
    target_link_libraries(vivora PRIVATE
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        PkgConfig::LIBAV
        PkgConfig::PIPEWIRE
        PkgConfig::DBUS
    )
endif()

if(WIN32)
    # Windows platform implementations
    target_sources(vivora PRIVATE
        windows_host_platform.cpp
        windows_view_platform.cpp
    )
    # Host-side sources (Windows DXGI/AMF/NVENC/QSV + SendInput)
    target_sources(vivora PRIVATE
        ${CMAKE_SOURCE_DIR}/src/host/capture/dxgi_capture.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/amf_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/nvenc_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/qsv_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/encoder_factory.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/fallback_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/input/input_injector_windows.cpp
    )
    # Client decode/render (D3D11 — Windows-only)
    target_sources(vivora PRIVATE
        ${CMAKE_SOURCE_DIR}/src/client/decode/mf_decoder.cpp
        ${CMAKE_SOURCE_DIR}/src/client/render/d3d_renderer.cpp
        ${CMAKE_SOURCE_DIR}/src/client/render/stream_window.cpp
    )
    target_include_directories(vivora PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/amf
        ${CMAKE_SOURCE_DIR}/third_party/nvenc
        ${CMAKE_SOURCE_DIR}/third_party/onevpl/api
    )
    target_link_libraries(vivora PRIVATE
        d3d11
        dxgi
    )
endif()
