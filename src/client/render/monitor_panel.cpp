#include "client/render/monitor_panel.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QEvent>
#include <QEnterEvent>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

#include <algorithm>

namespace vivora {

namespace {

// Palette — shared with StreamMenu's dark theme.
const char* kColGreen = "#3ddc84";
const QColor kPanelBg(27, 30, 36, 240);

// One clickable display thumbnail.  Plain QWidget (no Q_OBJECT) — the click is
// delivered through a std::function so a second moc'd type isn't needed in this
// translation unit.  Sized proportionally to the display's aspect ratio so a
// taller monitor reads as taller, mirroring the design mock.
class MonitorThumb : public QWidget {
public:
    MonitorThumb(const protocol::MonitorDesc& d, std::function<void(uint32_t)> on_click,
                 QWidget* parent = nullptr)
        : QWidget(parent), desc_(d), on_click_(std::move(on_click)) {
        setCursor(desc_.viewing ? Qt::ArrowCursor : Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);

        // Fixed height; width follows aspect, clamped so the strip stays sane.
        const int h = 96;
        double aspect = (desc_.height > 0)
            ? static_cast<double>(desc_.width) / static_cast<double>(desc_.height)
            : 16.0 / 9.0;
        int w = static_cast<int>(h * aspect + 0.5);
        w = std::clamp(w, 84, 176);
        setFixedSize(w, h);

        QString res = QString("%1 × %2").arg(desc_.width).arg(desc_.height);
        setToolTip(desc_.viewing ? QString("Display %1 — currently viewing").arg(desc_.index)
                                 : QString("Switch to display %1  (%2)").arg(desc_.index).arg(res));
    }

protected:
    void enterEvent(QEnterEvent*) override { hover_ = true; update(); }
    void leaveEvent(QEvent*) override { hover_ = false; update(); }

    void mousePressEvent(QMouseEvent*) override {
        if (!desc_.viewing && on_click_) on_click_(desc_.index);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);

        QColor fill, border, idCol, resCol;
        if (desc_.viewing) {
            // Highlighted like the menu's green session pill.
            fill   = QColor(45, 160, 90, 40);
            border = QColor(kColGreen);
            idCol  = QColor(kColGreen);
            resCol = QColor(0xb6, 0xc2, 0xba);
        } else {
            fill   = QColor(255, 255, 255, hover_ ? 28 : 14);
            border = QColor(255, 255, 255, hover_ ? 70 : 34);
            idCol  = QColor(0xae, 0xb3, 0xbc);
            resCol = QColor(0x6f, 0x75, 0x7f);
        }
        p.setPen(QPen(border, 1.4));
        p.setBrush(fill);
        p.drawRoundedRect(r, 8, 8);

        // Index / "viewing" label, top-left.
        QFont f = p.font();
        f.setPixelSize(15);
        f.setBold(true);
        p.setFont(f);
        p.setPen(idCol);
        const QString head = desc_.viewing
            ? QString("%1 · viewing").arg(desc_.index)
            : QString::number(desc_.index);
        p.drawText(r.adjusted(11, 8, -8, 0), Qt::AlignLeft | Qt::AlignTop, head);

        // Resolution, bottom-left.
        QFont f2 = p.font();
        f2.setPixelSize(11);
        f2.setBold(false);
        p.setFont(f2);
        p.setPen(resCol);
        p.drawText(r.adjusted(11, 0, -8, -8), Qt::AlignLeft | Qt::AlignBottom,
                   QString("%1 × %2").arg(desc_.width).arg(desc_.height));
    }

private:
    protocol::MonitorDesc          desc_;
    std::function<void(uint32_t)>  on_click_;
    bool                           hover_ = false;
};

QLabel* make_keycap(const QString& t, QWidget* parent) {
    auto* k = new QLabel(t, parent);
    k->setObjectName("keycap");
    return k;
}

} // namespace

