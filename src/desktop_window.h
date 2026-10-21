#pragma once
#include "appearance.h"
#include "letter_render.h"
#include "sound_notifier.h"
#include <QHash>
#include <QMainWindow>
#include <QVariantMap>
#include <functional>
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
class QSortFilterProxyModel;
class QStandardItemModel;
class QToolButton;
class QTreeView;
namespace bm {
class Session;
class FeedView;
class ChatsPane;
class SettingsPane;
class LetterPane;
class TrayIcon;
class DesktopWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit DesktopWindow(Session &session, QString soundThemesDir = {});
    void compose(QVariantMap letter = {}, bool reply = false, bool forward = false);
    void selectMessage(const QString &id);
    // The sound theme preferences and playback, for the settings page and tests.
    SoundNotifier &soundNotifier() { return sounds_; }
    // Opens a chat between one of the vault's identities and a peer: listed first and
    // empty until the first message, which makes it a chat of the mailbox.
    void startChat(const QString &local, const QString &peer);
    // Brings the window forward from wherever it is: hidden in the notification area, minimized,
    // behind others.
    void bringToFront();
    // The icon in the notification area, where the window goes when it is closed.
    TrayIcon *trayIcon() const { return tray_; }
    // For tests: what Quit does, in place of ending the app. Empty is the real thing.
    static void setQuitHandlerForTesting(std::function<void()> handler);

  protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;

  private:
    Session &session_;
    TrayIcon *tray_ = nullptr;
    Appearance appearance_;
    SoundNotifier sounds_;
    QListView *letters_;
    QListWidget *folders_;
    int lastFolder_ = 0; // the folder Settings goes back to without a mailbox
    QWidget *channelRail_;
    QVBoxLayout *channelChipLayout_;
    QString railShown_; // which chans the rail's chips were made for, and how
    QString activeChannelAddress_, activeBroadcastAddress_;
    QString activeChatKey_;    // the open chat: its identity and its peer (Session::chatKey)
    QString chatWindowKey_;    // the chat that chatWindow_ is the window of
    int chatWindow_ = 0;       // how many of its newest messages the open chat shows
    QStringList startedChats_; // chats started here and not yet written in, newest first
    QString listDensity_;
    QHash<QString, bool> railCollapsed_; // for each page of the rail (railPage()): is it collapsed
    QAbstractItemDelegate *letterDelegate_ = nullptr;
    QLabel *listCountLabel_;
    QLineEdit *search_;
    QProgressBar *searchProgress_;
    QVariantList channelIdentities_;
    QVariantList shownContacts_; // the address book the views were last drawn with
    QVariantList shownSubscriptions_; // the subscriptions the views were last drawn with
    QVariantList shownBroadcastSources_; // the Broadcasts rail's senders, last drawn
    QVariantList shownChats_; // the Chats rail's peers and the open conversation, last drawn
    QLabel *heading_, *status_, *error_, *updateLabel_;
    QWidget *updateBanner_;
    QAction *networkEnabledAction_, *createIdentityAction_;
    SettingsPane *settings_ = nullptr; // made when Settings is first opened
    LetterPane *pane_; // the selected letter
    QWidget *reader_, *identities_, *contacts_;
    ChatsPane *chats_ = nullptr;
    FeedView *feed_ = nullptr; // the Subscriptions page: one sender's posts as a feed
    QLineEdit *contactsFilter_;
    QTreeView *contactsTree_;
    QStandardItemModel *contactsModel_;     // the address book's rows: Address, Nickname, Local name
    QSortFilterProxyModel *contactsSorted_; // the same rows, sorted by a click on a header
    QLabel *contactsEmpty_;                 // stands in for the tree when no row is listed
    // The buttons for the selected contact, in the toolbar beside Add contact.
    QPushButton *contactWrite_;
    QToolButton *contactCopy_, *contactRename_, *contactDelete_;
    QWidget *contactActions_; // the pill of the last three
    QWidget *listColumn_;
    QStackedWidget *welcomeStack_;
    QWidget *lockedPage_, *noMailboxPage_;
    QLabel *lockedVaultName_, *lockedVaultPath_;
    QLineEdit *vaultPasswordField_;
    QPushButton *vaultUnlockButton_;
    QVBoxLayout *lockedRecentsLayout_, *noMailboxRecentsLayout_;
    QVBoxLayout *identityLayout_;
    QString targetVaultPath_;
    void updateState();
    void updateTray(); // the icon's menu follows the window and the mailbox
    void quitApp();
    void updateTheme();
    // Add (or, with a label, rename) an address-book entry; true when saved.
    bool editContact(QString address, QString label = {});
    void refreshIdentities();
    void refreshContacts();
    // The selected contact's address (column 0) or local name (column 2); empty with no row
    // selected.
    QString selectedContact(int column) const;
    void updateContactActions(); // the toolbar's buttons are on while a row is selected
    void refreshChannels();
    // The open chat's messages: a window of its newest ones; *older says more are kept.
    QVariantList chatThread(bool *older);
    void updateRailTexts(); // the rail's width, heading and buttons for its page
    // The page the one rail is for, by the name of its folder: Channels, Broadcasts (shown as
    // Subscriptions) or Chats. Each keeps its own collapse.
    QString railPage() const;
    // Whether the rail is collapsed on the page now shown.
    bool railCollapsed() const;
    void updateChatsIcon(bool force = false); // the Chats button's dot for unread chats
    void setListDensity(QString density);
    void openSettings();
    void setChannelRailCollapsed(bool collapsed); // for the page now shown; the others keep theirs
    void updateListCount();
    void showVaultPasswordFor(QString path);
    void newVault();    // the new vault dialog
    void newIdentity(); // the same dialog, for an identity alone
    void newChat();     // who to, and from which identity, then startChat()
    void joinChannel(); // public or secret, then Session::joinChannel
    void updateLockedScreen();
    void refreshRecentVaults();
    void refreshRecentMailboxes();
};
} // namespace bm
