// The mailbox: one SQLCipher file holding letters, chats, outbox and delivery state, the network
// jobs, the public keys learned, the subscriptions, the contacts and the settings. The SQL is the
// Qt app's, character for character; the Qt wrapper in src/ converts to and from QString.
#pragma once
#include <bmc/records.h>
#include <bmc/secret.h>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

struct sqlite3;
namespace bmc {
class Transaction;

// A letter brought in from another client's mailbox: with the date it was received then, whether it
// was read, the folder it is in, and the network object it came from (if that is known).
struct ImportedLetter {
    std::string id;         // the letter's id here (messages.hash)
    std::string objectHash; // the network object it came from, lower-case hex; empty if unknown
    std::string sender, recipient, subject, body;
    std::string folder;         // Inbox, Channels, Broadcasts, Sent, Drafts or Trash
    std::string previousFolder; // where a letter in Trash goes back to
    int64_t received = 0;
    bool unread = false;
    bool broadcast = false; // a sent letter or a draft that goes to subscribers
};
// A chat message brought in from another client: from the peer to one of our identities, with the
// date it was received then and whether it was read.
struct ImportedChat {
    std::string id;          // its id here (chat_messages.id)
    std::string local, peer; // our identity, and the one it came from
    int64_t seqno = 0;       // the sender's number for it in this conversation
    std::string body;
    int64_t received = 0;
    bool unread = false;
};
struct ImportCounts {
    int added = 0;
    int present = 0; // the id or the network object was here already
};
// What a letter says, as one value: the same text between the same two addresses gives the same
// answer. For recognising a copy of a letter that carries no id of the network object.
std::string letterFingerprint(std::string_view sender, std::string_view recipient,
                              std::string_view subject, std::string_view body);

class Mailbox {
    sqlite3 *db_ = nullptr;
    uint64_t messageRevision_ = 0;
    // Incoming letters and chats that were new to the mailbox, counted from the last open. Only
    // arrivals are counted, and only once each: a catch-up or retention pass that meets a letter
    // again does not raise them.
    uint64_t incomingLetters_ = 0, incomingChats_ = 0;
    void connect(const std::string &path, const Secret &key, bool create);
    void sql(const char *q);
    void migrate();
    void deliveryUpdate(const std::string &id, const std::string &state, const std::string &detail,
                        int64_t nextAttempt);
    // The chat halves of outbox(), outgoing() and acknowledge(), which live in mailbox_chats.cpp.
    std::vector<OutboxItem> outboxChats() const;
    std::optional<OutboxItem> outgoingChat(const std::string &id) const;
    std::vector<std::string> chatAckIds(ByteView token) const;

  public:
    ~Mailbox();
    Mailbox() = default;
    Mailbox(const Mailbox &) = delete;
    Mailbox &operator=(const Mailbox &) = delete;
    Mailbox(Mailbox &&) = delete;
    Mailbox &operator=(Mailbox &&) = delete;
    void create(const std::string &path, const std::string &keyId, const Secret &key);
    void open(const std::string &path, const Secret &key);
    void close();
    bool isOpen() const {
        return db_ != nullptr;
    }
    std::string keyId() const;
    void bindCache(const std::string &cacheId);
    // The identities the checkpoint was reached with; a different set restarts the scan from the
    // first object (keepCheckpoint: relabel only).
    void bindIdentities(const std::string &fingerprint, bool keepCheckpoint = false);
    std::string boundIdentities() const;
    // Groups many writes into one commit (one disk sync); without commit(), all of them roll back.
    class Batch {
        std::unique_ptr<Transaction> t_;

