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

    # Application icon + VERSIONINFO.  Without these the exe carried the
    # default MSVC icon everywhere Windows shows one -- taskbar, Explorer,
    # Alt-Tab, the installer's Add/Remove Programs entry -- and Properties ->
    # Details was blank.
    set(_VIVORA_ICO "${CMAKE_SOURCE_DIR}/icons/win/vivora.ico")
    if(EXISTS "${_VIVORA_ICO}")
        # rc.exe treats backslash as an escape inside string literals, so the
        # path has to be doubled rather than passed through as-is.
        string(REPLACE "/" "\\\\" VIVORA_ICO_PATH_RC "${_VIVORA_ICO}")
        configure_file(${CMAKE_SOURCE_DIR}/packaging/windows/vivora.rc.in
                       ${CMAKE_CURRENT_BINARY_DIR}/vivora.rc @ONLY)
        target_sources(vivora PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vivora.rc)
    else()
        message(WARNING "icons/win/vivora.ico missing - exe will have no icon "
                        "or version info.  Regenerate with scripts/make-ico.ps1")
    endif()

    # Application manifest, handed to CMake as a source file so it reaches the
    # linker as /MANIFESTINPUT and mt.exe merges it with the auto-generated
    # one.  Putting an RT_MANIFEST in the .rc instead collides with that
    # generated manifest and needs /MANIFEST:NO to work at all.
    configure_file(${CMAKE_SOURCE_DIR}/packaging/windows/vivora.exe.manifest.in
                   ${CMAKE_CURRENT_BINARY_DIR}/vivora.exe.manifest @ONLY)
    target_sources(vivora PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vivora.exe.manifest)
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
    #                is the cdhash, which changes every build - so TCC grants
    #                (Screen Recording / Accessibility) do NOT survive
    #                rebuilds and macOS re-prompts.
    #   "<name>"   : a code-signing certificate (e.g. a one-time self-signed
    #                "Vivora Dev" in the login keychain).  Its designated
    #                requirement is identifier+cert, stable across rebuilds.
    #                No Apple Developer account required; a real Developer ID
    #                swaps in here for distribution (VIV-14).
    # Create the self-signed cert once (see docs/macos-codesign.md), then
    # configure with -DVIVORA_MAC_CODESIGN_IDENTITY="Vivora Dev".
    #
    # The signing runs through a script rather than inline, because a
    # certificate needs an unlocked keychain and an SSH session never has one:
    # codesign fails with errSecInternalComponent, the build stops after the
    # link, and what is left behind still runs on the linker's own ad-hoc
    # signature -- under the wrong signing identifier, which is what TCC keys
    # grants on.  The script falls back to ad-hoc with a loud explanation
    # instead, and resets the grants either way.
    set(VIVORA_MAC_CODESIGN_IDENTITY "-" CACHE STRING
        "codesign identity for Vivora.app (- = ad-hoc; a keychain cert name = stable TCC)")
    add_custom_command(TARGET vivora POST_BUILD
        COMMAND bash "${CMAKE_SOURCE_DIR}/packaging/macos/codesign-app.sh"
                "${VIVORA_MAC_CODESIGN_IDENTITY}"
                $<TARGET_BUNDLE_DIR:vivora>
        COMMENT "Codesigning Vivora.app (identity: ${VIVORA_MAC_CODESIGN_IDENTITY})")
endif()

# GUI sources land on all three desktop platforms — same Qt 6 + QML +
# AppController / HostWorker / ViewSession backend.  (Linux wired in VIV-5;
# needs a Qt 6.5+ build with Quick on CMAKE_PREFIX_PATH.)
if(WIN32 OR APPLE OR (UNIX AND NOT APPLE))
    target_sources(vivora PRIVATE
        gui/gui_main.cpp
        gui/app_controller.cpp
        gui/clipboard_sync.cpp
        gui/host_worker.cpp
        gui/network_change_watcher.cpp
        gui/view_session.cpp
        gui/settings.cpp
        gui/address_book.cpp
        gui/cloud_client.cpp
        gui/device_mesh_model.cpp
        gui/update_checker.cpp
        gui/announcements_client.cpp
        gui/polls_client.cpp
        gui/tray.cpp
        gui/app_icon.cpp
        # App icon, tray icons and the bundled fonts.  Kept as a .qrc: the
        # module treatment in VIV-121 is about QML registration and has
        # nothing to say about images and fonts.
        ${CMAKE_SOURCE_DIR}/qml/assets.qrc
    )
    # Start-at-login backend (VIV-18): HKCU Run key on Windows, an XDG
    # autostart .desktop entry on Linux.  macOS still uses the inline no-op
    # stubs in gui/autostart.h.
    if(WIN32)
        target_sources(vivora PRIVATE gui/autostart_win.cpp)
    elseif(UNIX AND NOT APPLE)
        target_sources(vivora PRIVATE gui/autostart_linux.cpp)
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

