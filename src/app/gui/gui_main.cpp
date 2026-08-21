// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/gui/gui_main.h"

#include "app/gui/address_book.h"
#include "app/gui/app_controller.h"
#include "app/gui/app_icon.h"
#include "app/gui/settings.h"
#include "app/gui/tray.h"

#include "common/utils/log.h"

#include <QApplication>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QSslSocket>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>
#include <QtPlugin>

#ifdef VIVORA_WINDOWS
#include "common/net/winsock_socket.h"
#endif
#ifdef VIVORA_MACOS
#include "app/gui/mac_activation.h"
#endif

// Static Qt: a plugin is a static library, and the ones the linker cannot
// discover on its own have to be anchored with Q_IMPORT_PLUGIN.
//
// The QML plugins used to be listed here by hand. They are not any more:
// qt_add_qml_module() plus qt_import_qml_plugins() works out which QML modules
// this app imports and generates the initializers for exactly those (VIV-121).
// Adding an import to a .qml file no longer means editing this file, and
// nothing here is tied to a particular Qt build layout.
//
// What remains are the two backends that are not QML at all, so nothing can
// infer them, and only when Qt itself is static -- with a shared Qt (macOS,
// Linux, and a Windows CI runner using the official binaries) the loader finds
// plugins on disk and these macros would be unresolved symbols.
#if defined(VIVORA_WINDOWS) && defined(VIVORA_QT_STATIC)
// TLS backend for QNetworkAccessManager (CloudClient talks to
// https://cloud.vivora.dev).  Static Qt registers no TLS backend unless its
// plugin is imported; without one the first HTTPS request crashes.  Schannel
// is the native Windows backend -- no OpenSSL dependency, which is what let
// the OpenSSL 1.1 DLLs leave the ship set (VIV-122).
Q_IMPORT_PLUGIN(QSchannelBackend)
// Network information backend (Network List Manager) for
// QNetworkInformation -- drives the VIV-57 rendezvous refresh on network
// change.  Same static-Qt rule as the TLS backend: no plugin imported,
// no backend registered (NetworkChangeWatcher then logs a warning and
// the host falls back to the 30 s keepalive only).
Q_IMPORT_PLUGIN(QNetworkListManagerNetworkInformationBackendFactory)
#endif

namespace vivora::gui {

// Single-instance enforcement.  QLockFile sits in a writable per-user
// location; the running instance also listens on a QLocalServer that
// the second-launch process pings to ask "please raise your window".
//
// Both keys are stable per user — no version suffix.  If the lock-
// file format changes in a future release we'll bump the basename.
namespace {

QString lock_file_path() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) {
        // Windows / older macOS don't define a runtime location; fall
        // back to AppLocalData which always exists.
        dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    }
    QDir().mkpath(dir);
    return dir + "/vivora.lock";
}

// QLocalServer / QLocalSocket map to:
//   Windows: named pipe \\.\pipe\<name>
//   POSIX:   Unix domain socket in $TMPDIR
// One name per user is fine — the lock file already serialises us.
constexpr const char* IPC_SERVER_NAME = "vivora-instance";
constexpr const char* IPC_RAISE_CMD   = "raise\n";

// Send Qt's own diagnostics to our log.
//
// Without this they go nowhere on Windows: the GUI links as a windowed
// subsystem, so there is no console for Qt to write to.  That hid exactly the
// class of failure the user cannot diagnose either -- a QML error leaves them
// with no window at all, and the log file they would send us said nothing
// about why.
void qt_message_to_log(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    const std::string text = msg.toStdString();
    switch (type) {
    case QtDebugMsg:    log::debug("Qt", "%s", text.c_str()); break;
    case QtInfoMsg:     log::info ("Qt", "%s", text.c_str()); break;
    case QtWarningMsg:  log::warn ("Qt", "%s", text.c_str()); break;
    case QtCriticalMsg:
    case QtFatalMsg:    log::error("Qt", "%s", text.c_str()); break;
    }
}

} // namespace

