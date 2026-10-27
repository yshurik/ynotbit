#pragma once
// Contacts > Find people: asks a nickbot for a nickname, an address or a name, and adds what is
// found to the address book. The answer comes in minutes, so the dialog says so and waits.
#include <QDialog>
#include <QString>
#include <QVariantMap>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
namespace bm {
class Session;

class FindPeopleDialog : public QDialog {
    Q_OBJECT
  public:
    explicit FindPeopleDialog(Session &session, QWidget *parent = nullptr);

  private:
    Session &session_;
    QComboBox *bot_;
    QLineEdit *field_;
    QPushButton *search_, *add_;
    QLabel *status_;
    QTreeWidget *results_;
    QString key_;  // what is asked, as the answer will name it; empty when nothing is
    QString kind_; // address, nickname or search
    void ask();
    void answered(const QString &kind, const QString &key, const QString &bot,
                  const QVariantMap &result);
    void showRow(const QString &nick, const QString &address, int botIndex);
    void updateButtons();
    void addSelected();
    QString ownKind(const QString &address) const;
    QString relation(const QString &address) const;
};
} // namespace bm