# The QML lives in a proper module (VIV-121).
#
# It used to be a plain qml.qrc, and that one choice is what forced the Windows
# build to reach into a Qt *build tree*: qt_import_qml_plugins() only works for
# qt_add_qml_module() targets, so with a .qrc the fourteen QML plugin .libs,
# their twelve companion libs and about twenty-five raw qrc_*_init .obj files
# had to be listed by absolute path -- files that exist only where Qt was
# compiled, never in a Qt installation.  That is why Windows had no CI job.
#
# qt_add_qml_module() registers the same QML as a module named Vivora, and
# qt_import_qml_plugins() then works out which static QML plugins the module
# actually needs and links them itself.
set(VIVORA_QML_FILES
        ${CMAKE_SOURCE_DIR}/qml/Main.qml
        ${CMAKE_SOURCE_DIR}/qml/AddPeerDialog.qml
        ${CMAKE_SOURCE_DIR}/qml/AddressBookView.qml
        ${CMAKE_SOURCE_DIR}/qml/ConnectionApprovalDialog.qml
        ${CMAKE_SOURCE_DIR}/qml/DeviceAvatar.qml
        ${CMAKE_SOURCE_DIR}/qml/DeviceMockModel.qml
        ${CMAKE_SOURCE_DIR}/qml/DevicePalette.qml
        ${CMAKE_SOURCE_DIR}/qml/DeviceRow.qml
        ${CMAKE_SOURCE_DIR}/qml/DeviceRowMenu.qml
        ${CMAKE_SOURCE_DIR}/qml/Glyph.qml
        ${CMAKE_SOURCE_DIR}/qml/MyDevicesBlock.qml
        ${CMAKE_SOURCE_DIR}/qml/MyDevicesSettings.qml
        ${CMAKE_SOURCE_DIR}/qml/ProGate.qml
        ${CMAKE_SOURCE_DIR}/qml/RelayProDialog.qml
        ${CMAKE_SOURCE_DIR}/qml/RemoveDeviceDialog.qml
        ${CMAKE_SOURCE_DIR}/qml/ScreenRecordingPermissionDialog.qml
        ${CMAKE_SOURCE_DIR}/qml/SettingsDeviceRow.qml
        ${CMAKE_SOURCE_DIR}/qml/SettingsDialog.qml
        ${CMAKE_SOURCE_DIR}/qml/TierBadge.qml
        ${CMAKE_SOURCE_DIR}/qml/TrustPromptDialog.qml
)

# qml/ sits above this directory, so every entry is an absolute path; Qt needs
# an explicit alias for those or it refuses to place them in the resource tree.
foreach(_qml IN LISTS VIVORA_QML_FILES)
    get_filename_component(_qml_name "${_qml}" NAME)
    set_source_files_properties("${_qml}" PROPERTIES QT_RESOURCE_ALIAS "${_qml_name}")
endforeach()

qt_add_qml_module(vivora
    URI Vivora
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    QML_FILES ${VIVORA_QML_FILES}
)
# Plain add_executable() means Qt never finalises the target for us, so the
# step that scans the QML for imports and emits the Q_IMPORT_PLUGIN glue has to
# be asked for by name.  Without it a static build links but dies at startup
# with `module "QtQuick" plugin "qtquick2plugin" not found`.  On a dynamic Qt
# (macOS, Linux) it is a no-op: the loader finds plugins on disk.
qt_import_qml_plugins(vivora)

