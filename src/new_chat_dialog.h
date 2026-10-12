#pragma once
#include <QDialog>
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
namespace bm {
class Session;
// Who to chat with, and from which identity. A chat keeps its identity: another one is
// another chat. So it is chosen here, once, and asked only when the vault has several.
class NewChatDialog : public QDialog {
    Q_OBJECT
  public:
    NewChatDialog(Session &session, bool dark, QWidget *parent = nullptr);
    QString peer() const;     // the address typed or chosen, trimmed
    QString identity() const; // the identity the chat is under

  private:
    void validate();
    Session &session_;
    QLineEdit *peer_;
    QComboBox *identity_;
    QLabel *problem_;
    QPushButton *start_;
};
} // namespace bm