int run_gui(int argc, char** argv) {
    qInstallMessageHandler(qt_message_to_log);
    QApplication app(argc, argv);

#ifdef VIVORA_WINDOWS
    // Force the native Schannel TLS backend.  Since VIV-122 it is the only one
    // linked in, so this is belt and braces rather than a fix -- but it is the
    // line that would fail loudly if an OpenSSL backend ever crept back in,
    // which is worth keeping.  Must run before any QSslSocket is used.
    if (!QSslSocket::setActiveBackend(QStringLiteral("schannel")))
        log::warn("GUI", "could not select Schannel TLS backend");
#endif

    // Bundled fonts (VIV-5): register Inter (UI) + JetBrains Mono (codes /
    // fingerprints) so the GUI looks identical on every OS instead of falling
    // back to Segoe UI / SF Pro / system mono.  QML refers to them by family
    // name ("Inter", "JetBrains Mono"); the app default is set to Inter so
    // every unstyled Label inherits it.
    for (const char* f : {":/fonts/Inter-Regular.ttf",
                          ":/fonts/Inter-Medium.ttf",
                          ":/fonts/Inter-SemiBold.ttf",
                          ":/fonts/Inter-Bold.ttf",
                          ":/fonts/JetBrainsMono-Regular.ttf",
                          ":/fonts/JetBrainsMono-Bold.ttf"}) {
        if (QFontDatabase::addApplicationFont(QString::fromLatin1(f)) < 0)
            log::warn("GUI", "Failed to load bundled font %s", f);
    }
    {
        QFont ui("Inter");
        ui.setPixelSize(13);
        QApplication::setFont(ui);
    }

    // Settings has to know the org/app name to derive QStandardPaths
    // entries (incl. lock file path); set them before any Settings touch.
    // OrganizationDomain controls reverse-DNS path on macOS (~/Library/
    // Preferences/dev.vivora.Vivora.plist) and the QSettings registry
    // key on Windows; align with the bundle id prefix dev.vivora.*.
    QCoreApplication::setOrganizationName("Vivora");
    QCoreApplication::setOrganizationDomain("vivora.dev");
    QCoreApplication::setApplicationName("Vivora");

    // Send log output to a persistent file.  Windows GUI builds are
    // /SUBSYSTEM:WINDOWS so there's no attached console; macOS bundles
    // launched from Finder send stderr to a private launchd pipe.
    // Without a file we have no logs to look at when a user reports an
    // issue.  We can't just freopen(stderr) on Windows /SUBSYSTEM:WINDOWS
    // (stderr's underlying FD is invalid without a console — freopen
    // creates the file but subsequent fprintf silently fails).  Use the
    // log::set_file() API instead, which opens its own FILE* and writes
    // there directly.  Path: AppLocalDataLocation/vivora.log:
    //   Windows: %LOCALAPPDATA%\Vivora\Vivora\vivora.log
    //   macOS:   ~/Library/Application Support/Vivora/vivora.log
    //   Linux:   ~/.local/share/Vivora/Vivora/vivora.log
    {
        const QString dir = QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation);
        QDir().mkpath(dir);
        const QString path = dir + "/vivora.log";
        log::set_file(path.toUtf8().constData());
    }

#ifdef VIVORA_WINDOWS
    vivora::net::WinsockInit wsa;
    if (!wsa.ok) {
        log::error("GUI", "Failed to init Winsock");
        return 1;
    }
