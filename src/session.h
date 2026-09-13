#pragma once
#include "cache.h"
#include "storage.h"
#include <QLockFile>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantList>
namespace bm {
class Session : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool unlocked READ unlocked NOTIFY changed)
    Q_PROPERTY(bool mailboxOpen READ mailboxOpen NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString document READ document NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY changed)
    Q_PROPERTY(QVariantList identities READ identities NOTIFY changed)
    Q_PROPERTY(qint64 objectCount READ objectCount NOTIFY changed)
    Q_PROPERTY(qint64 cacheBytes READ cacheBytes NOTIFY changed)
    Q_PROPERTY(QString activity READ activity NOTIFY changed)
    Vault vault_;
    Mailbox mailbox_;
    std::unique_ptr<Cache> cache_;
    QString root_, vaultPath_, mailPath_, mailKey_, error_, activity_;
    QVariantList messages_;
    QProcess node_;
    QTimer timer_;
    std::unique_ptr<QLockFile> nodeLock_, vaultLock_, mailLock_;
    bool busy_ = false;
    bool offline_ = false;
    void attempt(const std::function<void()> &f);
    void refresh();
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
    Q_INVOKABLE void clearError() {
        error_.clear();
        emit changed();
    }
  signals:
    void changed();
    void locked();
};
} // namespace bm
