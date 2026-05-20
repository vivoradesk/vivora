#pragma once

#include <QIcon>

namespace vivora::gui {

// Procedurally-drawn placeholder icon set for the app + tray.  Lives
// here so we don't have to ship a PNG / wire up Qt's SVG plugin in the
// static build just for a placeholder.  Replace with a designer asset
// later by handing back QIcon(":/assets/icon.png") + the .ico baked
// into the .exe via a .rc file.
//
// All variants render at multiple sizes (16/24/32/48/64/128/256) so
// the OS picks the right one for taskbar / tray / explorer.
QIcon make_app_icon();        // blue — idle / default
QIcon make_tray_idle_icon();
QIcon make_tray_sharing_icon();   // green — actively sharing

} // namespace vivora::gui
