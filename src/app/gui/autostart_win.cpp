// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/gui/autostart.h"

#include "common/utils/log.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QString>

namespace vivora::gui {

namespace {

// Per-user Run key — no elevation needed, applies to the current user only.
constexpr const char* RUN_KEY =
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const char* VALUE_NAME = "Vivora";

// Quoted absolute native path of the running executable, e.g.
//   "C:\Program Files\Vivora\vivora.exe"
// Quoting matters: unquoted paths with spaces are parsed up to the first
// space by the shell that processes the Run key.
QString quoted_exe_path() {
    return QLatin1Char('"')
         + QDir::toNativeSeparators(QCoreApplication::applicationFilePath())
         + QLatin1Char('"');
}

} // namespace

bool Autostart::supported() { return true; }

bool Autostart::enabled() {
    QSettings run(QString::fromLatin1(RUN_KEY), QSettings::NativeFormat);
    return run.contains(QString::fromLatin1(VALUE_NAME));
}

void Autostart::setEnabled(bool on) {
    QSettings run(QString::fromLatin1(RUN_KEY), QSettings::NativeFormat);
    if (on) {
        const QString cmd = quoted_exe_path();
        if (run.value(QString::fromLatin1(VALUE_NAME)).toString() != cmd) {
            run.setValue(QString::fromLatin1(VALUE_NAME), cmd);
            log::info("GUI", "Start-at-login enabled: %s",
                      cmd.toUtf8().constData());
        }
    } else if (run.contains(QString::fromLatin1(VALUE_NAME))) {
        run.remove(QString::fromLatin1(VALUE_NAME));
        log::info("GUI", "Start-at-login disabled");
    }
    run.sync();
    if (run.status() != QSettings::NoError)
        log::warn("GUI", "Start-at-login registry write failed (status %d)",
                  static_cast<int>(run.status()));
}

} // namespace vivora::gui