MonitorPanel::MonitorPanel(QWidget* parent) : QWidget(parent) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::StrongFocus);
    setWindowTitle("Vivora — Monitors");
    // Width follows content: two wide thumbnails (up to 176px each) plus
    // margins exceed any sensible fixed width — a hard 380 clipped the
    // rightmost thumbnail (VIV-50 live testing).  The layout constraint
    // keeps the popup exactly sized to its contents as the list changes.
    setMinimumWidth(380);

    setStyleSheet(
        "QWidget { color: #e6e8ec;"
        "  font-family: 'Segoe UI','Inter','DejaVu Sans',sans-serif; font-size: 13px; }"
        "QLabel#title { font-size: 15px; font-weight: 700; }"
        "QLabel#count { color: #888e98; font-size: 12px; }"
        "QLabel#hint  { color: #6f757f; }"
        "QLabel#empty { color: #888e98; }"
        "QLabel#soon  { color: #6f757f; font-size: 10px; font-weight: 700;"
        "  letter-spacing: 1px; }"
        "QLabel#keycap { color: #aeb3bc; background: rgba(255,255,255,0.07);"
        "  border: 1px solid rgba(255,255,255,0.12); border-radius: 4px;"
        "  padding: 2px 6px; font-size: 11px; }"
        "QCheckBox { spacing: 10px; min-height: 20px; color: #888e98; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border-radius: 5px;"
        "  border: 1px solid rgba(255,255,255,0.20); background: rgba(255,255,255,0.03); }"
        "QPushButton#refresh { background: rgba(255,255,255,0.08); border: none;"
        "  border-radius: 8px; padding: 7px 16px; font-weight: 600; }"
        "QPushButton#refresh:hover { background: rgba(255,255,255,0.14); }");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(0);

    // ---- Header --------------------------------------------------------
    auto* header = new QHBoxLayout();
    title_label_ = new QLabel("Remote monitors", this);
    title_label_->setObjectName("title");
    count_label_ = new QLabel(QString(), this);
    count_label_->setObjectName("count");
    header->addWidget(title_label_);
    header->addStretch(1);
    header->addWidget(count_label_, 0, Qt::AlignVCenter);
    root->addLayout(header);

    root->addSpacing(14);

    // ---- Thumbnail strip ----------------------------------------------
    strip_ = new QWidget(this);
    strip_layout_ = new QHBoxLayout(strip_);
    strip_layout_->setContentsMargins(0, 0, 0, 0);
    strip_layout_->setSpacing(10);
    root->addWidget(strip_);

    empty_label_ = new QLabel("Host exposes a single display.", this);
    empty_label_->setObjectName("empty");
    empty_label_->hide();
    root->addWidget(empty_label_);

    root->addSpacing(16);

    // ---- Future options (from the design mock) ------------------------
    // Stitched multi-monitor capture and local-scale matching are separate
    // features; surface them as disabled with a SOON badge so the panel
    // matches the intended layout without overpromising (VIV-50 ships the
    // switch only).
    auto add_soon_toggle = [&](const QString& text) {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(7);
        auto* cb = new QCheckBox(text, this);
        cb->setEnabled(false);
        row->addWidget(cb, 0, Qt::AlignVCenter);
        row->addStretch(1);
        auto* soon = new QLabel("SOON", this);
        soon->setObjectName("soon");
        row->addWidget(soon, 0, Qt::AlignVCenter);
        root->addLayout(row);
    };
    add_soon_toggle("Show all monitors stitched");
    root->addSpacing(10);
    add_soon_toggle("Match local display scale");

    root->addSpacing(16);

    // ---- Footer: number-key hint + Refresh ----------------------------
    auto* footer = new QHBoxLayout();
    footer->setSpacing(7);
    footer->addWidget(make_keycap("1", this));
    auto* dot = new QLabel("·", this);
    dot->setObjectName("hint");
    footer->addWidget(dot);
    footer->addWidget(make_keycap("2", this));
    auto* sw = new QLabel("to switch", this);
    sw->setObjectName("hint");
    footer->addWidget(sw);
    footer->addStretch(1);
    refresh_btn_ = new QPushButton("Refresh", this);
    refresh_btn_->setObjectName("refresh");
    refresh_btn_->setCursor(Qt::PointingHandCursor);
    footer->addWidget(refresh_btn_);
    root->addLayout(footer);

    connect(refresh_btn_, &QPushButton::clicked, this, [this]() {
        if (on_refresh_) on_refresh_();
    });

    adjustSize();
}

