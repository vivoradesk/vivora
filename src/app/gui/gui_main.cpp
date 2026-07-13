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
#include <QUrl>
#include <QtPlugin>

#ifdef VIVORA_WINDOWS
#include "common/net/winsock_socket.h"
#endif

// Static Qt: every QML plugin is a static library and must be explicitly
// pulled in via Q_IMPORT_PLUGIN.  Class names come from each module's
// qmldir 'classname' entry.  Order matters only in that style plugins
// must be present before QtQuick.Controls instantiates a control.
//
// Windows uses a static Qt build at C:/qt6 so we anchor every plugin
// here.  macOS uses Homebrew Qt6 which is dynamic — the loader finds
// plugins from the framework bundle and these macros would be
// unresolved-symbol errors against missing static libs.  Linux GUI
// will choose its own path once it gets wired up.
#ifdef VIVORA_WINDOWS
Q_IMPORT_PLUGIN(QtQmlPlugin)
Q_IMPORT_PLUGIN(QtQmlModelsPlugin)
Q_IMPORT_PLUGIN(QtQmlWorkerScriptPlugin)
Q_IMPORT_PLUGIN(QtQuick2Plugin)
Q_IMPORT_PLUGIN(QtQuick_WindowPlugin)
Q_IMPORT_PLUGIN(QtQuickLayoutsPlugin)
Q_IMPORT_PLUGIN(QtQuickTemplates2Plugin)
Q_IMPORT_PLUGIN(QtQuickControls2Plugin)
Q_IMPORT_PLUGIN(QtQuickControls2ImplPlugin)
Q_IMPORT_PLUGIN(QtQuickControls2BasicStylePlugin)
Q_IMPORT_PLUGIN(QtQuickControls2BasicStyleImplPlugin)
Q_IMPORT_PLUGIN(QtQuickDialogsPlugin)
Q_IMPORT_PLUGIN(QtQuickDialogs2QuickImplPlugin)
// TLS backend for QNetworkAccessManager (CloudClient talks to
// https://cloud.vivora.dev).  Static Qt registers no TLS backend unless its
// plugin is imported; without one the first HTTPS request crashes.  Schannel
// is the native Windows backend — no OpenSSL runtime dependency.
Q_IMPORT_PLUGIN(QSchannelBackend)
// Network information backend (Network List Manager) for
// QNetworkInformation — drives the VIV-57 rendezvous refresh on network
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

} // namespace

int run_gui(int argc, char** argv) {
    QApplication app(argc, argv);

#ifdef VIVORA_WINDOWS
    // Force the native Schannel TLS backend.  The static Qt also exposes an
    // OpenSSL backend, but it's wired against a mismatched OpenSSL 1.1/3 set
    // (linked with /FORCE:MULTIPLE) and crashes mid-handshake.  Schannel uses
    // Windows' own TLS — no OpenSSL.  Must run before any QSslSocket is used.
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
    //   Linux:   ~/.local/share/Vivora/vivora.log
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

    // Hard requirement — without a tray we lose the "always on" promise.
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        log::error("GUI", "No system tray available on this desktop — refusing to start GUI");
        return 1;
    }

    // Stay alive when the main window closes; the tray "Quit" entry is
    // the only path that actually ends the process.
    QApplication::setQuitOnLastWindowClosed(false);

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
    engine.load(QUrl("qrc:/qml/main.qml"));
    if (engine.rootObjects().isEmpty()) {
        log::error("GUI", "Failed to load main.qml");
        return 1;
    }

    log::info("GUI", "Vivora GUI ready");
    const int rc = app.exec();
    // lock released in destructor; explicit reset here keeps the order
    // obvious — AppController teardown happens first (stops host /
    // view workers), then we drop the lock so the next launch can grab it.
    lock.reset();
    return rc;
}

} // namespace vivora::gui