      public:
        explicit Batch(Mailbox &m);
        ~Batch();
        void commit();
    };
    // objectHash: the network object the letter was read from. One object is stored once, whatever
    // id it comes with (a letter with an acknowledgment is keyed by what it says, not by its hash).
    void store(const std::string &hash, const std::string &from, const std::string &to,
               const std::string &subject, const std::string &body, int64_t checkpoint,
               const std::string &folder = "Inbox", const std::string &objectHash = {});
    std::string queueChat(const std::string &local, const std::string &peer,
                          const std::string &body, int64_t expires);
    void storeRouted(const std::string &id, const std::string &sender, const std::string &recipient,
                     const std::string &subject, const std::string &body, int64_t checkpoint,
                     const std::string &folder, int64_t expires,
                     const std::string &objectHash = {});
    // Letters brought in from another client: each with its own date, read state and folder. They
    // are no arrivals (no notification) and the scan checkpoint stays where it is. A letter whose
    // id or network object is here already is passed over. One transaction, or the one it is part
    // of. With `added`, it is given the position in `letters` of each letter that was stored.
    ImportCounts importLetters(const std::vector<ImportedLetter> &letters,
                               std::vector<size_t> *added = nullptr);
    // Chat messages brought in from another client, into their conversations, each with its own
    // date and read state. No arrival: nothing is notified, nothing is queued. A message whose id
    // is here, or whose number in its conversation is (a conversation holds one message per number,
    // whatever id the scan gave it), is passed over. One transaction, or the one it is part of.
    ImportCounts importChats(const std::vector<ImportedChat> &chats);
    // Contacts and subscriptions not here yet, by address; the ones here keep their names. How many
    // were added.
    int importContacts(const std::vector<Contact> &contacts);
    int importSubscriptions(const std::vector<Subscription> &subscriptions);
    // The fingerprints (letterFingerprint) of the letters in these folders.
    std::unordered_set<std::string>
    letterFingerprints(const std::vector<std::string> &folders) const;
    void storeChat(const std::string &id, const std::string &sender, const std::string &recipient,
                   int64_t seqno, const std::string &body, int64_t expires, bool outgoing = false);
    // A thread, oldest first. With a limit, only its newest messages (a window of it), and
    // in *more whether older ones are left.
    std::vector<ChatMessage> chatMessages(const std::string &local, const std::string &peer,
                                          int limit = 0, bool *more = nullptr) const;
    // One per conversation of every identity, the one with the newest message first.
    std::vector<ChatSummary> chatSummaries() const;
    int chatUnread() const;
    // Marks the peer's messages to this identity as looked at; how many were not.
    int markChatRead(const std::string &local, const std::string &peer);
    // What a service has still to answer: the received chats nobody has looked at, oldest first
    // across every conversation, at most limit. One indexed read however long the chats are.
    std::vector<ChatMessage> unreadChats(int limit) const;
    // Marks one received chat as looked at, by its id: false if it is not there or already read.
    bool markChatMessageRead(const std::string &id);
    // The delivery state of a chat, by its id; empty if there is none.
    std::string chatState(const std::string &id) const;
    // The network work of the chats past their expiry ends. The chats stay in the mailbox: an
    // expiry belongs to the network object, and is no reason to delete what its owner keeps.
    void endChatDeliveries(int64_t time);
    void advance(int64_t checkpoint);
    int64_t checkpoint() const;
    std::vector<Message> messages() const;
    std::vector<Message> messageSummaries(int limit = 2000) const;
    std::vector<Message> messageSummaries(const std::string &folder, const std::string &search,
                                          int offset, int limit, const std::string &recipient = {},
                                          bool unreadOnly = false,
                                          bool anonymousOnly = false) const;
    int messageCount(const std::string &folder, const std::string &search,
                     const std::string &recipient = {}, bool unreadOnly = false,
                     bool anonymousOnly = false) const;
    // The list's letters in display order (newest first), without reading bodies.
    std::vector<std::string> messageHashes(const std::string &folder,
                                           const std::string &recipient = {},
                                           bool unreadOnly = false,
                                           bool anonymousOnly = false) const;
    // Summaries of the given letters, in no particular order.
    std::vector<Message> messageSummaries(const std::vector<std::string> &hashes) const;
    // A letter's row in the unsearched list, or -1 when the list does not show it.
    int messagePosition(const std::string &hash, const std::string &folder,
                        const std::string &recipient = {}, bool unreadOnly = false,
                        bool anonymousOnly = false) const;
    // Addresses with letters in a per-source folder (Channels: the chan; Broadcasts: the
    // subscribed sender), and whether one has unread letters.
    std::vector<std::string> channelAddresses(const std::string &folder = "Channels") const;
    bool channelUnread(const std::string &recipient, const std::string &folder = "Channels") const;
    // The addresses the letters of a folder came from, each once, in order of address; a letter
    // with no sender adds none.
    std::vector<std::string> senders(const std::string &folder) const;
    // The messages the scan filed under "Service" (Delivery::setServiceAddresses), oldest first, at
    // most limit: returned and deleted together, in one transaction. Nothing else is touched.
    std::vector<Message> takeService(int limit);
    // Changes affecting the desktop message snapshot, excluding scan checkpoints/jobs.
    uint64_t messageRevision() const {
        return messageRevision_;
    }
    // Arrivals seen since the mailbox was opened, for the new-letter and new-chat notifications.
    uint64_t incomingLetterCount() const {
        return incomingLetters_;
    }
    uint64_t incomingChatCount() const {
        return incomingChats_;
    }
    void backup(const std::string &path, const Secret &key);
    std::string saveDraft(const std::string &id, const std::string &from, const std::string &to,
                          const std::string &subject, const std::string &body);
    Message message(const std::string &id) const;
    void queueDraft(const std::string &id, const std::string &kind, int64_t expires);
    std::vector<OutboxItem> outbox() const;
    OutboxItem outgoing(const std::string &id) const;
    void setDelivery(const std::string &id, const std::string &state,
                     const std::string &detail = {}, int64_t nextAttempt = 0);
    void setAck(const std::string &id, ByteView token, ByteView object = {});
    void setObjectHash(const std::string &id, const std::string &hash);
    bool acknowledge(ByteView token);
    void retry(const std::string &id, int64_t expires);
    void cancel(const std::string &id);
    std::vector<DeliveryEvent> events(const std::string &id) const;
    void recordMilestone(const std::string &id, const std::string &state,
                         const std::string &detail);
    std::string addJob(NetworkJob job);
    std::vector<NetworkJob> jobs() const;
    // A null payload (nullptr) leaves the stored one; an empty one clears it.
    void updateJob(const std::string &id, const std::string &state, const Bytes *payload = nullptr,
                   const std::string &hash = {}, const std::string &error = {});
    void removeJob(const std::string &id);
    void savePublicKey(const StoredPublicKey &key);
    std::optional<StoredPublicKey> publicKey(const std::string &address, int64_t now) const;
    std::vector<Subscription> subscriptions() const;
    void subscribe(const std::string &address, const std::string &label);
    void unsubscribe(const std::string &address);
    // The address book: private names for correspondents, ordered by name.
    std::vector<Contact> contacts() const;
    // Adds the address, or renames it if it is already a contact.
    void saveContact(const std::string &address, const std::string &label);
    void removeContact(const std::string &address);
    void moveMessage(const std::string &id, const std::string &folder);
    void restoreMessage(const std::string &id);
    void deleteMessage(const std::string &id);
    void markRead(const std::string &id);
    // The given letters read, in one transaction; how many of them were unread. A hash that is no
    // letter is passed over.
    int markRead(const std::vector<std::string> &ids);
    bool unread(const std::string &id) const;
    // The letters not yet read in each of the folders that receive mail (Inbox, Channels,
    // Broadcasts); a folder with none is not listed.
    std::map<std::string, int> unreadByFolder() const;
    // Those together: one of the two numbers on the app's icon, the other being chatUnread().
    int unreadLetters() const;
    // A number that changes when another connection to the file has written to it: the update
    // hook behind messageRevision() tells of this connection's own changes only.
    int64_t dataVersion() const;
    std::string setting(const std::string &name, const std::string &fallback = {}) const;
    void setSetting(const std::string &name, const std::string &value);
    // Every setting whose name starts with the prefix, in order of name: for a family such as
    // "nickname:<address>" read in one query instead of one per address. The prefix is literal.
    std::vector<std::pair<std::string, std::string>> settings(const std::string &prefix) const;
};
} // namespace bmc