void MonitorPanel::set_monitors(const std::vector<protocol::MonitorDesc>& monitors) {
    monitors_ = monitors;
    rebuild_thumbs();
}

void MonitorPanel::rebuild_thumbs() {
    // Clear the existing thumbnails.  Hide + detach NOW: deleteLater alone
    // is not enough — the CLI view loop pumps QApplication::processEvents(),
    // which does not deliver DeferredDelete, so "deleted" thumbs stayed
    // alive and painted UNDER the new generation (doubled labels, stale
    // green highlights — VIV-50 live testing).  Immediate delete is unsafe
    // here: rebuild is reached from the clicked thumb's own mousePressEvent.
    QLayoutItem* item;
    while ((item = strip_layout_->takeAt(0)) != nullptr) {
        if (auto* w = item->widget()) {
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete item;
    }

    count_label_->setText(monitors_.empty()
        ? QString()
        : QString("%1 display%2").arg(monitors_.size())
              .arg(monitors_.size() == 1 ? "" : "s"));

    // With 0 or 1 displays there's nothing to switch between — show a hint
    // instead of a lone non-interactive thumbnail.
    const bool switchable = monitors_.size() > 1;
    strip_->setVisible(switchable);
    empty_label_->setVisible(!switchable);
    refresh_btn_->setEnabled(true);

    if (switchable) {
        for (const auto& d : monitors_) {
            auto* thumb = new MonitorThumb(d, [this](uint32_t idx) { choose(idx); }, strip_);
            strip_layout_->addWidget(thumb);
        }
        strip_layout_->addStretch(1);
    }

    const QSize before = size();
    adjustSize();
    if (isVisible() && size() != before) {
        // Resizing a visible translucent frameless window leaves a DWM ghost
        // of the old geometry on Windows (repaint alone doesn't clear it).
        // Cycle the window: hide → recenter → show gives the layered surface
        // a clean start at the new size.  Rare in practice — the list is
        // pre-fetched on connect, so the panel normally opens full-sized.
        hide();
        if (auto* scr = screen()) {
            const QRect g = scr->geometry();
            move(g.center().x() - width() / 2, g.center().y() - height() / 2);
        }
        show();
        raise();
        activateWindow();
    }
}

void MonitorPanel::choose(uint32_t index) {
    if (on_select_) on_select_(index);
    // Optimistic local highlight: mark the chosen one viewing, clear the rest,
    // so the panel reflects the pick immediately.  The host re-advertises the
    // real list a moment later (after the encoder rebuild) via set_monitors().
    for (auto& m : monitors_) m.viewing = (m.index == index);
    rebuild_thumbs();
}

void MonitorPanel::open_over(QWidget* /*anchor*/) {
    adjustSize();
    // Centre on the screen the cursor / primary screen is on — the stream
    // window may be a native (non-Qt-anchored) surface.
    QScreen* scr = QGuiApplication::primaryScreen();
    if (scr) {
        const QRect g = scr->geometry();
        move(g.center().x() - width() / 2, g.center().y() - height() / 2);
    }
    show();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
}

void MonitorPanel::close_panel() {
    if (!isVisible()) return;
    hide();
    emit closed();
}

void MonitorPanel::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape
        || (e->key() == Qt::Key_F1 && (e->modifiers() & Qt::ControlModifier))) {
        close_panel();
        return;
    }
    // Number keys 1..9 switch to the Nth listed display.
    if (e->key() >= Qt::Key_1 && e->key() <= Qt::Key_9) {
        size_t n = static_cast<size_t>(e->key() - Qt::Key_1);
        if (n < monitors_.size() && !monitors_[n].viewing) {
            choose(monitors_[n].index);
        }
        return;
    }
    QWidget::keyPressEvent(e);
}

void MonitorPanel::changeEvent(QEvent* e) {
    if (e->type() == QEvent::ActivationChange && isVisible() && !isActiveWindow()) {
        close_panel();
    }
    QWidget::changeEvent(e);
}

void MonitorPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 18), 1));
    p.setBrush(kPanelBg);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);
}

} // namespace vivora
