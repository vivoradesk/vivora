#include "app/gui/app_icon.h"

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace deskbeam::gui {

namespace {

// Draw the icon at a given size into a freshly-allocated transparent
// pixmap.  Keeps geometry proportional so the same drawing works at
// 16x16 (tray) and 256x256 (Explorer / taskbar large).
QPixmap render_one(int sz, const QColor& fill) {
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // Rounded-square background.  Small inset so tray icons that get
    // cropped to a circle (some Linux DEs) still show the full glyph.
    const qreal inset  = sz * 0.06;
    const qreal radius = sz * 0.18;
    const QRectF bg(inset, inset, sz - 2*inset, sz - 2*inset);

    p.setBrush(fill);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(bg, radius, radius);

    // Stylised "monitor + beam" glyph in white.  The monitor is a
    // rounded rect in the lower 55% of the icon; the beam is two
    // concentric arcs above the monitor, suggesting a wireless
    // transmission.  Geometry derived from the icon size so it stays
    // crisp at tray-icon resolutions.
    p.setBrush(Qt::NoBrush);
    QPen pen(Qt::white);
    pen.setWidthF(sz * 0.06);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);

    // Monitor body
    const qreal mw = sz * 0.50;
    const qreal mh = sz * 0.34;
    const qreal mx = (sz - mw) / 2.0;
    const qreal my = sz * 0.50;
    const QRectF monitor(mx, my, mw, mh);
    p.setBrush(Qt::white);
    p.drawRoundedRect(monitor, sz * 0.04, sz * 0.04);
    p.setBrush(Qt::NoBrush);

    // Stand
    const qreal stand_w = sz * 0.16;
    const qreal stand_x = (sz - stand_w) / 2.0;
    p.fillRect(QRectF(stand_x, my + mh, stand_w, sz * 0.04), Qt::white);

    // Two wireless arcs above the monitor, centred on its top edge.
    const QPointF apex(sz / 2.0, my);
    auto draw_arc = [&](qreal radius_factor) {
        const qreal r = sz * radius_factor;
        const QRectF arc_rect(apex.x() - r, apex.y() - r, 2*r, 2*r);
        // Top half from 30° to 150° (Qt angles in 1/16 degree).
        p.drawArc(arc_rect, 30 * 16, 120 * 16);
    };
    draw_arc(0.18);
    draw_arc(0.30);

    p.end();
    return pm;
}

QIcon make_icon(const QColor& fill) {
    QIcon icon;
    // Sizes Windows / Linux / macOS pick from for tray + taskbar +
    // Explorer.  256 covers high-DPI taskbar and About dialogs.
    const int sizes[] = { 16, 24, 32, 48, 64, 128, 256 };
    for (int sz : sizes) icon.addPixmap(render_one(sz, fill));
    return icon;
}

} // namespace

// Brand colours — keep close to the QML accent (#2196f3) so tray / app
// / window all read as the same product.  Sharing variant shifts hue to
// the same green the main window uses for the "● Sharing" label.
QIcon make_app_icon()         { return make_icon(QColor("#2196f3")); }
QIcon make_tray_idle_icon()   { return make_icon(QColor("#607d8b")); } // grey-blue
QIcon make_tray_sharing_icon(){ return make_icon(QColor("#4caf50")); } // green

} // namespace deskbeam::gui
