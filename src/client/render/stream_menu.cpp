#include "client/render/stream_menu.h"

#include <QApplication>
#include <QCheckBox>
#include <QGuiApplication>
#include <QScreen>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace vivora {

namespace {

// Palette (matches the VIV-74 mock).
const char* kColGreen  = "#3ddc84";
const QColor kIconLight(200, 204, 210);
const QColor kIconDim(120, 126, 134);

// Tiny vector icons drawn at run time so we don't need extra qrc assets.
// kind: 0=monitor, 1=fullscreen, 2=power.
QPixmap draw_icon(int kind, QColor c, int sz = 16) {
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(c);
    pen.setWidthF(1.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const qreal s = sz;
    if (kind == 0) {                       // monitor
        p.drawRoundedRect(QRectF(2, 2.5, s - 4, s - 7), 1.5, 1.5);
        p.drawLine(QPointF(s / 2 - 2.5, s - 1.2), QPointF(s / 2 + 2.5, s - 1.2));
        p.drawLine(QPointF(s / 2, s - 4.5), QPointF(s / 2, s - 1.2));
    } else if (kind == 1) {                // fullscreen corners
        const qreal a = 2.5, len = 3.2, b = s - 2.5;
        p.drawLine(QPointF(a, a), QPointF(a + len, a));
        p.drawLine(QPointF(a, a), QPointF(a, a + len));
        p.drawLine(QPointF(b, a), QPointF(b - len, a));
        p.drawLine(QPointF(b, a), QPointF(b, a + len));
        p.drawLine(QPointF(a, b), QPointF(a + len, b));
        p.drawLine(QPointF(a, b), QPointF(a, b - len));
        p.drawLine(QPointF(b, b), QPointF(b - len, b));
        p.drawLine(QPointF(b, b), QPointF(b, b - len));
    } else if (kind == 2) {                // power
        p.drawArc(QRectF(3, 3.5, s - 6, s - 6), 70 * 16, 320 * 16);
        p.drawLine(QPointF(s / 2, 2.2), QPointF(s / 2, s / 2));
    }
    p.end();
    return pm;
}

// Brand mark: a rounded square holding the app icon (or a "V" fallback).
QPixmap make_logo(int sz = 38) {
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 20));
    p.drawRoundedRect(QRectF(0, 0, sz, sz), 10, 10);
    QIcon ic = qApp ? qApp->windowIcon() : QIcon();
    if (!ic.isNull()) {
        const int g = sz - 14;
        p.drawPixmap((sz - g) / 2, (sz - g) / 2, ic.pixmap(g, g));
    } else {
        p.setPen(QColor(kColGreen));
        QFont f = p.font();
        f.setPixelSize(sz - 14);
        f.setBold(true);
        p.setFont(f);
        p.drawText(pm.rect(), Qt::AlignCenter, "V");
    }
    p.end();
    return pm;
}

QFrame* make_separator(QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setFixedHeight(1);
    f->setStyleSheet("background: rgba(255,255,255,0.07); border: none;");
    return f;
}

} // namespace

