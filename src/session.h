#pragma once
#include "cache.h"
#include "delivery.h"
#include "message_model.h"
#include "storage.h"
#include <QJsonObject>
#include <QLockFile>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantList>
namespace bm {
class Session : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool networkEnabled READ networkEnabled NOTIFY changed)
    Q_PROPERTY(bool unlocked READ unlocked NOTIFY changed)
    Q_PROPERTY(bool mailboxOpen READ mailboxOpen NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString document READ document NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(MessageModel *messageModel READ messageModel CONSTANT)
    Q_PROPERTY(QVariantList identities READ identities NOTIFY changed)
    Q_PROPERTY(qint64 objectCount READ objectCount NOTIFY changed)
    Q_PROPERTY(qint64 cacheBytes READ cacheBytes NOTIFY changed)
    Q_PROPERTY(QString activity READ activity NOTIFY changed)
    Q_PROPERTY(QVariantList subscriptions READ subscriptions NOTIFY changed)
    Vault vault_;
    Mailbox mailbox_;
    std::unique_ptr<Cache> cache_;
    std::unique_ptr<Delivery> delivery_;
    std::unique_ptr<MessageModel> messageModel_;
    QString root_, vaultPath_, mailPath_, mailKey_, pendingVaultPath_, error_, activity_;
    QStringList recentVaultPaths_, recentMailboxPaths_;
    std::optional<quint64> displayedRevision_;
    QProcess node_;
    QTimer timer_;
    std::unique_ptr<QLockFile> nodeLock_, vaultLock_, mailLock_;
    bool busy_ = false;
    bool offline_ = false;
    mutable QJsonObject lastNodeStatus_; // last status.json read, see status()
    int retentionMB_ = 512, retentionDays_ = 90;
    void startNode();
    void attempt(const std::function<void()> &f);
    void refresh();
    void clearMessages();
    void tick();
    void acquireVault(const QString &);
    void openMailboxPath(const QString &);
    void rememberVault(const QString &path);
    void rememberMailbox(const QString &path);

  public:
    Session(QString dataRoot, bool offline = false, QObject *parent = nullptr);
    ~Session();
    bool unlocked() const {
        return vault_.unlocked();
    }
    bool mailboxOpen() const {
        return mailbox_.isOpen();
    }
    QString status() const;
    QString document() const;
    QString error() const {
        return error_;
    }
    QString activity() const {
        return activity_;
    }
    MessageModel *messageModel() const { return messageModel_.get(); }
    QVariantList identities() const;
    qint64 objectCount() const {
        return cache_ ? cache_->count() : 0;
    }
    qint64 cacheBytes() const {
        return cache_ ? cache_->bytes() : 0;
    }
    bool networkEnabled() const {
        return !offline_;
    }
    Q_INVOKABLE void setNetworkEnabled(bool enabled);
    Q_INVOKABLE void configureNode();
    Q_INVOKABLE void configureRetention();
    Q_INVOKABLE void restartNode();
    Q_INVOKABLE void createVault();
    Q_INVOKABLE void openVault();
    Q_INVOKABLE void unlockVault();
    Q_INVOKABLE void beginVaultCreate();
    Q_INVOKABLE void beginVaultOpen();
    Q_INVOKABLE void beginVaultUnlock();
    Q_INVOKABLE void submitVaultPassword(QString password, QString repeat, bool create);
    Q_INVOKABLE void choosePendingVault(QString path) {
        pendingVaultPath_ = path;
    }
    Q_INVOKABLE void createMailbox();
    Q_INVOKABLE void openMailbox();
    Q_INVOKABLE void openMailboxAt(QString path);
    QString vaultPath() const {
        return vaultPath_;
    }
    QString mailPath() const {
        return mailPath_;
    }
    QVariantList recentVaults() const;
    QVariantList recentMailboxes() const;
    Q_INVOKABLE void lock();
    Q_INVOKABLE void addIdentity();
    Q_INVOKABLE void joinChannel();
    Q_INVOKABLE void importIdentities();
    Q_INVOKABLE void changePassword();
    Q_INVOKABLE void backup();
    Q_INVOKABLE void saveDraft(QString recipient, QString subject, QString body);
    Q_INVOKABLE void rescan();
    Q_INVOKABLE QString saveLetter(QString id, QString from, QString to, QString subject,
                                   QString body, QString kind);
    Q_INVOKABLE bool sendLetter(QString id);
    Q_INVOKABLE void retryLetter(QString id);
    Q_INVOKABLE void cancelLetter(QString id);
    Q_INVOKABLE void moveLetter(QString id, QString folder);
    Q_INVOKABLE void restoreLetter(QString id);
    Q_INVOKABLE void deleteLetter(QString id);
    Q_INVOKABLE void readLetter(QString id);
    Q_INVOKABLE QVariantMap message(QString id) const;
    QHash<QString, QString> names() const;
    static QString errorText(const std::exception &e);
    QVariantList messagePage(const QString &folder, const QString &search, int offset, int limit, const QString &recipient = {}, bool unreadOnly = false, bool anonymousOnly = false) const;
    int messageCount(const QString &folder, const QString &search, const QString &recipient = {}, bool unreadOnly = false, bool anonymousOnly = false) const { return mailboxOpen() ? mailbox_.messageCount(folder, search, recipient, unreadOnly, anonymousOnly) : 0; }
    QVariantList channels() const;
    bool channelUnread(QString address) const {
        return mailboxOpen() && mailbox_.channelUnread(address);
    }
    Q_INVOKABLE QVariantList deliveryHistory(QString id);
    Q_INVOKABLE void subscribe();
    Q_INVOKABLE void unsubscribe(QString address);
    Q_INVOKABLE void renameIdentity(QString address);
    Q_INVOKABLE void deleteIdentity(QString address);
    Q_INVOKABLE void setDefaultIdentity(QString address);
    Q_INVOKABLE void copyAddress(QString address);
    Q_INVOKABLE void closeMailbox();
    QVariantList subscriptions() const;
    // ynotbit release announcements: the newer version announced and not yet
    // dismissed, or empty.
    QString availableUpdate() const;
    Q_INVOKABLE void dismissUpdate();
    bool updateNotices() const;
    Q_INVOKABLE void setUpdateNotices(bool on);
    // The address book (kept in the open mailbox). Names are local and private.
    QVariantList contacts() const;
    // Why the address cannot be added as a contact, or empty when it can.
    QString contactProblem(QString address) const;
    // Adds or renames a contact; an empty name falls back to the address.
    // Returns false (with error() set) when the address is refused.
    Q_INVOKABLE bool addContact(QString address, QString label);
    Q_INVOKABLE void removeContact(QString address);
    bool isContact(QString address) const;
    // Best local name for an address -- own identity or chan, then contact,
    // then subscription -- or empty when it has none.
    Q_INVOKABLE QString nameFor(QString address) const;
    Q_INVOKABLE void clearError() {
        error_.clear();
        emit changed();
    }
  signals:
    void changed();
    void messagesChanged();
    void messageRead(QString id);
    void locked();
    void aboutToCloseMailbox();
    void vaultPasswordRequired(QString path, bool create);
    void vaultPasswordAccepted();
};
} // namespace bm
