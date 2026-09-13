#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>
#include <map>
#include <memory>
#include <optional>
#include <sodium.h>
#include <vector>
struct sqlite3;
namespace bm {
class Secret {
    unsigned char *p_ = nullptr;
    size_t n_ = 0;

  public:
    explicit Secret(size_t n = 0);
    ~Secret();
    Secret(Secret &&other) noexcept;
    Secret &operator=(Secret &&other) noexcept;
    Secret(const Secret &) = delete;
    Secret &operator=(const Secret &) = delete;
    unsigned char *data() {
        return p_;
    }
    const unsigned char *data() const {
        return p_;
    }
    size_t size() const {
        return n_;
    }
    void clear();
};
struct Identity {
    QString label, address;
    bool chan = false;
    Secret keys{64};
    Identity() = default;
    Identity(Identity &&) = default;
    Identity &operator=(Identity &&) = default;
};
struct Message {
    QString hash, from, to, subject, body, folder;
    qint64 received = 0;
};
class Vault {
    QString path_;
    Secret wrapping_;
    QByteArray salt_;
    bool unlocked_ = false;
    std::vector<Identity> identities_;
    std::map<QString, Secret> mailboxKeys_;
    void save();

  public:
    ~Vault() {
        lock();
    }
    void create(const QString &, const QByteArray &password);
    void unlock(const QString &, const QByteArray &password);
    void changePassword(const QByteArray &password);
    void lock();
    bool unlocked() const {
        return unlocked_;
    }
    const std::vector<Identity> &identities() const {
        return identities_;
    }
    QString addIdentity(const QString &label);
    QString addChannel(const QString &phrase, const QString &label,
                       const QString &expectedAddress = {});
    void importKeys(const QString &path);
    QString addMailboxKey();
    void renameIdentity(const QString &address, const QString &label);
    QStringList mailboxIds() const;
    const Secret &mailboxKey(const QString &) const;
};
struct OutboxItem;
struct NetworkJob;
struct StoredPublicKey;
struct Subscription;
struct DeliveryEvent;
class Mailbox {
    sqlite3 *db_ = nullptr;
    quint64 messageRevision_ = 0;
    void connect(const QString &, const Secret &, bool create);
    void sql(const char *);
    void migrate();
    void deliveryUpdate(const QString &id, const QString &state, const QString &detail,
                        qint64 nextAttempt);

  public:
    ~Mailbox() {
        close();
    }
    void create(const QString &, const QString &keyId, const Secret &);
    void open(const QString &, const Secret &);
    void close();
    bool isOpen() const {
        return db_ != nullptr;
    }
    QString keyId() const;
    void bindCache(const QString &cacheId);
    void bindIdentities(const QString &fingerprint);
    void store(const QString &hash, const QString &from, const QString &to, const QString &subject,
               const QString &body, qint64 checkpoint, const QString &folder = "Inbox");
    void advance(qint64 checkpoint);
    qint64 checkpoint() const;
    QVector<Message> messages() const;
    QVector<Message> messageSummaries(int limit = 2000) const;
    QVector<Message> messageSummaries(const QString &folder, const QString &search, int offset,
                                      int limit) const;
    // Changes affecting the desktop message snapshot, excluding scan checkpoints/jobs.
    quint64 messageRevision() const {
        return messageRevision_;
    }
    void backup(const QString &, const Secret &);
    QString saveDraft(const QString &id, const QString &from, const QString &to,
                      const QString &subject, const QString &body);
    Message message(const QString &id) const;
    void queueDraft(const QString &id, const QString &kind, qint64 expires);
    QVector<OutboxItem> outbox() const;
    OutboxItem outgoing(const QString &id) const;
    void setDelivery(const QString &id, const QString &state, const QString &detail = {},
                     qint64 nextAttempt = 0);
    void setAck(const QString &id, const QByteArray &token, const QByteArray &object = {});
    void setObjectHash(const QString &id, const QString &hash);
    bool acknowledge(const QByteArray &token);
    void retry(const QString &id, qint64 expires);
    void cancel(const QString &id);
    QVector<DeliveryEvent> events(const QString &id) const;
    QString addJob(NetworkJob job);
    QVector<NetworkJob> jobs() const;
    void updateJob(const QString &id, const QString &state, const QByteArray &payload = {},
                   const QString &hash = {}, const QString &error = {});
    void removeJob(const QString &id);
    void savePublicKey(const StoredPublicKey &key);
    std::optional<StoredPublicKey> publicKey(const QString &address, qint64 now) const;
    QVector<Subscription> subscriptions() const;
    void subscribe(const QString &address, const QString &label);
    void unsubscribe(const QString &address);
    void moveMessage(const QString &id, const QString &folder);
    void restoreMessage(const QString &id);
    void deleteMessage(const QString &id);
    void markRead(const QString &id);
    bool unread(const QString &id) const;
    QString setting(const QString &name, const QString &fallback = {}) const;
    void setSetting(const QString &name, const QString &value);
};
} // namespace bm

namespace bm {
struct OutboxItem {
    QString id, kind, state, error, objectHash;
    QByteArray ackToken, ackObject;
    qint64 expires = 0, nextAttempt = 0;
    int attempts = 0;
    Message message;
};
struct NetworkJob {
    QString id, owner, kind, address, state, hash, error;
    QByteArray payload;
    qint64 expires = 0;
    quint64 nonceTrials = 1000, extraBytes = 1000;
};
struct StoredPublicKey {
    QString address;
    QByteArray signingKey, encryptionKey;
    quint64 nonceTrials = 1000, extraBytes = 1000;
    quint32 behaviors = 1;
    qint64 expires = 0;
};
struct Subscription {
    QString address, label;
};
struct DeliveryEvent {
    qint64 timestamp;
    QString state, detail;
};
} // namespace bm
