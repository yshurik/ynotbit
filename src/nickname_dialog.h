#pragma once
// The dialog that asks a nickbot for the nickname of one of the user's identities: the nickname,
// the nickbot when several are listed, and how the last try ended.
#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
namespace bm {
class Session;

class NicknameDialog : public QDialog {
    Q_OBJECT
  public:
    NicknameDialog(Session &session, const QString &address, const QString &label,
                   QWidget *parent = nullptr);

  protected:
    // Asks for the nickname. When the request cannot be made the dialog stays, and says why.
    void accept() override;

  private:
    void check();
    Session &session_;
    QString address_;
    QString standing_; // the nickname the identity has now, or empty
    QLineEdit *field_;
    QComboBox *which_ = nullptr; // only when several nickbots are listed
    QLabel *state_, *problem_;
    QPushButton *register_;
};
} // namespace bm
