#pragma once

#include <QObject>
#include <QSystemTrayIcon>

class QMenu;
class QAction;

namespace vivora::gui {

// Thin wrapper around QSystemTrayIcon: status-driven icon (idle vs
// sharing), small context menu with Show / Stop sharing / Settings / Quit.
// Actions emit signals the AppController can wire up.

class Tray : public QObject {
    Q_OBJECT
public:
    explicit Tray(QObject* parent = nullptr);
    ~Tray() override;

    // Update the tray icon/tooltip to reflect current state.  Cheap —
    // safe to call on every state change.
    void setSharing(bool sharing, int clientCount);

    // Toast-style notification (Windows balloon / macOS Notification
    // Center / Linux libnotify, all driven by the same Qt API).
    void notify(const QString& title, const QString& body);

signals:
    void showRequested();        // double-click on icon, or "Show" menu entry
    void stopSharingRequested();
    void settingsRequested();
    void quitRequested();

private:
    QSystemTrayIcon* tray_ = nullptr;
    QMenu*           menu_ = nullptr;
    QAction*         showAct_         = nullptr;
    QAction*         stopSharingAct_  = nullptr;
    QAction*         settingsAct_     = nullptr;
    QAction*         quitAct_         = nullptr;
};

} // namespace vivora::gui
