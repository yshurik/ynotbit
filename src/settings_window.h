#pragma once
#include <QWidget>
class QCheckBox;
class QLabel;
class QListWidget;
class QStackedWidget;
namespace bm {
class Session;
// One window for every setting: sections on the left, the chosen section's
// page on the right. Changes apply at once; there is no OK button.
class SettingsWindow : public QWidget {
    Q_OBJECT
  public:
    explicit SettingsWindow(Session &session, QWidget *parent = nullptr);
    void showPage(const QString &id);

  private:
    Session &session_;
    QListWidget *sections_;
    QStackedWidget *pages_;
    QCheckBox *networkEnabled_;
    QLabel *usage_;
    void addPage(const QString &id, const QString &title, QWidget *page);
    QWidget *networkPage();
    QWidget *storagePage();
    void refresh();
};
} // namespace bm
