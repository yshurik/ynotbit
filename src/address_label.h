#pragma once
// A label for an address. A QLabel that wraps reports as its size hint the width of its text broken
// into pieces (it tries a quarter of eighty characters, then a half), and a layout with room to
// spare gives it just that: an address was cut after "BM-" with the whole pane beside it. This one
// asks for its text on one line, and wraps only when the layout cannot give it that.
#include <QFontMetricsF>
#include <QLabel>
#include <cmath>

namespace bm {
class AddressLabel : public QLabel {
  public:
    using QLabel::QLabel;
    QSize sizeHint() const override {
        // The exact width of the text, rounded up and a pixel over: the nearest whole pixel is
        // sometimes a fraction short of what the text needs, and its last character went to a
        // second line.
        const int room = 2 * (margin() + frameWidth());
        const int width =
            int(std::ceil(QFontMetricsF(font()).horizontalAdvance(text()))) + 1 + room;
        return {width, heightForWidth(width)};
    }
};
} // namespace bm
