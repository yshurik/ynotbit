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
    QAction *updateNoticesAction_;
    QTextEdit *subject_;
    QLabel *fromAddress_, *toAddress_, *deliveryStatus_, *deliveryError_;
    QLabel *fromName_, *toName_, *toLabel_;
    QWidget *addFromContact_, *addToContact_;
    QLabel *timeline_;
    QWidget *details_;
    QTextBrowser *body_;
    QWidget *actions_, *reader_, *identities_, *contacts_;
    FeedView *feed_ = nullptr; // the Subscriptions page: one sender's posts as a feed
    QVBoxLayout *contactLayout_;
    QLineEdit *contactsFilter_;
    QWidget *letterStripe_;
    QWidget *viewSwitch_;
    QWidget *sidebarWidget_, *listColumn_;
    QStackedWidget *welcomeStack_;
    QWidget *lockedPage_, *noMailboxPage_;
    QLabel *lockedVaultName_, *lockedVaultPath_;
    QLineEdit *vaultPasswordField_, *vaultRepeatField_;
    QPushButton *vaultUnlockButton_;
    QVBoxLayout *lockedRecentsLayout_, *noMailboxRecentsLayout_;
    QVBoxLayout *identityLayout_;
    QVariantMap selected_;
    QString targetVaultPath_;
    bool vaultCreateMode_ = false;
    void updateState();
    void updateTheme();
    void clearDetails();
    void renderSelectedBody();
    void updateDeliveryStatus();
    void updateTimeline();
    void updateCorrespondents();
    // Add (or, with a label, rename) an address-book entry; true when saved.
    bool editContact(QString address, QString label = {});
    void refreshIdentities();
    void refreshContacts();
    void refreshChannels();
    void updateRailTexts(); // the rail heading and add button for its page
    void setListDensity(QString density);
    void setChannelRailCollapsed(bool collapsed);
    void updateListCount();
    void showVaultPasswordFor(QString path, bool create);
    void updateLockedScreen();
    void refreshRecentVaults();
    void refreshRecentMailboxes();
};
} // namespace bm
