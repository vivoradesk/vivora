#include "app/gui/gui_main.h"

#include "app/gui/address_book.h"
#include "app/gui/app_controller.h"
#include "app/gui/settings.h"
#include "app/gui/tray.h"

#include "common/utils/log.h"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSystemTrayIcon>
#include <QUrl>

#ifdef DESKBEAM_WINDOWS
#include "common/net/winsock_socket.h"
#endif

namespace deskbeam::gui {

int run_gui(int argc, char** argv) {
    QApplication app(argc, argv);

#ifdef DESKBEAM_WINDOWS
    deskbeam::net::WinsockInit wsa;
    if (!wsa.ok) {
        log::error("GUI", "Failed to init Winsock");
        return 1;
    }
#endif

    // Hard requirement — without a tray we lose the "always on" promise.
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        log::error("GUI", "No system tray available on this desktop — refusing to start GUI");
        return 1;
    }

    // Stay alive when the main window closes; the tray "Quit" entry is
    // the only path that actually ends the process.
    QApplication::setQuitOnLastWindowClosed(false);

    AppController controller;
    Tray          tray;
    controller.setTray(&tray);

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

    log::info("GUI", "DeskBeam GUI ready");
    return app.exec();
}

} // namespace deskbeam::gui
