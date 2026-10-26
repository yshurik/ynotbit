#pragma once
// A one-line label that gives way to the widgets beside it: when the room is short its text ends
// in "…", where a plain QLabel keeps its whole width and is cut off, or takes the room from them.
// (Not the ElidedLabel of the lock screen, which elides the middle of a path in a box of its own
// width and asks the layout for no room.)
#include <QFontMetrics>
#include <QLabel>
#include <QPainter>
#include <QStyle>

namespace bm {
class EllipsisLabel : public QLabel {
  public:
    using QLabel::QLabel;
    // The text as it is drawn at the width the label has.
    QString shownText() const {
        return fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width());
    }
    QSize minimumSizeHint() const override {
        return {fontMetrics().horizontalAdvance("…"), QLabel::minimumSizeHint().height()};
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        style()->drawItemText(&painter, contentsRect(), alignment(), palette(), isEnabled(),
                              shownText(), foregroundRole());
    }
};
} // namespace bm
