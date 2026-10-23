#include "rail_badge.h"
#include <QFontMetrics>
#include <QPainter>
#include <algorithm>
namespace bm {
namespace {
constexpr int kHeight = 17; // the ring included
} // namespace
RailBadge::RailBadge(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents); // a click goes to the icon under it
    setFocusPolicy(Qt::NoFocus);
    auto digits = font();
    digits.setPixelSize(10);
    digits.setBold(true);
    setFont(digits);
    hide();
}
void RailBadge::setCount(int count) {
    count_ = std::max(0, count);
    text_ = !count_ ? QString() : count_ > 99 ? QString("99+") : QString::number(count_);
    if (!count_) {
        hide();
        return;
    }
    const int width = std::max(kHeight, QFontMetrics(font()).horizontalAdvance(text_) + 10);
    setFixedSize(width, kHeight);
    if (auto icon = parentWidget())
        move(icon->width() - width - 1, 1); // the icon's top right corner
    show();
    update();
}
void RailBadge::setDark(bool dark) {
    if (dark_ == dark)
        return;
    dark_ = dark;
    update();
}
void RailBadge::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF all(rect());
    p.setPen(Qt::NoPen);
    // The rail's own colour as a ring, so the badge stands off the icon under it, and the theme's
    // accent inside it: by the theme, not the palette, which may be changing as this is drawn.
    p.setBrush(QColor(dark_ ? "#141b23" : "#f5f7fa"));
    p.drawRoundedRect(all, all.height() / 2, all.height() / 2);
    const auto disc = all.adjusted(1.5, 1.5, -1.5, -1.5);
    p.setBrush(QColor(dark_ ? "#73d8c7" : "#126d65"));
    p.drawRoundedRect(disc, disc.height() / 2, disc.height() / 2);
    p.setPen(QColor(dark_ ? "#0d1b1f" : "#ffffff"));
    p.setFont(font());
    p.drawText(disc, Qt::AlignCenter, text_);
}
} // namespace bm
