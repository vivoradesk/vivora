#include "client/render/stream_menu.h"

#include <QCheckBox>
#include <QColor>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace vivora {

StreamMenu::StreamMenu(QWidget* parent) : QWidget(parent) {
    // Top-level frameless overlay — interactive, so (unlike the HUD) it
    // accepts focus and mouse input.  Stays on top of the stream window.
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::StrongFocus);
    setWindowTitle("Vivora — Menu");

    setStyleSheet(
        "QWidget { color: rgb(235,235,235);"
        "  font-family: 'Segoe UI','DejaVu Sans',sans-serif; font-size: 13px; }"
        "QLabel#title { font-size: 16px; font-weight: 600; }"
        "QLabel#info { color: rgb(170,176,186);"
        "  font-family: 'Consolas','DejaVu Sans Mono',monospace; font-size: 12px; }"
        "QLabel#hint { color: rgb(140,146,156); font-size: 11px; }"
        "QCheckBox { spacing: 8px; }"
        "QPushButton { background: rgba(255,255,255,0.10); border: none;"
        "  border-radius: 6px; padding: 8px 14px; }"
        "QPushButton:hover { background: rgba(255,255,255,0.18); }"
        "QPushButton:disabled { color: rgb(120,124,132);"
        "  background: rgba(255,255,255,0.05); }"
        "QPushButton#disconnect { background: rgba(220,68,68,0.85); color: white; }"
        "QPushButton#disconnect:hover { background: rgba(235,80,80,0.95); }"
        "QSlider::groove:horizontal { height: 4px; background: rgba(255,255,255,0.20);"
        "  border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 14px; margin: -6px 0;"
        "  background: rgb(235,235,235); border-radius: 7px; }"
        "QSlider::sub-page:horizontal { background: rgb(120,170,255);"
        "  border-radius: 2px; }");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(12);

    auto* title = new QLabel("Vivora", this);
    title->setObjectName("title");
    root->addWidget(title);

    info_label_ = new QLabel(this);
    info_label_->setObjectName("info");
    info_label_->setText("Not connected");
    root->addWidget(info_label_);

    // --- Audio ---------------------------------------------------------
    auto* vol_row = new QHBoxLayout();
    vol_row->setSpacing(10);
    auto* vol_lbl = new QLabel("Volume", this);
    vol_lbl->setMinimumWidth(72);
    volume_slider_ = new QSlider(Qt::Horizontal, this);
    volume_slider_->setRange(0, 100);
    volume_slider_->setValue(100);
    volume_value_ = new QLabel("100%", this);
    volume_value_->setMinimumWidth(40);
    volume_value_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    vol_row->addWidget(vol_lbl);
    vol_row->addWidget(volume_slider_, 1);
    vol_row->addWidget(volume_value_);
    root->addLayout(vol_row);

    mute_check_ = new QCheckBox("Mute", this);
    root->addWidget(mute_check_);

    // --- Input ---------------------------------------------------------
    viewonly_check_ = new QCheckBox("View only (don't send my input)", this);
    root->addWidget(viewonly_check_);

    // --- Display -------------------------------------------------------
    // Monitor selection lands with VIV-50 (host advertises a display list).
    // Shown disabled so the slot is visible and the intent is clear.
    auto* monitor_btn = new QPushButton("Switch monitor…  (soon)", this);
    monitor_btn->setEnabled(false);
    monitor_btn->setToolTip("Host monitor selection — coming with VIV-50");
    root->addWidget(monitor_btn);

    fullscreen_btn_ = new QPushButton("Toggle fullscreen", this);
    root->addWidget(fullscreen_btn_);

    // --- Session -------------------------------------------------------
    disconnect_btn_ = new QPushButton("Disconnect", this);
    disconnect_btn_->setObjectName("disconnect");
    root->addWidget(disconnect_btn_);

    auto* hint = new QLabel("Ctrl+F1 or Esc to close", this);
    hint->setObjectName("hint");
    root->addWidget(hint);

    // --- Wiring --------------------------------------------------------
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
    connect(fullscreen_btn_, &QPushButton::clicked, this, [this]() {
        emit fullscreenToggled();
    });
    connect(disconnect_btn_, &QPushButton::clicked, this, [this]() {
        if (actions_.disconnect) actions_.disconnect();
        close_menu();
    });

    adjustSize();
    setFixedWidth(320);
}

void StreamMenu::set_initial_state(float volume, bool muted, bool view_only) {
    suppress_signals_ = true;
    int pct = static_cast<int>(volume * 100.0f + 0.5f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    volume_slider_->setValue(pct);
    volume_value_->setText(QString::number(pct) + "%");
    mute_check_->setChecked(muted);
    volume_slider_->setEnabled(!muted);
    viewonly_check_->setChecked(view_only);
    suppress_signals_ = false;
}

void StreamMenu::set_info(float rtt_ms, uint32_t width, uint32_t height,
                          const QString& decoder) {
    QString res = (width && height)
        ? QString("%1×%2").arg(width).arg(height)
        : QString("—");
    info_label_->setText(
        QString::asprintf("%-9s %.0f ms\n", "Latency:", rtt_ms)
        + QString("Display:  %1\n").arg(res)
        + QString("Decoder:  %1").arg(decoder.isEmpty() ? QString("—") : decoder));
}

void StreamMenu::open_over(QWidget* anchor) {
    if (anchor) {
        const QRect a(anchor->mapToGlobal(QPoint(0, 0)), anchor->size());
        adjustSize();
        move(a.center().x() - width() / 2, a.center().y() - height() / 2);
    }
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
    // Click-away: when the panel loses activation (user clicked the stream or
    // another window), dismiss it.  Mirrors the HUD/status overlay discipline.
    if (e->type() == QEvent::ActivationChange && isVisible() && !isActiveWindow()) {
        close_menu();
    }
    QWidget::changeEvent(e);
}

void StreamMenu::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(24, 26, 31, 235));
    p.drawRoundedRect(rect(), 12, 12);
}

} // namespace vivora