StreamMenu::StreamMenu(QWidget* parent) : QWidget(parent) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::StrongFocus);
    setWindowTitle("Vivora — Menu");
    setFixedWidth(360);

    setStyleSheet(
        "QWidget { color: #e6e8ec;"
        "  font-family: 'Segoe UI','Inter','DejaVu Sans',sans-serif; font-size: 13px; }"
        "QLabel#app { font-size: 16px; font-weight: 700; }"
        "QLabel#peer { color: #888e98; font-size: 12px; }"
        "QLabel#label { color: #888e98; }"
        "QLabel#value { color: #d9dce2; font-weight: 600; }"
        "QLabel#hint  { color: #6f757f; }"
        "QLabel#timer { color: " + QString(kColGreen) + ";"
        "  background: rgba(45,160,90,0.16); border-radius: 11px;"
        "  padding: 4px 10px; font-weight: 600; font-size: 12px; }"
        "QLabel#soon  { color: #6f757f; font-size: 10px; font-weight: 700;"
        "  letter-spacing: 1px; }"
        "QLabel#keycap { color: #aeb3bc; background: rgba(255,255,255,0.07);"
        "  border: 1px solid rgba(255,255,255,0.12); border-radius: 4px;"
        "  padding: 2px 6px; font-size: 11px; }"
        "QCheckBox { spacing: 10px; min-height: 22px; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border-radius: 5px;"
        "  border: 1px solid rgba(255,255,255,0.28); background: rgba(255,255,255,0.04); }"
        "QCheckBox::indicator:checked { background: #4a8cff; border-color: #4a8cff; }"
        "QPushButton { background: rgba(255,255,255,0.06); border: none;"
        "  border-radius: 9px; }"
        "QPushButton:hover { background: rgba(255,255,255,0.11); }"
        "QPushButton:disabled { background: rgba(255,255,255,0.03); }"
        "QPushButton#disconnect { background: #e5484d; }"
        "QPushButton#disconnect:hover { background: #ec5b60; }"
        "QSlider::groove:horizontal { height: 4px; background: rgba(255,255,255,0.18);"
        "  border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 15px; margin: -6px 0;"
        "  background: white; border-radius: 7px; }"
        "QSlider::sub-page:horizontal { background: #4a8cff; border-radius: 2px; }");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 15);
    root->setSpacing(0);

    // ---- Header -------------------------------------------------------
    auto* header = new QHBoxLayout();
    header->setSpacing(12);
    logo_label_ = new QLabel(this);
    logo_label_->setPixmap(make_logo());
    logo_label_->setFixedSize(38, 38);
    header->addWidget(logo_label_);

    auto* names = new QVBoxLayout();
    names->setSpacing(1);
    app_label_ = new QLabel("Vivora", this);
    app_label_->setObjectName("app");
    peer_label_ = new QLabel(QString(), this);
    peer_label_->setObjectName("peer");
    names->addWidget(app_label_);
    names->addWidget(peer_label_);
    header->addLayout(names);
    header->addStretch(1);

    timer_label_ = new QLabel("●  00:00:00", this);
    timer_label_->setObjectName("timer");
    header->addWidget(timer_label_, 0, Qt::AlignTop);
    root->addLayout(header);

    root->addSpacing(14);
    root->addWidget(make_separator(this));
    root->addSpacing(14);

    // ---- Info grid ----------------------------------------------------
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(7);
    grid->setColumnStretch(1, 1);
    auto add_info_row = [&](int row, const char* label, QLabel*& value) {
        auto* l = new QLabel(label, this);
        l->setObjectName("label");
        value = new QLabel("—", this);
        value->setObjectName("value");
        value->setTextFormat(Qt::RichText);
        grid->addWidget(l, row, 0);
        grid->addWidget(value, row, 1);
    };
    add_info_row(0, "Latency", latency_value_);
    add_info_row(1, "Display", display_value_);
    add_info_row(2, "Decoder", decoder_value_);
    root->addLayout(grid);

    root->addSpacing(14);
    root->addWidget(make_separator(this));
    root->addSpacing(16);

    // ---- Volume -------------------------------------------------------
    auto* vol_row = new QHBoxLayout();
    vol_row->setSpacing(12);
    auto* vol_lbl = new QLabel("Volume", this);
    volume_slider_ = new QSlider(Qt::Horizontal, this);
    volume_slider_->setRange(0, 100);
    volume_slider_->setValue(100);
    volume_value_ = new QLabel("100%", this);
    volume_value_->setObjectName("value");
    volume_value_->setMinimumWidth(40);
    volume_value_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    vol_row->addWidget(vol_lbl);
    vol_row->addWidget(volume_slider_, 1);
    vol_row->addWidget(volume_value_);
    root->addLayout(vol_row);

    root->addSpacing(14);
    // All three checkboxes share one row shape (checkbox [+ hint] + stretch)
    // so their indicators line up at the same x and the row heights — hence
    // the inter-row gaps — are identical across platforms.
    auto add_check = [&](QCheckBox*& cb, const QString& text, const QString& hint) {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(7);
        cb = new QCheckBox(text, this);
        row->addWidget(cb, 0, Qt::AlignVCenter);
        if (!hint.isEmpty()) {
            auto* h = new QLabel(hint, this);
            h->setObjectName("hint");
            row->addWidget(h, 0, Qt::AlignVCenter);
        }
        row->addStretch(1);
        root->addLayout(row);
    };
    add_check(mute_check_, "Mute", QString());
    root->addSpacing(12);
    add_check(viewonly_check_, "View only", "(don't send my input)");
    root->addSpacing(12);
    add_check(aspect_check_, "Keep aspect ratio", QString());
    aspect_check_->setChecked(true);

    root->addSpacing(16);
    root->addWidget(make_separator(this));
    root->addSpacing(14);

    // ---- Action buttons ----------------------------------------------
    // Built with an inner layout (icon + centred text [+ badge]) so the
    // icon sits left of the label like the mock.  Child labels are
    // mouse-transparent so clicks reach the button.
    auto make_button = [&](int icon_kind, const QString& text, QColor icon_col,
                           QColor text_col, const char* obj_name,
                           bool with_soon) -> QPushButton* {
        auto* btn = new QPushButton(this);
        if (obj_name) btn->setObjectName(obj_name);
        btn->setFixedHeight(46);
        btn->setCursor(Qt::PointingHandCursor);
        auto* lay = new QHBoxLayout(btn);
        lay->setContentsMargins(14, 0, 14, 0);
        lay->setSpacing(9);
        auto* icon = new QLabel(btn);
        icon->setPixmap(draw_icon(icon_kind, icon_col));
        icon->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* lbl = new QLabel(text, btn);
        lbl->setStyleSheet(QString("color: %1; font-weight: 600;").arg(text_col.name()));
        lbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        lay->addStretch(1);
        lay->addWidget(icon);
        lay->addWidget(lbl);
        lay->addStretch(1);
        if (with_soon) {
            auto* soon = new QLabel("SOON", btn);
            soon->setObjectName("soon");
            soon->setAttribute(Qt::WA_TransparentForMouseEvents);
            lay->addWidget(soon);
        }
        return btn;
    };

    monitor_btn_ = make_button(0, "Switch monitor…", kIconDim, kIconDim,
                               nullptr, /*with_soon=*/true);
    monitor_btn_->setEnabled(false);
    monitor_btn_->setCursor(Qt::ArrowCursor);
    monitor_btn_->setToolTip("Host monitor selection — coming with VIV-50");
    root->addWidget(monitor_btn_);

    root->addSpacing(10);
    fullscreen_btn_ = make_button(1, "Toggle fullscreen", kIconLight,
                                  QColor("#e6e8ec"), nullptr, false);
    root->addWidget(fullscreen_btn_);

    root->addSpacing(10);
    disconnect_btn_ = make_button(2, "Disconnect", QColor("#ffffff"),
                                  QColor("#ffffff"), "disconnect", false);
    root->addWidget(disconnect_btn_);

    // ---- Footer -------------------------------------------------------
    root->addSpacing(15);
    auto* footer = new QHBoxLayout();
    footer->setSpacing(7);
    auto keycap = [&](const QString& t) {
        auto* k = new QLabel(t, this);
        k->setObjectName("keycap");
        return k;
    };
