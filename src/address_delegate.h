#pragma once
// The cells of a table of addresses: the address book, the identities and the results of Find
// people draw their addresses alike.
#include "appearance.h"
#include <QStyledItemDelegate>

namespace bm {
// Draws a view's addresses in the address font, at the size of the view's own text:
// addressFont() alone has the platform's fixed-font size (10 px on macOS, beside text of 13), and
// a model's font keeps its size when the style sheet changes the view's. The first column holds
// addresses; any other cell that is one (a contact named by its address) gets it too.
class AddressDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

  protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (index.column() == 0 || index.data().toString().contains("BM-")) {
            option->font.setFamilies(addressFont().families());
            option->fontMetrics = QFontMetrics(option->font);
        }
    }
};
} // namespace bm
