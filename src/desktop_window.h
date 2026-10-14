#pragma once
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
class ChatsPane;
class SettingsPane;
class LetterPane;
class DesktopWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit DesktopWindow(Session &session);
    void compose(QVariantMap letter = {}, bool reply = false, bool forward = false);
    void selectMessage(const QString &id);
    // Opens a chat between one of the vault's identities and a peer: listed first and
    // empty until the first message, which makes it a chat of the mailbox.
    void startChat(const QString &local, const QString &peer);

  private:
    Session &session_;
    Appearance appearance_;
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
    bool channelRailCollapsed_ = false;
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
    QVBoxLayout *contactLayout_;
    QLineEdit *contactsFilter_;
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
    void updateTheme();
    // Add (or, with a label, rename) an address-book entry; true when saved.
    bool editContact(QString address, QString label = {});
    void refreshIdentities();
    void refreshContacts();
    void refreshChannels();
    // The open chat's messages: a window of its newest ones; *older says more are kept.
    QVariantList chatThread(bool *older);
    void updateRailTexts(); // the rail heading and add button for its page
    void updateChatsIcon(bool force = false); // the Chats button's dot for unread chats
    void setListDensity(QString density);
    void openSettings();
    void setChannelRailCollapsed(bool collapsed);
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
