#pragma once
#include "cache.h"
#include "delivery.h"
#include "storage.h"
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
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
    Q_PROPERTY(QVariantList identities READ identities NOTIFY changed)
    Q_PROPERTY(qint64 objectCount READ objectCount NOTIFY changed)
    Q_PROPERTY(qint64 cacheBytes READ cacheBytes NOTIFY changed)
    Q_PROPERTY(QString activity READ activity NOTIFY changed)
    Q_PROPERTY(QVariantList subscriptions READ subscriptions NOTIFY changed)
    Vault vault_;
    Mailbox mailbox_;
    std::unique_ptr<Cache> cache_;
    std::unique_ptr<Delivery> delivery_;
    QString root_, vaultPath_, mailPath_, mailKey_, error_, activity_;
    QVariantList messages_;
    std::optional<quint64> displayedRevision_;
    QProcess node_;
    QTimer timer_;
    std::unique_ptr<QLockFile> nodeLock_, vaultLock_, mailLock_;
    bool busy_ = false;
    bool offline_ = false;
    int retentionMB_ = 512, retentionDays_ = 90;
    void startNode();
    void attempt(const std::function<void()> &f);
    void refresh();
    void clearMessages();
    void tick();
    void acquireVault(const QString &);
    void openMailboxPath(const QString &);

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
    QVariantList messages() const {
        return messages_;
    }
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
    Q_INVOKABLE void createMailbox();
    Q_INVOKABLE void openMailbox();
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
    Q_INVOKABLE QVariantList deliveryHistory(QString id);
    Q_INVOKABLE void subscribe();
    Q_INVOKABLE void unsubscribe(QString address);
    Q_INVOKABLE void renameIdentity(QString address);
    Q_INVOKABLE void copyAddress(QString address);
    Q_INVOKABLE void closeMailbox();
    QVariantList subscriptions() const;
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
};
} // namespace bm
