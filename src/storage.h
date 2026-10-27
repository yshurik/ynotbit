#pragma once
#include <QByteArray>
#include <QMap>
#include <QPair>
#include <QString>
#include <QVector>
#include <bmc/mailbox.h>
#include <bmc/secret.h>
#include <bmc/vault.h>
#include <map>
#include <memory>
#include <optional>
#include <sodium.h>
#include <vector>
struct sqlite3;
namespace bm::detail {
class Transaction;
}
namespace bm {
// Letter sizes. Bitmessage nodes drop any object over 2^18 bytes. A letter's
// text -- "Subject:<subject>\nBody:<body>" in UTF-8 -- may take kMaxLetterText
// of them; the rest is the object header, the sender's keys, the acknowledgment,
// the signature and the encryption: 486 bytes at most for a msg (see
// Wire::encodeMessage), fewer for a broadcast, with some slack on top.
constexpr int kMaxObjectBytes = 262144;
constexpr int kMaxLetterText = 261000;
inline int letterTextBytes(const QString &subject, const QString &body) {
    return int(("Subject:" + subject + "\nBody:" + body).toUtf8().size());
}
// Drafts stay on this device, so they may run well past what the network
// carries: a letter that grew too large can always be kept and trimmed.
constexpr int kMaxDraftBytes = 4 * 1024 * 1024;
using Secret = bmc::Secret;
struct Identity {
    QString label, address;
    bool chan = false;
    bool isDefault = false;
    Secret keys{64};
    Identity() = default;
    Identity(Identity &&) = default;
    Identity &operator=(Identity &&) = default;
};
struct Message {
    QString hash, from, to, subject, body, folder;
    qint64 received = 0;
};
struct ChatMessage {
    QString id, localAddress, peerAddress, body, state;
    qint64 seqno = 0, received = 0, expires = 0;
    bool outgoing = false;
};
// A conversation as a list shows it: its newest message and what is unread.
struct ChatSummary {
    QString local, peer, body, state; // the newest message's text and delivery state
    qint64 received = 0;              // when it came, or was queued
    bool outgoing = false;            // it is ours
    int unread = 0;                   // messages from the peer that nobody has looked at
};
// A chan's name as the app shows it: "#<name>". PyBitmessage's default
// "[chan] <phrase>" and a bare "<name>" both become "#<name>".
QString channelName(const QString &label);
class Vault {
    std::shared_ptr<bmc::Vault> core_ = std::make_shared<bmc::Vault>();

  public:
    Vault() = default;
    explicit Vault(std::shared_ptr<bmc::Vault> core) : core_(std::move(core)) {}
    Vault(const Vault &) = delete;
    Vault &operator=(const Vault &) = delete;
    bmc::Vault &core() const {
        return *core_;
    }
    const std::shared_ptr<bmc::Vault> &corePtr() const {
        return core_;
    }
    ~Vault() {
        lock();
    }
    void create(const QString &, const QByteArray &password);
    void unlock(const QString &, const QByteArray &password);
    void changePassword(const QByteArray &password);
    void lock();
    bool unlocked() const {
        return core_->unlocked();
    }
    std::vector<Identity> identities() const;
    QString addIdentity(const QString &label);
    // An identity made beforehand (the new vault dialog's): its keys are checked
    // against its address and kept as they are.
    QString addIdentity(const Identity &identity);
    // label empty: "#<phrase>"; any other label is made a channelName().
    QString addChannel(const QString &phrase, const QString &label,
                       const QString &expectedAddress = {});
    void importKeys(const QString &path);
    QString addMailboxKey();
    void renameIdentity(const QString &address, const QString &label);
    void deleteIdentity(const QString &address);
    void setDefaultIdentity(const QString &address);
    QStringList mailboxIds() const;
    const Secret &mailboxKey(const QString &) const;
};
struct OutboxItem;
struct NetworkJob;
struct StoredPublicKey;
struct Subscription;
struct Contact;
struct DeliveryEvent;
class Mailbox {
    std::shared_ptr<bmc::Mailbox> core_ = std::make_shared<bmc::Mailbox>();

  public:
    Mailbox() = default;
    explicit Mailbox(std::shared_ptr<bmc::Mailbox> core) : core_(std::move(core)) {}
    Mailbox(const Mailbox &) = delete;
    Mailbox &operator=(const Mailbox &) = delete;
    bmc::Mailbox &core() const {
        return *core_;
    }
    const std::shared_ptr<bmc::Mailbox> &corePtr() const {
        return core_;
    }
    ~Mailbox() {
        close();
    }
    void create(const QString &, const QString &keyId, const Secret &);
    void open(const QString &, const Secret &);
    void close();
    bool isOpen() const {
        return core_->isOpen();
    }
    QString keyId() const;
    void bindCache(const QString &cacheId);
    // The identities the checkpoint was reached with; a different set restarts
    // the scan from the first object (keepCheckpoint: relabel only).
    void bindIdentities(const QString &fingerprint, bool keepCheckpoint = false);
    QString boundIdentities() const;
    // Groups many writes into one commit (one disk sync); without commit(),
    // all of them roll back.
    class Batch {
        std::unique_ptr<bmc::Mailbox::Batch> t_;