#ifdef Q_OS_MACOS
    footer->addWidget(keycap("⌘ F1"));
#else
    footer->addWidget(keycap("Ctrl + F1"));
#endif
    auto* or_lbl = new QLabel("or", this);
    or_lbl->setObjectName("hint");
    footer->addWidget(or_lbl);
    footer->addWidget(keycap("Esc"));
    auto* close_lbl = new QLabel("to close", this);
    close_lbl->setObjectName("hint");
    footer->addWidget(close_lbl);
    footer->addStretch(1);
    root->addLayout(footer);

    // ---- Wiring -------------------------------------------------------
    connect(volume_slider_, &QSlider::valueChanged, this, [this](int v) {
        volume_value_->setText(QString::number(v) + "%");
        if (suppress_signals_) return;
        if (actions_.set_volume) actions_.set_volume(static_cast<float>(v) / 100.0f);
    });
    connect(mute_check_, &QCheckBox::toggled, this, [this](bool on) {
        volume_slider_->setEnabled(!on);
        if (suppress_signals_) return;
        if (actions_.set_muted) actions_.set_muted(on);
    });
    connect(viewonly_check_, &QCheckBox::toggled, this, [this](bool on) {
        if (suppress_signals_) return;
        if (actions_.set_view_only) actions_.set_view_only(on);
    });
    connect(aspect_check_, &QCheckBox::toggled, this, [this](bool on) {
        if (suppress_signals_) return;
        emit keepAspectToggled(on);
    });
    connect(fullscreen_btn_, &QPushButton::clicked, this,
            [this]() { emit fullscreenToggled(); });
    connect(disconnect_btn_, &QPushButton::clicked, this, [this]() {
        if (actions_.disconnect) actions_.disconnect();
        close_menu();
    });

    adjustSize();
}

