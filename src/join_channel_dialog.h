#pragma once
#include <QDialog>
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QWidget;
namespace bm {
// Joins a chan, or creates one: both are a shared phrase. A public chan's phrase is its
// name, shown as typed ("bitmessage" is #bitmessage). A secret chan has a name of its own
// and a phrase typed like a password.
class JoinChannelDialog : public QDialog {
    Q_OBJECT
  public:
    explicit JoinChannelDialog(bool dark, QWidget *parent = nullptr);
    ~JoinChannelDialog() override; // clears the phrase field
    QString phrase() const;
    QString name() const;            // "#<name>"
    QString expectedAddress() const; // trimmed; empty: no check, a version 4 chan

  private:
    void update();
    QRadioButton *public_, *secret_;
    QLineEdit *publicName_, *secretName_, *phrase_, *address_;
    QWidget *publicRow_, *secretRow_;
    QLabel *preview_;
    QPushButton *join_;
};
} // namespace bm
