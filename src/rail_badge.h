#pragma once
#include <QWidget>
namespace bm {
// A round count on the corner of a rail icon: the theme's accent on a ring of the rail's colour,
// the number inside ("99+" past 99); not there at zero.
class RailBadge : public QWidget {
    Q_OBJECT
  public:
    explicit RailBadge(QWidget *parent);
    void setCount(int count);
    void setDark(bool dark);
    int count() const {
        return count_;
    }
    QString text() const {
        return text_;
    }
    bool dark() const {
        return dark_;
    }

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    int count_ = 0;
    bool dark_ = false;
    QString text_;
};
} // namespace bm
