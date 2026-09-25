#pragma once
#include "appearance.h"
#include <QMainWindow>
#include <QVariantMap>
class QListView;
class QListWidget;
class QLabel;
class QTextBrowser;
class QWidget;
class QVBoxLayout;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QAbstractItemDelegate;
namespace bm {
class Session;
class DesktopWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit DesktopWindow(Session &session);
    void compose(QVariantMap letter = {}, bool reply = false);
    void selectMessage(const QString &id);

  private:
    Session &session_;
    Appearance appearance_;
    QListView *letters_;
    QListWidget *folders_;
    QWidget *channelRail_;
    QVBoxLayout *channelChipLayout_;
    QString activeChannelAddress_;
    QString listDensity_;
    QAbstractItemDelegate *letterDelegate_ = nullptr;
    QLabel *listCountLabel_;
    QLineEdit *search_;
    QVariantList channelIdentities_;
    QLabel *heading_, *status_, *error_, *subject_;
    QLabel *fromAddress_, *toAddress_, *deliveryStatus_, *deliveryError_;
    QLabel *timeline_;
    QWidget *details_;
    QTextBrowser *body_;
    QWidget *actions_, *reader_, *identities_;
    QWidget *letterStripe_;
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
    void refreshIdentities();
    void refreshChannels();
    void setListDensity(QString density);
    void updateListCount();
    void showVaultPasswordFor(QString path, bool create);
    void updateLockedScreen();
    void refreshRecentVaults();
    void refreshRecentMailboxes();
};
} // namespace bm