void StreamMenu::set_header(const QString& app, const QString& peer) {
    app_label_->setText(app.isEmpty() ? QStringLiteral("Vivora") : app);
    peer_label_->setText(peer);
    peer_label_->setVisible(!peer.isEmpty());
}

void StreamMenu::set_initial_state(float volume, bool muted, bool view_only,
                                   bool keep_aspect) {
    suppress_signals_ = true;
    int pct = static_cast<int>(volume * 100.0f + 0.5f);
    pct = qBound(0, pct, 100);
    volume_slider_->setValue(pct);
    volume_value_->setText(QString::number(pct) + "%");
    mute_check_->setChecked(muted);
    volume_slider_->setEnabled(!muted);
    viewonly_check_->setChecked(view_only);
    aspect_check_->setChecked(keep_aspect);
    suppress_signals_ = false;
}

void StreamMenu::set_info(const MenuInfo& info) {
    if (info.connected) {
        latency_value_->setText(
            QString("<span style='color:%1'>●</span>&nbsp; %2 ms &nbsp;·&nbsp; %3")
                .arg(kColGreen)
                .arg(info.rtt_ms, 0, 'f', 1)
                .arg(info.transport.isEmpty() ? QStringLiteral("P2P") : info.transport));
        QString disp = (info.width && info.height)
            ? QString("%1 × %2").arg(info.width).arg(info.height)
            : QStringLiteral("—");
        if (info.hz) disp += QString(" &nbsp;·&nbsp; %1 Hz").arg(info.hz);
        display_value_->setText(disp);
        QString dec = info.codec.isEmpty() ? QString() : info.codec;
        if (!info.decoder.isEmpty())
            dec += (dec.isEmpty() ? QString() : QStringLiteral(" &nbsp;·&nbsp; ")) + info.decoder;
        decoder_value_->setText(dec.isEmpty() ? QStringLiteral("—") : dec);
    } else {
        latency_value_->setText("—");
        display_value_->setText("—");
        decoder_value_->setText("—");
    }

    const uint32_t s = info.session_seconds;
    timer_label_->setText(QString::asprintf("●  %02u:%02u:%02u",
                                            s / 3600, (s / 60) % 60, s % 60));
}

void StreamMenu::open_over(QWidget* anchor) {
    adjustSize();
    QPoint center;
    if (anchor) {
        const QRect a(anchor->mapToGlobal(QPoint(0, 0)), anchor->size());
        center = a.center();
    } else if (auto* scr = QGuiApplication::primaryScreen()) {
        // No Qt anchor (e.g. the macOS Cocoa stream window) — centre on the
        // primary screen.
        center = scr->geometry().center();
    }
    if (!center.isNull())
        move(center.x() - width() / 2, center.y() - height() / 2);
    show();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
}

void StreamMenu::close_menu() {
    if (!isVisible()) return;
    hide();
    emit closed();
}

void StreamMenu::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape
        || (e->key() == Qt::Key_F1 && (e->modifiers() & Qt::ControlModifier))) {
        close_menu();
        return;
    }
    QWidget::keyPressEvent(e);
}

void StreamMenu::changeEvent(QEvent* e) {
    if (e->type() == QEvent::ActivationChange && isVisible() && !isActiveWindow()) {
        close_menu();
    }
    QWidget::changeEvent(e);
}

void StreamMenu::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 18), 1));
    p.setBrush(QColor(27, 30, 36, 240));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);
}

} // namespace vivora
