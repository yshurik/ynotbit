#pragma once
#include <QPointer>
#include "appearance.h"
#include "letter_render.h"
#include <QMainWindow>
#include <QVariantMap>
class QListView;
class QListWidget;
class QLabel;
class QProgressBar;
class QTextBrowser;
class QTextEdit;
class QWidget;
class QAction;
class QVBoxLayout;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QAbstractItemDelegate;
namespace bm {
class Session;
class FeedView;
class SettingsWindow;
class LetterPane;
class DesktopWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit DesktopWindow(Session &session);
    void compose(QVariantMap letter = {}, bool reply = false, bool forward = false);
    void selectMessage(const QString &id);

  private:
    Session &session_;
    Appearance appearance_;
    QListView *letters_;
    QListWidget *folders_;
    QWidget *channelRail_;
    QVBoxLayout *channelChipLayout_;
    QString railShown_; // which chans the rail's chips were made for, and how
    QString activeChannelAddress_, activeBroadcastAddress_;
    QString listDensity_;
    bool channelRailCollapsed_ = false;
    QAbstractItemDelegate *letterDelegate_ = nullptr;
    QLabel *listCountLabel_;
    QLineEdit *search_;
    QProgressBar *searchProgress_;
    QVariantList channelIdentities_;
    QVariantList shownContacts_; // the address book the views were last drawn with
    QVariantList shownSubscriptions_; // the subscriptions the views were last drawn with
    QVariantList shownBroadcastSources_; // the Broadcasts rail's senders, last drawn
    QLabel *heading_, *status_, *error_, *updateLabel_;
    QWidget *updateBanner_;
    QAction *networkEnabledAction_;
    QPointer<SettingsWindow> settings_;
    LetterPane *pane_; // the selected letter
    QWidget *reader_, *identities_, *contacts_;
    FeedView *feed_ = nullptr; // the Subscriptions page: one sender's posts as a feed
    QVBoxLayout *contactLayout_;
    QLineEdit *contactsFilter_;
    QWidget *sidebarWidget_, *listColumn_;
    QStackedWidget *welcomeStack_;
    QWidget *lockedPage_, *noMailboxPage_;
    QLabel *lockedVaultName_, *lockedVaultPath_;
    QLineEdit *vaultPasswordField_, *vaultRepeatField_;
    QPushButton *vaultUnlockButton_;
    QVBoxLayout *lockedRecentsLayout_, *noMailboxRecentsLayout_;
    QVBoxLayout *identityLayout_;
    QString targetVaultPath_;
    bool vaultCreateMode_ = false;
    void updateState();
    void updateTheme();
    // Add (or, with a label, rename) an address-book entry; true when saved.
    bool editContact(QString address, QString label = {});
    void refreshIdentities();
    void refreshContacts();
    void refreshChannels();
    void updateRailTexts(); // the rail heading and add button for its page
    void setListDensity(QString density);
    void openSettings();
    void setChannelRailCollapsed(bool collapsed);
    void updateListCount();
    void showVaultPasswordFor(QString path, bool create);
    void updateLockedScreen();
    void refreshRecentVaults();
    void refreshRecentMailboxes();
};
} // namespace bm
