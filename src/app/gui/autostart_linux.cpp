// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/gui/autostart.h"

#include "common/utils/log.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTextStream>

namespace vivora::gui {

namespace {

// The XDG autostart spec: every .desktop file in $XDG_CONFIG_HOME/autostart
// is launched when the session starts.  Named after the reverse-DNS app id so
// it lines up with the .desktop we install into share/applications.
QString autostart_path() {
    QString dir = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.config");
    return dir + QStringLiteral("/autostart/dev.vivora.app.desktop");
}

// Absolute path to launch at login.
//
// applicationFilePath() is wrong inside an AppImage: it points into the
// per-run mount under /tmp, which is gone by the next boot.  AppImages export
// $APPIMAGE with the path of the .AppImage file itself, which is what we want
// to record.
QString launch_target() {
    const QString appimage = qEnvironmentVariable("APPIMAGE");
    if (!appimage.isEmpty() && QFileInfo::exists(appimage))
        return appimage;
    return QCoreApplication::applicationFilePath();
}

// Exec= is parsed with shell-like quoting rules, so a path containing spaces
// has to be quoted and the characters the spec calls out have to be escaped.
QString quote_exec(const QString& path) {
    QString out = path;
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    out.replace(QLatin1Char('"'),  QStringLiteral("\\\""));
    out.replace(QLatin1Char('$'),  QStringLiteral("\\$"));
    out.replace(QLatin1Char('`'),  QStringLiteral("\\`"));
    return QLatin1Char('"') + out + QLatin1Char('"');
}

} // namespace

bool Autostart::supported() { return true; }

bool Autostart::enabled() {
    // The file on disk is the source of truth, not our ini: the user can
    // remove it with GNOME Tweaks or `rm`, and the Settings checkbox has to
    // follow reality on the next launch.  Honour Hidden=true, which is how
    // some desktop tools disable an entry rather than deleting it.
    QFile f(autostart_path());
    if (!f.exists()) return false;
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.compare(QStringLiteral("Hidden=true"), Qt::CaseInsensitive) == 0)
            return false;
    }
    return true;
}

void Autostart::setEnabled(bool on) {
    const QString path = autostart_path();

    if (!on) {
        if (QFile::exists(path) && !QFile::remove(path))
            log::warn("Autostart", "Could not remove %s", path.toUtf8().constData());
        return;
    }

    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        log::warn("Autostart", "Could not write %s", path.toUtf8().constData());
        return;
    }

    // Rewriting unconditionally also self-heals a stale path left behind by a
    // moved or updated install, which is the same reason the Windows backend
    // overwrites its Run key entry.
    QTextStream out(&f);
    out << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Version=1.0\n"
        << "Name=Vivora\n"
        << "Comment=Low-latency open-source remote desktop\n"
        << "Exec=" << quote_exec(launch_target()) << "\n"
        << "Icon=dev.vivora.app\n"
        << "Terminal=false\n"
        << "StartupNotify=false\n"
        // Vivora starts minimised to the tray and is useful from the moment
        // the session is up; no reason to delay it.
        << "X-GNOME-Autostart-enabled=true\n";
    f.close();
    log::info("Autostart", "Registered %s", path.toUtf8().constData());
}

} // namespace vivora::gui
