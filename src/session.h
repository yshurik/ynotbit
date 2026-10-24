#pragma once
#include "message_model.h"
#include "port_mapper.h"
#include "storage.h"
#include <QLockFile>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <bmc/client.h>
#include <bmc/node_config.h>
#include <bmc/node_status.h>
#include <bmc/node_supervisor.h>
#include <bmc/node_watch.h>
#include <utility>
namespace bm {
// What is unread, by the rail's pages: the letters of Inbox, Channels and Subscriptions (the
// Broadcasts folder), and the chat messages.
struct UnreadCounts {
    int inbox = 0, channels = 0, broadcasts = 0, chats = 0;
    int total() const {
        return inbox + channels + broadcasts + chats;
    }
};
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
    // The cores a bmc::Client shares with the two wrapper objects: one vault, one mailbox and
    // one node store, whether the app or a bot is driving them.
    std::shared_ptr<bmc::Vault> vaultCore_ = std::make_shared<bmc::Vault>();
    std::shared_ptr<bmc::Mailbox> mailboxCore_ = std::make_shared<bmc::Mailbox>();
    std::shared_ptr<bmc::Client> client_;
    Vault vault_{vaultCore_};
    Mailbox mailbox_{mailboxCore_};
    QString welcomeSource_;
    QString pyBitmessageOffer_; // see takePyBitmessageOffer()
    std::unique_ptr<MessageModel> messageModel_;
    QString root_, vaultPath_, mailPath_, mailKey_, pendingVaultPath_, error_, activity_;
    QStringList recentVaultPaths_, recentMailboxPaths_;
    std::optional<quint64> displayedRevision_;
    // The mailbox's revision and data version that unreadCounts_ is of.
    mutable std::optional<std::pair<quint64, qint64>> unreadAt_;
    mutable UnreadCounts unreadCounts_;
    // The node process, and what it last said: the core owns how to start and stop it, this class
    // owns when (the timers below) and what the window shows.
    std::unique_ptr<bmc::NodeSupervisor> node_;
    QTimer timer_;
    std::unique_ptr<QLockFile> nodeLock_, vaultLock_, mailLock_;
    bool busy_ = false;
    bool importing_ = false;
    bool offline_ = false;
    QString nodeError_;         // why the node last stopped, shown in status()
    quint16 listenPort_ = 8444; // the node's actual listen port, from status.json
    mutable std::optional<bmc::NodeStatus> lastNodeStatus_; // last status.json read, see status()
    QString lastStderrLine_; // the last line of the node's stderr shown, see pollNode()
    int retentionMB_ = 2048, retentionDays_ = 90;
    QTimer restartSoon_; // see scheduleRestart()
    int nodeRestarts_ = 0;
    bmc::NodeWatch nodeWatch_; // see setNodeWatchdog(): a node that runs but does no good
    QString watchdogNote_;     // why the watchdog last restarted the node
    int watchdogRestarts_ = 0;
    quint16 configuredPort() const;     // the listen port the node is asked for (8444 by default)
    bmc::NodeConfig nodeConfig() const; // what the node is started with, in the core's terms
    qint64 objectCount_ = 0, objectBytes_ = 0;
    int incoming_ = 0; // established_incoming from the node's status.json
    std::unique_ptr<PortMapper> portMapper_;
    void updatePortMapping();
    // Arrival notifications: the counts the mailbox was last watched at, and
    // whether the first look (which is not an arrival) has happened.
    quint64 seenLetters_ = 0, seenChats_ = 0;
    bool arrivalsPrimed_ = false;
    void bindMailboxToCache();
    void startNode();
    void pollNode();
    void attempt(const std::function<void()> &f);
    void refresh();
    void clearMessages();
    void tick();
    void checkArrivals();
    void acquireVault(const QString &);
    std::unique_ptr<QLockFile> lockVaultFile(const QString &); // or throws why there is none
    void openMailboxPath(const QString &);
    void askForVault(const QString &path);
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
    MessageModel *messageModel() const {
        return messageModel_.get();
    }
    QVariantList identities() const;
    qint64 objectCount() const {
        return objectCount_;
    }
    qint64 cacheBytes() const {
        return objectBytes_;
    }
    bool networkEnabled() const {
        return !offline_;
    }
    Q_INVOKABLE void setNetworkEnabled(bool enabled);
    Q_INVOKABLE void restartNode();
    QStringList nodeArguments() const;
    // An empty string or IP:port, as the node accepts for -P and -r.
    static bool validEndpoint(const QString &text);
    QString peer() const;
    QString proxy() const;
    // Save a valid value (trimmed; empty clears it) and restart the node soon.
    // false, and nothing saved, when the value is not an IP:port.
    bool setPeer(QString value);
    bool setProxy(QString value);
    int retentionMB() const {
        return retentionMB_;
    }
    int retentionDays() const {
        return retentionDays_;
    }
    void setRetention(int mb, int days);
    // One node restart 1 s after the last call, so a burst of changes restarts once.
    void scheduleRestart();
    int nodeRestarts() const {
        return nodeRestarts_;
    }
    // A node whose status stopped coming is restarted, always. With lonelyMinutes above zero a node
    // that has had no peer that long is too (a service that must keep its network asks for it; a
    // desktop that is simply offline does not). Never twice within five minutes.
    void setNodeWatchdog(int lonelyMinutes);
    // Why the watchdog last restarted the node ("" if it has not), and how many times.
    QString watchdogNote() const {
        return watchdogNote_;
    }
    int watchdogRestarts() const {
        return watchdogRestarts_;
    }
    bool gpuEnabled() const;
    Q_INVOKABLE void setGpuEnabled(bool on);
    // Incoming connections (default on); never while a proxy is set.
    bool listenEnabled() const;
    Q_INVOKABLE void setListenEnabled(bool on);
    // Open the node's port on the router with UPnP (default on).
    bool upnpEnabled() const;
    Q_INVOKABLE void setUpnpEnabled(bool on);
    int incomingConnections() const {
        return incoming_;
    }
    PortMapper &portMapper() {
        return *portMapper_;
    }
    static bool wantsPortMapping(bool online, bool listen, bool upnp, const QString &proxy) {
        return bmc::wantsPortMapping(online, listen, upnp, proxy.toStdString());
    }
    // A new vault at path holding identity, its first and default, in place of
    // the open vault, which stays open when the new file cannot be reserved;
    // true when made, else error() says why.
    bool createVault(const QString &path, QString password, QString repeated,
                     const Identity &identity);
    Q_INVOKABLE void openVault();
    Q_INVOKABLE void unlockVault();
    Q_INVOKABLE void beginVaultOpen();
    // Asks for the password of the vault at path, as Open vault… does once a file is chosen. An
    // open vault is closed first, as a new vault or a mailbox takes the open one's place; asking
    // for the open vault itself changes nothing.
    Q_INVOKABLE void beginVaultOpenAt(QString path);
    Q_INVOKABLE void beginVaultUnlock();
    Q_INVOKABLE void submitVaultPassword(QString password);
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
    // Adds identity to the open vault; true when added, else error() says why.
    bool addIdentity(const Identity &identity);
    // A chan from its shared phrase, listed as name ("#<name>"); expected, when given,
    // must be the address the phrase makes.
    bool joinChannel(QString phrase, const QString &name, const QString &expected);
    Q_INVOKABLE void importIdentities();
    // The vault and the mailbox as the core works on them, for the import of another client's data.
    bmc::Vault &coreVault() {
        return *vaultCore_;
    }
    bmc::Mailbox &coreMailbox() {
        return *mailboxCore_;
    }
    // While an import runs the scan of the node's objects waits: the import stores letters with the
    // date and read state they had, and then starts the scan over for the identities that came in,
    // which must not get to the same letters first. Ending it shows what the import stored.
    void setImporting(bool on);
    bool importing() const {
        return importing_;
    }
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
    // The given letters read at once, in one write (the list's "Mark all as read"); how many of
    // them were unread.
    Q_INVOKABLE int markLettersRead(QStringList ids);
    Q_INVOKABLE QVariantMap message(QString id) const;
    QHash<QString, QString> names() const;
    static QString errorText(const std::exception &e);
    // Queues a chat; *id, if asked for, is the new message's id, to follow it with chatState().
    bool sendChat(QString from, QString peer, QString body, QString *id = nullptr);
    // Where the delivery of a chat of ours has got to ("queued", "awaiting_ack",
    // "acknowledged"...); empty if there is no such chat.
    QString chatState(QString id) const;
    // A conversation, oldest first; with a limit, only its newest messages (a window of it),
    // and in *more whether older ones are kept.
    QVariantList chatMessages(QString local, QString peer, int limit = 0,
                              bool *more = nullptr) const;
    // The chats of this vault's identities, the one with the newest message first. A chat
    // is between one identity and a peer: one peer under two identities is two chats.
    QVariantList chatConversations() const;
    // A chat's key in lists: its identity and its peer.
    static QString chatKey(const QString &local, const QString &peer) {
        return local + '|' + peer;
    }
    static QString chatLocal(const QString &key) {
        return key.section('|', 0, 0);
    }
    static QString chatPeer(const QString &key) {
        return key.section('|', 1);
    }
    int chatUnread() const {
        return mailboxOpen() ? mailbox_.chatUnread() : 0;
    }
    // What is unread on each page of the rail, all zero with no mailbox open. Worked out again
    // only when the mailbox has changed (by this program or by another connection to the file), so
    // asking after every change of the window costs nothing.
    UnreadCounts unreadCounts() const;
    // The unread letters (of the folders that receive mail) and chat messages together: the
    // number on the app's icon.
    int unreadTotal() const;
    // What has come in this conversation is looked at. How many messages were not.
    int markChatRead(QString local, QString peer);
    // The chats still to be answered, oldest first across every conversation, at most limit:
    // id, local, peer, body, seqno, received. What a service works through.
    QVariantList unreadChats(int limit) const;
    // One received chat looked at, by its id: false if it is not there or was read already.
    bool markChatMessageRead(QString id);
    QVariantList messagePage(const QString &folder, const QString &search, int offset, int limit,
                             const QString &recipient = {}, bool unreadOnly = false,
                             bool anonymousOnly = false) const;
    int messageCount(const QString &folder, const QString &search, const QString &recipient = {},
                     bool unreadOnly = false, bool anonymousOnly = false) const {
        return mailboxOpen()
                   ? mailbox_.messageCount(folder, search, recipient, unreadOnly, anonymousOnly)
                   : 0;
    }
    QStringList messageHashes(const QString &folder, const QString &recipient = {},
                              bool unreadOnly = false, bool anonymousOnly = false) const {
        return mailboxOpen() ? mailbox_.messageHashes(folder, recipient, unreadOnly, anonymousOnly)
                             : QStringList();
    }
    int messagePosition(const QString &hash, const QString &folder, const QString &recipient = {},
                        bool unreadOnly = false, bool anonymousOnly = false) const {
        return mailboxOpen()
                   ? mailbox_.messagePosition(hash, folder, recipient, unreadOnly, anonymousOnly)
                   : -1;
    }
    // List rows for the given letters, keyed by hash.
    QHash<QString, QVariantMap> messageRows(const QStringList &hashes) const;
    // The open mailbox's key, for a second (search) connection; null when closed.
    const Secret *mailboxSecret() const {
        return mailboxOpen() ? &vault_.mailboxKey(mailKey_) : nullptr;
    }
    QVariantList channels() const;
    bool channelUnread(QString address) const {
        return mailboxOpen() && mailbox_.channelUnread(address);
    }
    // The Broadcasts page's sources: subscriptions, ynotbit's release
    // announcements while notices are on, and any other sender whose broadcasts
    // are still kept. Each: address, label, subscribed, updates (the built-in one).
    QVariantList broadcastSources() const;
    // The built-in subscriptions (updates.h), once per mailbox.
    void seedSubscriptions();
    // Sets pyBitmessageOffer_ for the mailbox that has just opened or been made.
    void considerPyBitmessageOffer();
    // The built-in contacts (updates.h), once per mailbox each: deleting one sticks. One that is
    // already there keeps its name; the address gets its nickname either way.
    void seedContacts();
    // Everyone a letter in Sent went to joins the address book, named from the address, once
    // per mailbox: for those made before the book was filled this way. A mailbox made now starts
    // marked, so whom it writes to later stays out until the user adds them.
    void addSentRecipients();
    // The same for everyone who wrote a letter in the Inbox: the user knows them. Once per
    // mailbox, and a mailbox made now starts marked too, so whoever writes to it later stays out
    // until the user adds them.
    void addInboxSenders();
    // The same, now, for people met in letters that have just come in (an import): each named from
    // its address, unless it is in the book already, is one of the vault's own or is no address.
    // How many were added.
    int addToAddressBook(const QStringList &addresses);
    // Where the window opens after a mailbox opens, once per opening: the
    // Subscriptions page on the Bitmessage digest.
    QString takeWelcomeSource() {
        return std::exchange(welcomeSource_, {});
    }
    // Where PyBitmessage's data is on this computer: its folders that hold some.
    QStringList pyBitmessageFolders() const;
    // Tests name the folders themselves, so that a PyBitmessage on the machine they run on is not
    // found. An empty function brings the real search back.
    static void setPyBitmessageFoldersForTesting(std::function<QStringList()> search);
    // The mailbox's flag for the import of PyBitmessage's data. A mailbox starts with it not set;
    // it is set when an import finishes, or when the user asks not to be offered the import
    // again. Until then the import is offered each time the mailbox opens or is made, if
    // PyBitmessage's data is on this computer.
    bool pyBitmessageImportSettled() const;
    void settlePyBitmessageImport();
    // Once per opening, like takeWelcomeSource(): the folder of PyBitmessage's data that the window
    // should offer to import now, or empty when it has nothing to offer.
    QString takePyBitmessageOffer() {
        return std::exchange(pyBitmessageOffer_, {});
    }
    bool broadcastUnread(QString address) const {
        return mailboxOpen() && mailbox_.channelUnread(address, "Broadcasts");
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
    // The address book (kept in the open mailbox). Names are local and private. Each contact:
    // address, label (its name) and nickname (what it goes by, /bot, @person or #chan; often
    // empty).
    QVariantList contacts() const;
    // An address's nickname, empty when it has none. Setting an empty one takes it away.
    QString nickname(QString address) const;
    void setNickname(QString address, QString nickname);
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
    // A letter or a chat this mailbox had not seen before, once per arrival.
    void incomingLetter();
    void incomingChat();
    void messageRead(QString id);
    void locked();
    void aboutToCloseMailbox();
    void vaultPasswordRequired(QString path);
    void vaultPasswordAccepted();
};
} // namespace bm
