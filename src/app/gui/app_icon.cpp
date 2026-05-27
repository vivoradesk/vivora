#include "app/gui/app_icon.h"

#include <QPixmap>

namespace vivora::gui {

namespace {

// Load the multi-size icon for a given `prefix` from the qrc, e.g.
// `:/icons/tray/idle-` → loads idle-16.png, idle-22.png, ... and
// composes them into a single multi-resolution QIcon.  QIcon then
// picks the best size automatically based on the request site (tray
// surface, taskbar, About dialog, etc.).
QIcon load_multi(const QString& prefix, const int* sizes, int n) {
    QIcon icon;
    for (int i = 0; i < n; ++i) {
        QPixmap pm(prefix + QString::number(sizes[i]) + ".png");
        if (!pm.isNull()) icon.addPixmap(pm);
    }
    return icon;
}

} // namespace

QIcon make_app_icon() {
    static const int sizes[] = { 16, 32, 48, 64, 128, 256 };
    return load_multi(":/icons/app/", sizes, sizeof(sizes)/sizeof(sizes[0]));
}

QIcon make_tray_idle_icon() {
    static const int sizes[] = { 16, 22, 32, 44, 64 };
    return load_multi(":/icons/tray/idle-", sizes, sizeof(sizes)/sizeof(sizes[0]));
}

QIcon make_tray_sharing_icon() {
    static const int sizes[] = { 16, 22, 32, 44, 64 };
    return load_multi(":/icons/tray/sharing-", sizes, sizeof(sizes)/sizeof(sizes[0]));
}

QIcon make_tray_error_icon() {
    static const int sizes[] = { 16, 22, 32, 44, 64 };
    return load_multi(":/icons/tray/error-", sizes, sizeof(sizes)/sizeof(sizes[0]));
}

} // namespace vivora::gui
