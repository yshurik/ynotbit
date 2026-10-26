#pragma once
// Settings > Nickbots: the nickbots a mailbox lists (the first is the default, each has a colour),
// the two switches, and the nickname of each identity with a button to register one.
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
    QCheckBox *enabled_, *automatic_;
    QTreeWidget *bots_, *identities_;
    QPushButton *add_, *remove_, *up_, *down_, *registerButton_;
    QLabel *note_, *problem_;
    void refresh();
    void addBot();
    void move(int by);
    void removeBot();
    void registerNickname();
};
} // namespace bm
