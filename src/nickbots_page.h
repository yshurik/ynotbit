#pragma once
// Settings > Nickbots: the nickbots a mailbox lists (the first is the default, each has a colour),
// and the two switches. The nicknames of the user's own identities are on the Identities page.
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QTreeWidget;
namespace bm {
class Session;

class NickbotsPage : public QWidget {
    Q_OBJECT
  public:
    explicit NickbotsPage(Session &session, QWidget *parent = nullptr);

  private:
    Session &session_;
    QCheckBox *enabled_;
    QTreeWidget *bots_;
    QPushButton *add_, *remove_, *up_, *down_;
    QLabel *note_;
    void refresh();
    void addBot();
    void move(int by);
    void removeBot();
};
} // namespace bm