      public:
        explicit Batch(Mailbox &);
        ~Batch();
        void commit();
    };
    void store(const QString &hash, const QString &from, const QString &to, const QString &subject,
               const QString &body, qint64 checkpoint, const QString &folder = "Inbox");
    QString queueChat(const QString &local, const QString &peer, const QString &body,
                      qint64 expires);
    void storeRouted(const QString &id, const QString &sender, const QString &recipient,
                     const QString &subject, const QString &body, qint64 checkpoint,
                     const QString &folder, qint64 expires);
    void storeChat(const QString &id, const QString &sender, const QString &recipient, qint64 seqno,
                   const QString &body, qint64 expires, bool outgoing = false);
    // A thread, oldest first. With a limit, only its newest messages (a window of it), and
    // in *more whether older ones are left.
    QVector<ChatMessage> chatMessages(const QString &local, const QString &peer, int limit = 0,
                                      bool *more = nullptr) const;
    // One per conversation of every identity, the one with the newest message first.
    QVector<ChatSummary> chatSummaries() const;
    int chatUnread() const;
    // Marks the peer's messages to this identity as looked at; how many were not.
    int markChatRead(const QString &local, const QString &peer);
    // What a service has still to answer: the received chats nobody has looked at, oldest first
    // across every conversation, at most limit. One indexed read however long the chats are.
    QVector<ChatMessage> unreadChats(int limit) const;
    // Marks one received chat as looked at, by its id: false if it is not there or already read.
    bool markChatMessageRead(const QString &id);
    // The delivery state of a chat, by its id; empty if there is none.
    QString chatState(const QString &id) const;
    // The network work of the chats past their expiry ends. The chats stay in the mailbox: an
    // expiry belongs to the network object, and is no reason to delete what its owner keeps.
    void endChatDeliveries(qint64 time);
    void advance(qint64 checkpoint);
    qint64 checkpoint() const;
    QVector<Message> messages() const;
    QVector<Message> messageSummaries(int limit = 2000) const;
    QVector<Message> messageSummaries(const QString &folder, const QString &search, int offset,
                                      int limit, const QString &recipient = {},
                                      bool unreadOnly = false, bool anonymousOnly = false) const;
    int messageCount(const QString &folder, const QString &search, const QString &recipient = {},
                     bool unreadOnly = false, bool anonymousOnly = false) const;
    // The list's letters in display order (newest first), without reading bodies.
    QStringList messageHashes(const QString &folder, const QString &recipient = {},
                              bool unreadOnly = false, bool anonymousOnly = false) const;
    // Summaries of the given letters, in no particular order.
    QVector<Message> messageSummaries(const QStringList &hashes) const;
    // A letter's row in the unsearched list, or -1 when the list does not show it.
    int messagePosition(const QString &hash, const QString &folder, const QString &recipient = {},
                        bool unreadOnly = false, bool anonymousOnly = false) const;
    // Addresses with letters in a per-source folder (Channels: the chan;
    // Broadcasts: the subscribed sender), and whether one has unread letters.
    QStringList channelAddresses(const QString &folder = "Channels") const;
    bool channelUnread(const QString &recipient, const QString &folder = "Channels") const;
    // The addresses the letters of a folder came from, each once, in order of address.
    QStringList senders(const QString &folder) const;
    // The time of the newest post of each address that has posts (the folder Broadcasts).
    QMap<QString, qint64> lastPosts() const;
    // Changes affecting the desktop message snapshot, excluding scan checkpoints/jobs.
    quint64 messageRevision() const {
        return core_->messageRevision();
    }
    // Arrivals seen since the mailbox was opened, for the new-letter and new-chat notifications.
    // Session watches them for increases.
    quint64 incomingLetterCount() const {
        return core_->incomingLetterCount();
    }
    quint64 incomingChatCount() const {
        return core_->incomingChatCount();
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
    void recordMilestone(const QString &id, const QString &state, const QString &detail);
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
    // The address book: private names for correspondents, ordered by name.
    QVector<Contact> contacts() const;
    // Adds the address, or renames it if it is already a contact.
    void saveContact(const QString &address, const QString &label);
    void removeContact(const QString &address);
    void moveMessage(const QString &id, const QString &folder);
    void restoreMessage(const QString &id);
    void deleteMessage(const QString &id);
    void markRead(const QString &id);
    // The given letters read, in one write; how many of them were unread.
    int markRead(const QStringList &ids);
    bool unread(const QString &id) const;
    // The letters not yet read in each folder that receives mail (Inbox, Channels, Broadcasts);
    // a folder with none is not listed. And those together.
    QMap<QString, int> unreadByFolder() const;
    int unreadLetters() const;
    // Changes when another connection to the file has written to it, which messageRevision(), the
    // count of this connection's own changes, does not tell of.
    qint64 dataVersion() const;
    QString setting(const QString &name, const QString &fallback = {}) const;
    void setSetting(const QString &name, const QString &value);
    // Every setting whose name starts with the prefix (read literally), in order of name.
    QVector<QPair<QString, QString>> settings(const QString &prefix) const;
    // The letters filed under "Service" (Delivery::setServiceAddresses), oldest first, at most
    // limit: returned and deleted in one step.
    QVector<Message> takeService(int limit);
};
} // namespace bm

namespace bm {
struct OutboxItem {
    QString id, kind, state, error, objectHash;
    QByteArray ackToken, ackObject;
    qint64 expires = 0, nextAttempt = 0;
    int attempts = 0;
    Message message;
    bool chat = false;
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
struct Contact {
    QString address, label;
    qint64 added = 0;
};
struct DeliveryEvent {
    qint64 timestamp;
    QString state, detail;
};
} // namespace bm