#endif

    // Single-instance check.  If another vivora.exe is already
    // running for this user, ping it via the local IPC server so it
    // raises its tray window, then exit cleanly.  Without this the
    // second launch would silently fight over the same QSettings,
    // host identity, and rendezvous registration.
    auto lock = std::make_unique<QLockFile>(lock_file_path());
    lock->setStaleLockTime(0);  // don't auto-remove — the IPC check below covers crash recovery
    if (!lock->tryLock(100)) {
        // Could be live or stale.  Try the IPC ping first; if it lands
        // we know someone is alive and we just hand off.
        QLocalSocket sock;
        sock.connectToServer(IPC_SERVER_NAME);
        if (sock.waitForConnected(500)) {
            sock.write(IPC_RAISE_CMD);
            sock.waitForBytesWritten(500);
            sock.disconnectFromServer();
            log::info("GUI", "Existing Vivora instance found — asked it to raise its window");
            return 0;
        }
        // No live owner — must be a stale lock from a crashed process.
        // Remove it and try once more.  removeStaleLockFile() checks
        // the recorded PID and only deletes when that process is gone,
        // so this is safe against a real concurrent launch.
        if (lock->removeStaleLockFile() && lock->tryLock(100)) {
            log::warn("GUI", "Removed stale instance lock and acquired ours");
        } else {
            log::error("GUI", "Another instance is holding the lock but not answering IPC — refusing to start");
            return 1;
        }
    }

    // A tray is how Vivora keeps its "always available" promise, but it is
    // not a hard requirement and refusing to start without one was a bad
    // trade: stock GNOME has no StatusNotifier host unless the AppIndicator
    // extension is installed, so on a default Fedora or a GNOME session
    // without extensions the app exited with code 1 and the user saw
    // absolutely nothing happen.  Run anyway; the window simply becomes the
    // only way to reach us, which means closing it has to quit.
    const bool tray_available = QSystemTrayIcon::isSystemTrayAvailable();
    if (!tray_available) {
        log::warn("GUI", "No system tray on this desktop — the main window is "
                         "the only way to reach Vivora, and closing it quits");
    }
    QApplication::setQuitOnLastWindowClosed(!tray_available);

    // Brand icon for taskbar / Alt-Tab / Explorer / window title bar.
    // QML windows pick it up via QGuiApplication::windowIcon by default
    // so nothing else needs wiring up.
    QApplication::setWindowIcon(make_app_icon());

    // Pin QtQuick.Controls to the Basic style.  By default it auto-loads
    // the platform-native style (QtQuick.Controls.Windows on Windows,
    // QtQuick.Controls.macOS on Mac, etc.), each of which is a separate
    // static plugin we'd otherwise have to link in.  Basic gets us a
    // portable look + only one plugin set.
    QQuickStyle::setStyle("Basic");

    AppController controller;
    controller.setTrayAvailable(tray_available);
    Tray          tray;
    controller.setTray(&tray);

    // Local-socket server: a second vivora.exe launch will connect
    // here and send "raise\n" to bring the existing window forward.
    // removeServer() clears a stale Unix socket / pipe handle left by
    // a previous crashed process before we try to bind.
    QLocalServer ipc_server;
    QLocalServer::removeServer(IPC_SERVER_NAME);
    if (!ipc_server.listen(IPC_SERVER_NAME)) {
        log::warn("GUI", "QLocalServer listen failed: %s — single-instance raise won't work",
                  ipc_server.errorString().toUtf8().constData());
    } else {
        QObject::connect(&ipc_server, &QLocalServer::newConnection,
                         &controller, [&ipc_server, &controller] {
            while (auto* client = ipc_server.nextPendingConnection()) {
                QObject::connect(client, &QLocalSocket::disconnected,
                                 client, &QLocalSocket::deleteLater);
                if (client->waitForReadyRead(200)) {
                    const QByteArray cmd = client->readAll().trimmed();
                    if (cmd == "raise") {
                        controller.showMainWindow();
                    }
                }
                client->disconnectFromServer();
            }
        });
    }

    QQmlApplicationEngine engine;
    // Expose the controller (and via its properties — Settings, AddressBook)
    // to QML as the singleton-like object `App`.  We don't bother with a
    // proper qmlRegisterSingletonInstance for one object; a context property
    // does the same thing with less ceremony.
    engine.rootContext()->setContextProperty("App", &controller);
    // Front-end-ahead-of-backend preview switch for the My Devices UI (VIV-52):
    // the device-mesh backend does not exist yet, so the isolated QML mock
    // reads this to render a given state.  Empty unless the env var is set.
    //   VIVORA_DEVICES_VARIANT = pro | trial | free | empty | cached
    engine.rootContext()->setContextProperty(
        "VivoraDevicesVariant",
        qEnvironmentVariable("VIVORA_DEVICES_VARIANT"));
    engine.loadFromModule("Vivora", "Main");
    if (engine.rootObjects().isEmpty()) {
        log::error("GUI", "Failed to load Vivora/Main.qml");
        return 1;
    }

#ifdef VIVORA_MACOS
    // Come to the front, unless this is the login-item launch.
    //
    // Vivora sets LSUIElement so it has no Dock icon, and macOS does not
    // raise a background launch on its own -- so the window opened behind
    // everything with nothing to click. That is how granting Screen Recording
    // looked from the outside: macOS quits and reopens the app itself, and
    // the app appeared to vanish.
    // Deferred to the first turn of the event loop: AppKit drops an
    // activation asked for before the loop is running.
    if (!launched_as_login_item())
        QTimer::singleShot(0, &app, [] { activate_app(); });
#endif

    log::info("GUI", "Vivora GUI ready");
    const int rc = app.exec();
    // lock released in destructor; explicit reset here keeps the order
    // obvious — AppController teardown happens first (stops host /
    // view workers), then we drop the lock so the next launch can grab it.
    lock.reset();
    return rc;
}

} // namespace vivora::gui
