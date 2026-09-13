#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>
#include <map>
#include <memory>
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
    QStringList mailboxIds() const;
    const Secret &mailboxKey(const QString &) const;
};
class Mailbox {
    sqlite3 *db_ = nullptr;
    void connect(const QString &, const Secret &, bool create);
    void sql(const char *);

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
    void backup(const QString &, const Secret &);
};
} // namespace bm
