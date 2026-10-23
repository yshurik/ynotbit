#pragma once
#include <QHash>
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
class SoundNotifier;
// Every setting, as a page of the main window: sections on the left, the
// chosen section's page on the right. Changes apply at once; there is no OK
// button.
class SettingsPane : public QWidget {
    Q_OBJECT
  public:
    SettingsPane(Session &session, Appearance &appearance, QString density, SoundNotifier &sounds,
                 QWidget *parent = nullptr);
    void showPage(const QString &id);
    // The letter list's density changed elsewhere (its own switch).
    void showDensity(const QString &density);
  signals:
    void densityChosen(QString density);

  private:
    Session &session_;
    Appearance &appearance_;
    SoundNotifier &sounds_;
    QListWidget *sections_;
    QStackedWidget *pages_;
    QCheckBox *networkEnabled_, *updateNotices_, *listen_, *upnp_, *soundEnabled_, *badgeEnabled_;
    QLabel *proxyNote_, *incoming_, *upnpStatus_;
    QComboBox *density_;
    QLabel *usage_;
    QPushButton *changePassword_, *addSoundTheme_;
    QListWidget *soundThemes_;
    QHash<QString, QCheckBox *> soundEvents_; // event id -> the box that turns it on
    void addPage(const QString &id, const QString &title, QWidget *page);
    QWidget *networkPage();
    QWidget *storagePage();
    QWidget *sendingPage();
    QWidget *appearancePage(const QString &density);
    QWidget *notificationsPage();
    QWidget *securityPage();
    void refreshSoundThemes(); // rebuild the theme list from disk
    void addSoundTheme();      // pick a ZIP, unpack it, select it
    void refresh();
};
} // namespace bm
