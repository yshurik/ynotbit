#pragma once
#include <QWidget>
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
namespace bm {
class Appearance;
class Session;
// One window for every setting: sections on the left, the chosen section's
// page on the right. Changes apply at once; there is no OK button.
class SettingsWindow : public QWidget {
    Q_OBJECT
  public:
    SettingsWindow(Session &session, Appearance &appearance, QString density,
                   QWidget *parent = nullptr);
    void showPage(const QString &id);
    // The letter list's density changed elsewhere (its own switch).
    void showDensity(const QString &density);
  signals:
    void densityChosen(QString density);

  private:
    Session &session_;
    Appearance &appearance_;
    QListWidget *sections_;
    QStackedWidget *pages_;
    QCheckBox *networkEnabled_, *updateNotices_, *listen_, *upnp_;
    QLabel *proxyNote_, *incoming_;
    QComboBox *density_;
    QLabel *usage_;
    QPushButton *changePassword_;
    void addPage(const QString &id, const QString &title, QWidget *page);
    QWidget *networkPage();
    QWidget *storagePage();
    QWidget *sendingPage();
    QWidget *appearancePage(const QString &density);
    QWidget *notificationsPage();
    QWidget *securityPage();
    void refresh();
};
} // namespace bm