# One translation unit inside Qt6Network -- qpassworddigestor.cpp, reached via
# QAuthenticator -- calls the OpenSSL 3 KDF API (EVP_KDF_*, OSSL_PARAM_*).
# Nothing we ship uses PBKDF2, but the symbols have to resolve, so link OpenSSL
# 3 statically: it costs a few hundred KB in the exe and no DLL on disk.
#
# This used to need /FORCE:MULTIPLE, because Qt also dragged in the OpenSSL 1.1
# import libs through the openssl TLS plugin and the two definitions collided.
# With that plugin excluded (VIV-122) there is one definition of everything and
# the link is clean -- which also retires the LNK4088 "image may not run"
# warning the forced link was producing on every build.
if(WIN32)
    set(VIVORA_OPENSSL3_STATIC_DIR "C:/OpenSSL-WinUniversal/lib/VC/x64/MT" CACHE PATH
        "Directory holding libssl_static.lib / libcrypto_static.lib (OpenSSL 3, /MT)")
    if(EXISTS "${VIVORA_OPENSSL3_STATIC_DIR}/libcrypto_static.lib")
        target_link_libraries(vivora PRIVATE
            "${VIVORA_OPENSSL3_STATIC_DIR}/libcrypto_static.lib"
            crypt32
        )
    else()
        message(FATAL_ERROR
            "libcrypto_static.lib not found under ${VIVORA_OPENSSL3_STATIC_DIR}.
"
            "Qt6Network references the OpenSSL 3 KDF API and will not link "
            "without it; point VIVORA_OPENSSL3_STATIC_DIR at an OpenSSL 3 /MT "
            "static build.")
    endif()
    # Native Windows TLS backend (Schannel) for QNetworkAccessManager.  Static
    # Qt registers no TLS backend unless its plugin lib is linked AND imported
    # (Q_IMPORT_PLUGIN(QSchannelBackendPlugin) in gui_main.cpp); without one the
    # first HTTPS request (CloudClient -> https://cloud.vivora.dev) crashes.
    # Schannel needs no OpenSSL — just the Windows security libs below.
    target_link_libraries(vivora PRIVATE secur32 crypt32 bcrypt ncrypt ws2_32)
    if(NOT QT6_IS_SHARED_LIBS_BUILD)
        target_link_libraries(vivora PRIVATE Qt6::QSchannelBackendPlugin)
        target_compile_definitions(vivora PRIVATE VIVORA_QT_STATIC)
    endif()

    # QNetworkInformation backend (Network List Manager) — VIV-57 network-
    # change rendezvous refresh.  Same static-plugin rule as Schannel above:
    # the .lib must be on the link line AND the plugin imported in
    # gui_main.cpp (Q_IMPORT_PLUGIN(QNetworkListManagerNetworkInformation-
    # BackendFactory)).
    if(NOT QT6_IS_SHARED_LIBS_BUILD)
        target_link_libraries(vivora PRIVATE Qt6::QNLMNIPlugin)
    endif()

    # Do not let Qt auto-import the OpenSSL TLS backend (VIV-122).
    #
    # With a static Qt, CMake adds an initializer object for every plugin of a
    # type the target uses, which pulled in qopensslbackend.lib and with it an
    # implicit dependency on libcrypto-1_1-x64.dll / libssl-1_1-x64.dll -- two
    # DLLs that reached end of life in September 2023 and were being shipped
    # beside a remote-access product.  Nothing called them: TLS goes through
    # Schannel, imported by hand just above, and the media path is Monocypher.
    # They were there only because the exe would not start without them.
    #
    # EXCLUDE_BY_TYPE drops the whole tls category, Schannel included; the
    # explicit qschannelbackend.lib link plus Q_IMPORT_PLUGIN(QSchannelBackend)
    # in gui_main.cpp is what actually registers a backend, and that is
    # unaffected.
    qt_import_plugins(vivora EXCLUDE_BY_TYPE tls)
endif()

# Client networking (cross-platform)
target_sources(vivora PRIVATE
    ${CMAKE_SOURCE_DIR}/src/client/net/video_receiver.cpp
    ${CMAKE_SOURCE_DIR}/src/client/net/client_session.cpp
    # Decoder-capability probe (VIV-112).  Platform bodies are #ifdef'd;
    # links against MF on Windows / libavcodec on Linux (both already on the
    # vivora target below), a plain return on macOS.
    ${CMAKE_SOURCE_DIR}/src/client/decode/codec_caps.cpp
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
    # Headers only -- libpipewire is resolved at runtime (VIV-126), so that a
    # machine that only ever views a remote screen does not need it installed
    # in order to start the binary at all.
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
        ${CMAKE_SOURCE_DIR}/src/host/capture/pipewire_dyn.cpp
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
        ${PIPEWIRE_INCLUDE_DIRS}
    )
    target_link_libraries(vivora PRIVATE
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        PkgConfig::LIBAV
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

# ── Install rules ────────────────────────────────────────────────────────────
#
# These exist so packaging is `cmake --install` into a staging tree rather than
# a script copying files it hopes are the right ones.  The AppDir an AppImage
# is built from is exactly this layout, which is why the Linux side follows the
# FHS names precisely.

if(WIN32)
    # One self-contained exe.  The two OpenSSL 1.1 DLLs that used to ship
    # beside it are gone with the openssl TLS plugin (VIV-122).
    install(TARGETS vivora RUNTIME DESTINATION . COMPONENT app)
    install(FILES ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION . COMPONENT app)

elseif(UNIX AND NOT APPLE)
    # The desktop file is renamed to the reverse-DNS app id because the
    # metainfo's <launchable> names it that way, and AppStream validates the
    # pair.
    configure_file(${CMAKE_SOURCE_DIR}/packaging/linux/vivora.desktop.in
                   ${CMAKE_CURRENT_BINARY_DIR}/dev.vivora.app.desktop @ONLY)

    install(TARGETS vivora RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT app)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dev.vivora.app.desktop
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications COMPONENT app)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/dev.vivora.app.metainfo.xml
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo COMPONENT app)
    foreach(_sz 16 32 48 64 128 256)
        install(FILES ${CMAKE_SOURCE_DIR}/icons/win/vivora-${_sz}.png
                DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${_sz}x${_sz}/apps
                RENAME dev.vivora.app.png COMPONENT app)
    endforeach()
    install(FILES ${CMAKE_SOURCE_DIR}/icons/vivora-mark.svg
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
            RENAME dev.vivora.app.svg COMPONENT app)
    # The udev rule is documentation, not something we can install ourselves:
    # /etc/udev/rules.d needs root and an AppImage has no installer step.
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/60-vivora-uinput.rules
            DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT app)
    install(FILES ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/README.md
            DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT app)
endif()
