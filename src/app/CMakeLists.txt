if(UNIX AND NOT APPLE)
    # Linux GUI (VIV-5): the Qt Quick stack is now required here too.  Ubuntu's
    # apt Qt (6.2 base) lacks qt6-declarative-dev, so point CMAKE_PREFIX_PATH at
    # a Qt 6.5+ build with Quick (e.g. a from-source /opt|~/qt6).  OpenGL(Widgets)
    # stay for the QtGlVideoView stream renderer.
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Qml Quick QuickControls2 Network OpenGL OpenGLWidgets)
else()
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets Qml Quick QuickControls2 Network)
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
        MACOSX_BUNDLE_ICON_FILE "Vivora"
    )
    # Generate Vivora.icns from the source iconset and drop it into the
    # bundle's Contents/Resources/ so Finder, Dock, and the About panel
    # all pick up the brand glyph.  Info.plist already references it via
    # CFBundleIconFile=Vivora.  Rebuilt every time the iconset PNGs
    # change.
    set(_VIVORA_ICONSET ${CMAKE_SOURCE_DIR}/icons/mac/Vivora.iconset)
    set(_VIVORA_ICNS    ${CMAKE_CURRENT_BINARY_DIR}/Vivora.icns)
    file(GLOB _VIVORA_ICONSET_PNGS ${_VIVORA_ICONSET}/*.png)
    add_custom_command(OUTPUT ${_VIVORA_ICNS}
        COMMAND iconutil --convert icns --output ${_VIVORA_ICNS} ${_VIVORA_ICONSET}
        DEPENDS ${_VIVORA_ICONSET_PNGS}
        COMMENT "Generating Vivora.icns from iconset")
    target_sources(vivora PRIVATE ${_VIVORA_ICNS})
    set_source_files_properties(${_VIVORA_ICNS} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    # Codesign identity for the bundle.  Two modes:
    #   "-"        : ad-hoc (no cert).  Easy, but the designated requirement
    #                is the cdhash, which changes every build — so TCC grants
    #                (Screen Recording / Accessibility) DON'T survive rebuilds
    #                and macOS re-prompts (worse on macOS 15+/26: it can even
    #                loop on relaunch).  We reset TCC each build so the next
    #                launch at least re-grants cleanly.
    #   "<name>"   : a code-signing certificate (e.g. a one-time self-signed
    #                "Vivora Dev" in the login keychain).  Its DR is
    #                identifier+cert — STABLE across rebuilds and relaunches —
    #                so grants persist and no reset is needed.  No Apple
    #                Developer account required; a real Developer ID swaps in
    #                here for distribution (VIV-14).
    # Create the self-signed cert once (see docs/macos-codesign or VIV-14),
    # then configure with -DVIVORA_MAC_CODESIGN_IDENTITY="Vivora Dev".
    set(VIVORA_MAC_CODESIGN_IDENTITY "-" CACHE STRING
        "codesign identity for Vivora.app (- = ad-hoc; a keychain cert name = stable TCC)")
    add_custom_command(TARGET vivora POST_BUILD
        COMMAND codesign --force --sign "${VIVORA_MAC_CODESIGN_IDENTITY}" --identifier dev.vivora.app
                $<TARGET_BUNDLE_DIR:vivora>
        COMMENT "Codesigning Vivora.app (identity: ${VIVORA_MAC_CODESIGN_IDENTITY})")
    if(VIVORA_MAC_CODESIGN_IDENTITY STREQUAL "-")
        # Ad-hoc only: cdhash changed → the old grant is dead.  Reset so the
        # next launch re-prompts and registers.  A stable cert needs no reset.
        add_custom_command(TARGET vivora POST_BUILD
            COMMAND tccutil reset ScreenCapture dev.vivora.app || true
            COMMAND tccutil reset Accessibility dev.vivora.app || true
            COMMENT "Ad-hoc build: resetting TCC grants (set VIVORA_MAC_CODESIGN_IDENTITY for stable grants)")
    endif()
endif()

# GUI sources land on all three desktop platforms — same Qt 6 + QML +
# AppController / HostWorker / ViewSession backend.  (Linux wired in VIV-5;
# needs a Qt 6.5+ build with Quick on CMAKE_PREFIX_PATH.)
if(WIN32 OR APPLE OR (UNIX AND NOT APPLE))
    target_sources(vivora PRIVATE
        gui/gui_main.cpp
        gui/app_controller.cpp
        gui/host_worker.cpp
        gui/network_change_watcher.cpp
        gui/view_session.cpp
        gui/settings.cpp
        gui/address_book.cpp
        gui/cloud_client.cpp
        gui/update_checker.cpp
        gui/announcements_client.cpp
        gui/polls_client.cpp
        gui/tray.cpp
        gui/app_icon.cpp
        ${CMAKE_SOURCE_DIR}/qml/qml.qrc
    )
    if(WIN32)
        # Start-at-login backend (VIV-18): HKCU Run key.  Non-Windows builds
        # use the inline no-op stubs in gui/autostart.h until the macOS /
        # Linux backends land (separate issue).
        target_sources(vivora PRIVATE gui/autostart_win.cpp)
    endif()
endif()

target_link_libraries(vivora PRIVATE
    vivora_common
    Qt6::Core Qt6::Widgets
)
if(WIN32 OR APPLE OR (UNIX AND NOT APPLE))
    target_link_libraries(vivora PRIVATE
        Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Network
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

    # Native Windows TLS backend (Schannel) for QNetworkAccessManager.  Static
    # Qt registers no TLS backend unless its plugin lib is linked AND imported
    # (Q_IMPORT_PLUGIN(QSchannelBackendPlugin) in gui_main.cpp); without one the
    # first HTTPS request (CloudClient -> https://cloud.vivora.dev) crashes.
    # Schannel needs no OpenSSL — just the Windows security libs below.
    target_link_libraries(vivora PRIVATE
        "${_VIVORA_QT_LIB_DIR}/../plugins/tls/qschannelbackend.lib"
        secur32 crypt32 bcrypt ncrypt ws2_32
    )

    # QNetworkInformation backend (Network List Manager) — VIV-57 network-
    # change rendezvous refresh.  Same static-plugin rule as Schannel above:
    # the .lib must be on the link line AND the plugin imported in
    # gui_main.cpp (Q_IMPORT_PLUGIN(QNetworkListManagerNetworkInformation-
    # BackendFactory)).
    target_link_libraries(vivora PRIVATE
        "${_VIVORA_QT_LIB_DIR}/../plugins/networkinformation/qnetworklistmanager.lib"
    )

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
    ${CMAKE_SOURCE_DIR}/src/host/session/paced_sender.cpp
    ${CMAKE_SOURCE_DIR}/src/host/audio/audio_sender.cpp
    ${CMAKE_SOURCE_DIR}/src/client/audio/audio_receiver.cpp
    # In-stream control menu (VIV-74) — pure Qt Widgets, hosted by the
    # per-platform stream view (Windows StreamWindow, Linux QtGlVideoView,
    # macOS MacVideoView).
    ${CMAKE_SOURCE_DIR}/src/client/render/stream_menu.cpp
    # "Switch monitor…" panel (VIV-50) — pure Qt Widgets, same hosting.
    ${CMAKE_SOURCE_DIR}/src/client/render/monitor_panel.cpp
)
target_include_directories(vivora PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

if(APPLE)
    # macOS platform implementations (Objective-C++)
    target_sources(vivora PRIVATE
        mac_host_platform.mm
        mac_view_platform.mm
        mac_video_pipeline.cpp
        # VIV-57: NSWorkspaceDidWake observer feeding NetworkChangeWatcher
        # (wake often re-associates WiFi without a reachability change).
        gui/network_change_watcher_mac.mm
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
    # wl_output enumeration for the VIV-50 monitor list (the portal doesn't
    # expose displays programmatically).  Optional: on a non-Wayland/X11 host
    # the helper simply returns an empty list and we advertise one display.
    pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)

    target_sources(vivora PRIVATE
        linux_view_platform.cpp
        linux_video_pipeline.cpp
        linux_host_platform.cpp
        ${CMAKE_SOURCE_DIR}/src/client/decode/ffmpeg_decoder.cpp
        ${CMAKE_SOURCE_DIR}/src/client/render/qt_gl_video_view.cpp
        # Linux host: PipeWire capture + VAAPI encoder + uinput injector,
        # wired into HostSession via LinuxHostPlatform.  Audio capture
        # via PulseAudio monitor is pending (L3 sub-task).
        ${CMAKE_SOURCE_DIR}/src/host/capture/pipewire_capture.cpp
        ${CMAKE_SOURCE_DIR}/src/host/capture/x11_cursor.cpp
        ${CMAKE_SOURCE_DIR}/src/host/capture/wayland_outputs.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/vaapi_encoder.cpp
        # Native NVENC for NVIDIA hosts (VIV-8): CUDA-session encoder behind
        # the ILinuxEncoder interface, selected over VAAPI by the factory.
        # The driver libs (libcuda / libnvidia-encode) are dlopen'd at
        # runtime, so this links and runs on non-NVIDIA machines too.
        ${CMAKE_SOURCE_DIR}/src/host/encode/linux_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/encode/nvenc_linux_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/host/input/input_injector_linux.cpp
    )
    target_include_directories(vivora PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/nvenc
    )
    target_link_libraries(vivora PRIVATE
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        PkgConfig::LIBAV
        PkgConfig::PIPEWIRE
        PkgConfig::DBUS
        ${CMAKE_DL_LIBS}
    )
    if(WAYLAND_CLIENT_FOUND)
        target_link_libraries(vivora PRIVATE PkgConfig::WAYLAND_CLIENT)
        target_compile_definitions(vivora PRIVATE VIVORA_HAVE_WAYLAND_CLIENT)
    endif()
endif()

if(WIN32)
    # Windows platform implementations
    target_sources(vivora PRIVATE
        windows_host_platform.cpp
        windows_view_platform.cpp
        windows_video_pipeline.cpp
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
