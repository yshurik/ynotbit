// The mailbox: the letters, the outbox and its delivery records, the network jobs, the public
// keys, the subscriptions, the contacts, the settings and the chats. The SQL is the Qt app's
// (src/storage.cpp and src/mailbox_ops.cpp of the previous release), character for character.
#include <algorithm>
#include <bmc/chat_format.h>
#include <bmc/clock.h>
#include <bmc/hash.h>
#include <bmc/mailbox.h>
#include <bmc/path.h>
#include <bmc/random.h>
#include <bmc/sqlite.h>
#include <bmc/text.h>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sqlcipher/sqlite3.h>

namespace bmc {
namespace {
bool column(sqlite3 *db, const char *table, const std::string &name) {
    const auto query = std::string("PRAGMA table_info(") + table + ")";
    Statement s(db, query.c_str());
    while (s.row())
        if (s.text(1) == name)
            return true;
    return false;
}
// Mailbox::deliveryUpdate's set of states (src/mailbox_ops.cpp:196-199).
bool knownDeliveryState(const std::string &state) {
    static const char *const states[] = {
        "queued",       "awaiting_pubkey", "calculating_ack", "calculating_message",
        "ready",        "publishing",      "awaiting_ack",    "published",
        "acknowledged", "cancelled",       "failed",          "expired"};
    for (const char *s : states)
        if (state == s)
            return true;
    return false;
}
bool isTerminal(const std::string &state) {
    return state == "acknowledged" || state == "published" || state == "cancelled" ||
           state == "failed" || state == "expired";
}
// The shared filter of the letter list (src/storage.cpp:252-265).
std::string messageFilter(const std::string &search, const std::string &recipient, bool unreadOnly,
                          bool anonymousOnly) {
    std::string filter(" WHERE folder=?");
    if (!recipient.empty())
        filter += " AND recipient=?";
    if (unreadOnly)
        filter += " AND unread<>0";
    if (anonymousOnly)
        filter += " AND sender=recipient";
    if (!search.empty())
        filter += " AND (instr(lower(subject),lower(?)) OR instr(lower(sender),lower(?)) OR "
                  "instr(lower(recipient),lower(?)) OR instr(lower(body),lower(?)))";
    return filter;
}
} // namespace

void Mailbox::sql(const char *q) {
    if (!db_)
        throw Error(Code::State, "Mailbox is closed");
    char *err = nullptr;
    const int r = sqlite3_exec(db_, q, nullptr, nullptr, &err);
    const std::string message = err ? err : "";
    sqlite3_free(err);
    if (r != SQLITE_OK)
        throw Error(Code::Io, message);
}

void Mailbox::connect(const std::string &path, const Secret &key, bool create) {
    if (db_)
        throw Error(Code::State, "Close the current mailbox first");
    if (key.size() != 32)
        throw Error(Code::Argument, "Invalid mailbox key");
    incomingLetters_ = incomingChats_ = 0;
    const auto file = bmc::path(path);
    if (create)
        reserveFile(file);
    // The path the caller gave, not the native one: SQLite takes UTF-8 and makes its own wide path
    // on Windows, so there is nothing to convert here.
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        close();
        if (create)
            std::filesystem::remove(file);
        throw Error(Code::Io, "Cannot open mailbox");
    }
    try {
        sqlite3_update_hook(
            db_,
            [](void *context, int, const char *, const char *table, sqlite3_int64) {
                if (std::strcmp(table, "messages") == 0 || std::strcmp(table, "outbox") == 0 ||
                    std::strcmp(table, "chat_messages") == 0 ||
                    std::strcmp(table, "chat_outbox") == 0 || std::strcmp(table, "settings") == 0 ||
                    std::strcmp(table, "delivery_events") == 0)
                    ++static_cast<Mailbox *>(context)->messageRevision_;
            },
            this);
        sqlite3_busy_timeout(db_, 3000);
#ifdef SQLITE_HAS_CODEC
        if (sqlite3_key(db_, key.data(), 32) != SQLITE_OK)
            throw Error(Code::Auth, "Cannot apply mailbox key");
#else
        throw Error(Code::Internal, "This build lacks SQLCipher");
#endif
        Statement version(db_, "PRAGMA cipher_version");
        if (!(version.row() && !version.text(0).empty()))
            throw Error(Code::Internal, "SQLCipher is required");
        sql("PRAGMA cipher_memory_security=ON; PRAGMA temp_store=MEMORY; PRAGMA foreign_keys=ON;");
        Statement verify(db_, "SELECT count(*) FROM sqlite_master");
        verify.row();
    } catch (...) {
        close();
        if (create)
            std::filesystem::remove(file);
        throw;
    }
}

void Mailbox::create(const std::string &path, const std::string &id, const Secret &key) {
    connect(path, key, true);
    try {
        sql("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; BEGIN IMMEDIATE; CREATE TABLE "
            "meta(id TEXT NOT NULL, checkpoint INTEGER NOT NULL, cache_id TEXT NOT NULL DEFAULT "
            "'', identities TEXT NOT NULL DEFAULT ''); CREATE TABLE messages(hash TEXT PRIMARY "
            "KEY, sender TEXT NOT NULL, recipient TEXT NOT NULL, subject TEXT NOT NULL, body TEXT "
            "NOT NULL, folder TEXT NOT NULL, received INTEGER NOT NULL); PRAGMA user_version=1;");
        Statement s(db_, "INSERT INTO meta(id,checkpoint) VALUES(?,0)");
        s.text(1, id);
        s.row();
        sql("COMMIT");
        migrate();
    } catch (...) {
        close();
        std::filesystem::remove(bmc::path(path));
        throw;
    }
}

void Mailbox::open(const std::string &path, const Secret &key) {
    connect(path, key, false);
    try {
        Statement s(db_, "PRAGMA user_version");
        if (!(s.row() && (s.number(0) == 1 || s.number(0) == 2)))
            throw Error(Code::Invalid, "Unsupported mailbox format");
        keyId();
        migrate();
    } catch (...) {
        close();
        throw;
    }
}

Mailbox::~Mailbox() {
    close();
}

void Mailbox::close() {
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
        ++messageRevision_;
    }
}

std::string Mailbox::keyId() const {
    Statement s(db_, "SELECT id FROM meta");
    if (!s.row())
        throw Error(Code::Invalid, "Invalid mailbox");
    auto id = s.text(0);
    if (s.row())
        throw Error(Code::Invalid, "Invalid mailbox metadata");
    return id;
}

int64_t Mailbox::checkpoint() const {
    Statement s(db_, "SELECT checkpoint FROM meta");
    if (!s.row())
        throw Error(Code::Invalid, "Invalid checkpoint");
    return s.number(0);
}

void Mailbox::advance(int64_t c) {
    Statement s(db_, "UPDATE meta SET checkpoint=?");
    s.number(1, c);
    s.row();
}

void Mailbox::store(const std::string &hash, const std::string &from, const std::string &to,
                    const std::string &subject, const std::string &body, int64_t c,
                    const std::string &folder, const std::string &objectHash) {
    Transaction t(db_);
    bool inserted = false;
    {
        Statement s(db_, "INSERT OR IGNORE INTO messages(hash,sender,recipient,subject,body,folder,"
                         "received,object_hash) VALUES(?,?,?,?,?,?,?,?)");
        s.text(1, hash);
        s.text(2, from);
        s.text(3, to);
        s.text(4, subject);
        s.text(5, body);
        s.text(6, folder);
        s.number(7, now());
        s.text(8, objectHash);
        s.row();
        // A row that was actually inserted is a letter this mailbox had not seen: an INSERT OR
        // IGNORE that matched an existing hash is not.
        inserted = sqlite3_changes(db_) > 0;
        if (c >= 0) // a catch-up pass stores behind the checkpoint and leaves it be
            advance(c);
    }
    t.commit();
    if (inserted && folder == "Inbox")
        ++incomingLetters_;
}

std::vector<Message> Mailbox::messages() const {
    Statement s(db_, "SELECT hash,sender,recipient,subject,body,folder,received FROM messages "
                     "ORDER BY received DESC,rowid DESC");
    std::vector<Message> list;
    while (s.row())
        list.push_back(
            {s.text(0), s.text(1), s.text(2), s.text(3), s.text(4), s.text(5), s.number(6)});
    return list;
}

std::vector<Message> Mailbox::messageSummaries(int limit) const {
    Statement s(db_, "SELECT hash,sender,recipient,subject,substr(body,1,240),folder,received "
                     "FROM messages ORDER BY received DESC,rowid DESC LIMIT ?");
    s.number(1, std::clamp(limit, 1, 10000));
    std::vector<Message> list;
    while (s.row())
        list.push_back(
            {s.text(0), s.text(1), s.text(2), s.text(3), s.text(4), s.text(5), s.number(6)});
    return list;
}

std::vector<Message> Mailbox::messageSummaries(const std::string &folder, const std::string &search,
                                               int offset, int limit, const std::string &recipient,
                                               bool unreadOnly, bool anonymousOnly) const {
    const auto query =
        std::string(
            "SELECT hash,sender,recipient,substr(subject,1,240),substr(body,1,240),folder,received "
            "FROM messages") +
        messageFilter(search, recipient, unreadOnly, anonymousOnly) +
        " ORDER BY received DESC,rowid DESC LIMIT ? OFFSET ?";
    Statement s(db_, query.c_str());
    s.text(1, folder);
    int parameter = 2;
    if (!recipient.empty())
        s.text(parameter++, recipient);
    if (!search.empty())
        for (int i = 0; i < 4; ++i)
            s.text(parameter++, search);
    s.number(parameter++, std::clamp(limit, 1, 500));
    s.number(parameter, std::max(0, offset));
    std::vector<Message> list;
    while (s.row())
        list.push_back(
            {s.text(0), s.text(1), s.text(2), s.text(3), s.text(4), s.text(5), s.number(6)});
    return list;
}

int Mailbox::messageCount(const std::string &folder, const std::string &search,
                          const std::string &recipient, bool unreadOnly, bool anonymousOnly) const {
    const auto query = std::string("SELECT count(*) FROM messages") +
                       messageFilter(search, recipient, unreadOnly, anonymousOnly);
    Statement s(db_, query.c_str());
    s.text(1, folder);
    int parameter = 2;
    if (!recipient.empty())
        s.text(parameter++, recipient);
    if (!search.empty())
        for (int i = 0; i < 4; ++i)
            s.text(parameter++, search);
    s.row();
    return int(s.number(0));
}

std::vector<std::string> Mailbox::messageHashes(const std::string &folder,
                                                const std::string &recipient, bool unreadOnly,
                                                bool anonymousOnly) const {
    const auto query = std::string("SELECT hash FROM messages") +
                       messageFilter({}, recipient, unreadOnly, anonymousOnly) +
                       " ORDER BY received DESC,rowid DESC";
    Statement s(db_, query.c_str());
    s.text(1, folder);
    if (!recipient.empty())
        s.text(2, recipient);
    std::vector<std::string> hashes;
    while (s.row())
        hashes.push_back(s.text(0));
    return hashes;
}

std::vector<Message> Mailbox::messageSummaries(const std::vector<std::string> &hashes) const {
    Statement s(
        db_,
        "SELECT hash,sender,recipient,substr(subject,1,240),substr(body,1,240),folder,received "
        "FROM messages WHERE hash=?");
    std::vector<Message> list;
    for (const auto &hash : hashes) {
        s.reset();
        s.text(1, hash);
        if (s.row())
            list.push_back(
                {s.text(0), s.text(1), s.text(2), s.text(3), s.text(4), s.text(5), s.number(6)});
    }
    return list;
}

int Mailbox::messagePosition(const std::string &hash, const std::string &folder,
                             const std::string &recipient, bool unreadOnly,
                             bool anonymousOnly) const {
    const auto filter = messageFilter({}, recipient, unreadOnly, anonymousOnly);
    const auto bind = [&](Statement &s) {
        s.text(1, folder);
        if (!recipient.empty())
            s.text(2, recipient);
        return recipient.empty() ? 2 : 3;
    };
    Statement at(
        db_, (std::string("SELECT received,rowid FROM messages") + filter + " AND hash=?").c_str());
    at.text(bind(at), hash);
    if (!at.row())
        return -1;
    const int64_t received = at.number(0), rowid = at.number(1);
    // Rows above it: newer, or as new and inserted later (the list's order).
    Statement before(db_, (std::string("SELECT count(*) FROM messages") + filter +
                           " AND (received>? OR (received=? AND rowid>?))")
                              .c_str());
    int parameter = bind(before);
    before.number(parameter++, received);
    before.number(parameter++, received);
    before.number(parameter, rowid);
    before.row();
    return int(before.number(0));
}

std::vector<std::string> Mailbox::channelAddresses(const std::string &folder) const {
    Statement s(db_, "SELECT DISTINCT recipient FROM messages WHERE folder=? AND recipient<>'' "
                     "ORDER BY recipient");
    s.text(1, folder);
    std::vector<std::string> result;
    while (s.row())
        result.push_back(s.text(0));
    return result;
}

bool Mailbox::channelUnread(const std::string &recipient, const std::string &folder) const {
    Statement s(db_, "SELECT 1 FROM messages WHERE folder=? AND recipient=? AND unread<>0 LIMIT 1");
    s.text(1, folder);
    s.text(2, recipient);
    return s.row();
}

void Mailbox::backup(const std::string &path, const Secret &key) {
    if (!db_)
        throw Error(Code::State, "Mailbox is closed");
    Mailbox dest;
    dest.connect(path, key, true);
    auto b = sqlite3_backup_init(dest.db_, "main", db_, "main");
    if (!b) {
        dest.close();
        std::filesystem::remove(bmc::path(path));
        throw Error(Code::Io, "Cannot start mailbox backup");
    }
    const int r = sqlite3_backup_step(b, -1);
    const int f = sqlite3_backup_finish(b);
    if (r != SQLITE_DONE || f != SQLITE_OK) {
        dest.close();
        std::filesystem::remove(bmc::path(path));
        throw Error(Code::Io, "Mailbox backup failed");
    }
    dest.close();
}

void Mailbox::bindCache(const std::string &id) {
    Statement s(
        db_,
        "UPDATE meta SET checkpoint=CASE WHEN cache_id=? THEN checkpoint ELSE 0 END, cache_id=?");
    s.text(1, id);
    s.text(2, id);
    s.row();
}

Mailbox::Batch::Batch(Mailbox &m) : t_(std::make_unique<Transaction>(m.db_)) {}
Mailbox::Batch::~Batch() = default;
void Mailbox::Batch::commit() {
    t_->commit();
}

std::string Mailbox::boundIdentities() const {
    Statement s(db_, "SELECT identities FROM meta");
    return s.row() ? s.text(0) : std::string();
}

void Mailbox::bindIdentities(const std::string &fingerprint, bool keepCheckpoint) {
    // Called before every scan batch: write (a synchronous commit) only when the
    // identities actually changed.
    if (boundIdentities() == fingerprint)
        return;
    if (keepCheckpoint) {
        Statement s(db_, "UPDATE meta SET identities=?");
        s.text(1, fingerprint);
        s.row();
        return;
    }
    Statement s(db_, "UPDATE meta SET checkpoint=CASE WHEN identities=? THEN checkpoint ELSE 0 "
                     "END, identities=?");
    s.text(1, fingerprint);
    s.text(2, fingerprint);
    s.row();
}

void Mailbox::migrate() {
    Transaction t(db_);
    sql("CREATE INDEX IF NOT EXISTS messages_folder_received ON messages(folder,received)");
    sql("CREATE INDEX IF NOT EXISTS messages_channel_received ON "
        "messages(folder,recipient,received)");
    if (!column(db_, "meta", "cache_id"))
        sql("ALTER TABLE meta ADD COLUMN cache_id TEXT NOT NULL DEFAULT ''");
    if (!column(db_, "meta", "identities"))
        sql("ALTER TABLE meta ADD COLUMN identities TEXT NOT NULL DEFAULT ''");
    if (!column(db_, "messages", "unread")) {
        sql("ALTER TABLE messages ADD COLUMN unread INTEGER NOT NULL DEFAULT 1");
        sql("UPDATE messages SET unread=0 WHERE folder NOT IN ('Inbox','Channels','Broadcasts')");
    }
    // Only the unread rows, so that counting them (unreadLetters) reads no letter.
    sql("CREATE INDEX IF NOT EXISTS messages_unread ON messages(folder) WHERE unread<>0");
    // The network object a letter was read from. One object makes one letter, whatever id it was
    // stored under (a letter with an acknowledgment is keyed by what it says, not by its hash), so
    // that a rescan and an import meeting the same object do not store it twice. Only the rows that
    // name an object are held to it: the older ones, and the letters written here, name none.
    if (!column(db_, "messages", "object_hash"))
        sql("ALTER TABLE messages ADD COLUMN object_hash TEXT NOT NULL DEFAULT ''");
    sql("CREATE UNIQUE INDEX IF NOT EXISTS messages_object_hash ON messages(object_hash) "
        "WHERE object_hash<>''");
    if (!column(db_, "messages", "previous_folder"))
        sql("ALTER TABLE messages ADD COLUMN previous_folder TEXT NOT NULL DEFAULT 'Inbox'");
    sql("CREATE TABLE IF NOT EXISTS outbox(id TEXT PRIMARY KEY REFERENCES messages(hash) ON DELETE "
        "CASCADE,kind TEXT NOT NULL,state TEXT NOT NULL,error TEXT NOT NULL DEFAULT '',object_hash "
        "TEXT NOT NULL DEFAULT '',ack_token BLOB NOT NULL DEFAULT X'',ack_object BLOB NOT NULL "
        "DEFAULT X'',expires INTEGER NOT NULL,next_attempt INTEGER NOT NULL DEFAULT 0,attempts "
        "INTEGER NOT NULL DEFAULT 1);"
        "CREATE TABLE IF NOT EXISTS delivery_events(seq INTEGER PRIMARY KEY AUTOINCREMENT,id TEXT "
        "NOT NULL REFERENCES messages(hash) ON DELETE CASCADE,ts INTEGER NOT NULL,state TEXT NOT "
        "NULL,detail TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS network_jobs(id TEXT PRIMARY KEY,owner TEXT NOT NULL,kind TEXT "
        "NOT NULL,address TEXT NOT NULL,state TEXT NOT NULL,hash TEXT NOT NULL,error TEXT NOT "
        "NULL,payload BLOB NOT NULL,expires INTEGER NOT NULL,trials INTEGER NOT NULL,extra INTEGER "
        "NOT NULL);"
        "CREATE TABLE IF NOT EXISTS public_keys(address TEXT PRIMARY KEY,signing BLOB NOT "
        "NULL,encryption BLOB NOT NULL,trials INTEGER NOT NULL,extra INTEGER NOT NULL,behaviors "
        "INTEGER NOT NULL,expires INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS subscriptions(address TEXT PRIMARY KEY,label TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS contacts(address TEXT PRIMARY KEY,label TEXT NOT NULL,added "
        "INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS settings(name TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS delivery_events_id ON delivery_events(id,seq); PRAGMA "
        "user_version=2;");
    sql("CREATE TABLE IF NOT EXISTS chat_messages(id TEXT PRIMARY KEY,local_address TEXT NOT NULL,"
        "peer_address TEXT NOT NULL,direction TEXT NOT NULL CHECK(direction IN ('in','out')),"
        "seqno INTEGER NOT NULL,body TEXT NOT NULL,received INTEGER NOT NULL,expires INTEGER NOT "
        "NULL,"
        "state TEXT NOT NULL,unread INTEGER NOT NULL DEFAULT 0,"
        "UNIQUE(local_address,peer_address,direction,seqno));"
        "CREATE INDEX IF NOT EXISTS chat_conversation ON "
        "chat_messages(local_address,peer_address,received);"
        "CREATE INDEX IF NOT EXISTS chat_expiry ON chat_messages(expires);"
        "CREATE TABLE IF NOT EXISTS chat_sequences(local_address TEXT NOT NULL,peer_address TEXT "
        "NOT NULL,"
        "last_seq INTEGER NOT NULL,PRIMARY KEY(local_address,peer_address));"
        "CREATE TABLE IF NOT EXISTS chat_outbox(id TEXT PRIMARY KEY REFERENCES chat_messages(id) "
        "ON DELETE CASCADE,"
        "state TEXT NOT NULL,error TEXT NOT NULL DEFAULT '',object_hash TEXT NOT NULL DEFAULT '',"
        "ack_token BLOB NOT NULL DEFAULT X'',ack_object BLOB NOT NULL DEFAULT X'',"
        "expires INTEGER NOT NULL,next_attempt INTEGER NOT NULL DEFAULT 0,attempts INTEGER NOT "
        "NULL DEFAULT 1);");
    // Chats made before unread counts: all of them read.
    if (!column(db_, "chat_messages", "unread"))
        sql("ALTER TABLE chat_messages ADD COLUMN unread INTEGER NOT NULL DEFAULT 0");
    // The chats still to be looked at, which a service reads again and again: its own small index.
    sql("CREATE INDEX IF NOT EXISTS chat_unread ON chat_messages(received) "
        "WHERE direction='in' AND unread=1");
    t.commit();
}

std::string Mailbox::saveDraft(const std::string &id, const std::string &from,
                               const std::string &to, const std::string &subject,
                               const std::string &body) {
    require(subject.size() <= 8192 && body.size() <= size_t(kMaxDraftBytes), "Letter is too large");
    auto draft = id.empty() ? "draft-" + uuid() : id;
    Transaction t(db_);
    if (!id.empty()) {
        auto m = message(id);
        require(m.folder == "Drafts", "Only drafts can be edited");
        Statement pending(db_, "SELECT state FROM outbox WHERE id=?");
        pending.text(1, id);
        require(!pending.row(),
                "This letter already has a delivery record; duplicate it as a new draft");
    }
    Statement s(db_,
                "INSERT INTO messages(hash,sender,recipient,subject,body,folder,received,unread) "
                "VALUES(?,?,?,?,?,'Drafts',?,0) ON CONFLICT(hash) DO UPDATE SET "
                "sender=excluded.sender,recipient=excluded.recipient,subject=excluded.subject,body="
                "excluded.body,received=excluded.received");
    s.text(1, draft);
    s.text(2, from);
    s.text(3, to);
    s.text(4, subject);
    s.text(5, body);
    s.number(6, now());
    s.row();
    t.commit();
    return draft;
}

Message Mailbox::message(const std::string &id) const {
    Statement s(
        db_,
        "SELECT hash,sender,recipient,subject,body,folder,received FROM messages WHERE hash=?");
    s.text(1, id);
    require(s.row(), "Letter no longer exists");
    return {s.text(0), s.text(1), s.text(2), s.text(3), s.text(4), s.text(5), s.number(6)};
}

void Mailbox::queueDraft(const std::string &id, const std::string &kind, int64_t expires) {
    require(kind == "direct" || kind == "broadcast", "Invalid delivery type");
    auto m = message(id);
    require(m.folder == "Drafts", "Only a draft can be sent");
    require(!m.from.empty() && (kind == "broadcast" || !m.to.empty()),
            "Choose a sender and recipient");
    require(expires > now(), "Message expiry is in the past");
    Transaction t(db_);
    Statement s(db_, "INSERT INTO outbox(id,kind,state,expires) VALUES(?,?,'queued',?)");
    s.text(1, id);
    s.text(2, kind);
    s.number(3, expires);
    s.row();
    Statement move(db_, "UPDATE messages SET folder='Outbox',unread=0 WHERE hash=?");
    move.text(1, id);
    move.row();
    deliveryUpdate(id, "queued", "Queued for encrypted delivery", 0);
    t.commit();
}

namespace {
OutboxItem readOutgoing(Statement &s, const Mailbox &mailbox) {
    OutboxItem o;
    o.id = s.text(0);
    o.kind = s.text(1);
    o.state = s.text(2);
    o.error = s.text(3);
    o.objectHash = s.text(4);
    o.ackToken = s.blob(5);
    o.ackObject = s.blob(6);
    o.expires = s.number(7);
    o.nextAttempt = s.number(8);
    o.attempts = int(s.number(9));
    o.message = mailbox.message(o.id);
    return o;
}
} // namespace

std::vector<OutboxItem> Mailbox::outbox() const {
    std::vector<OutboxItem> result;
    Statement letters(
        db_,
        "SELECT id,kind,state,error,object_hash,ack_token,ack_object,expires,next_attempt,attempts "
        "FROM outbox ORDER BY rowid");
    while (letters.row())
        result.push_back(readOutgoing(letters, *this));
    // Chats stay in the mailbox for good, so those whose delivery is over (the terminal states
    // of delivery.cpp) are left out: every tick and every scanned pubkey walks this list.
    for (auto &chat : outboxChats())
        result.push_back(std::move(chat));
    return result;
}

OutboxItem Mailbox::outgoing(const std::string &id) const {
    if (id.rfind("chat-", 0) == 0) {
        auto chat = outgoingChat(id);
        require(chat.has_value(), "No delivery record for this chat");
        return std::move(*chat);
    }
    Statement s(
        db_,
        "SELECT id,kind,state,error,object_hash,ack_token,ack_object,expires,next_attempt,attempts "
        "FROM outbox WHERE id=?");
    s.text(1, id);
    require(s.row(), "No delivery record for this letter");
    return readOutgoing(s, *this);
}

void Mailbox::deliveryUpdate(const std::string &id, const std::string &state,
                             const std::string &detail, int64_t next) {
    require(knownDeliveryState(state), "Invalid delivery state");
    const bool chat = id.rfind("chat-", 0) == 0;
    Statement s(db_, chat ? "UPDATE chat_outbox SET state=?,error=?,next_attempt=? WHERE id=?"
                          : "UPDATE outbox SET state=?,error=?,next_attempt=? WHERE id=?");
    s.text(1, state);
    s.text(2, state == "failed" || state == "expired" ? detail : std::string());
    s.number(3, next);
    s.text(4, id);
    s.row();
    require(sqlite3_changes(db_) == 1, "Delivery no longer exists");
    if (chat) {
        Statement status(db_, "UPDATE chat_messages SET state=? WHERE id=?");
        status.text(1, state);
        status.text(2, id);
        status.row();
        return;
    }
    Statement e(db_, "INSERT INTO delivery_events(id,ts,state,detail) VALUES(?,?,?,?)");
    e.text(1, id);
    e.number(2, now());
    e.text(3, state);
    e.text(4, detail);
    e.row();
    if (state == "acknowledged" || state == "published") {
        Statement move(db_, "UPDATE messages SET folder='Sent' WHERE hash=? AND folder='Outbox'");
        move.text(1, id);
        move.row();
    }
}

void Mailbox::setDelivery(const std::string &id, const std::string &state,
                          const std::string &detail, int64_t next) {
    Transaction t(db_);
    deliveryUpdate(id, state, detail, next);
    t.commit();
}

void Mailbox::setAck(const std::string &id, ByteView token, ByteView object) {
    require(token.size() == 32, "Invalid acknowledgment token");
    Statement s(db_, id.rfind("chat-", 0) == 0
                         ? "UPDATE chat_outbox SET ack_token=?,ack_object=? WHERE id=?"
                         : "UPDATE outbox SET ack_token=?,ack_object=? WHERE id=?");
    s.blob(1, token);
    s.blob(2, object);
    s.text(3, id);
    s.row();
}

void Mailbox::setObjectHash(const std::string &id, const std::string &hash) {
    Statement s(db_, id.rfind("chat-", 0) == 0 ? "UPDATE chat_outbox SET object_hash=? WHERE id=?"
                                               : "UPDATE outbox SET object_hash=? WHERE id=?");
    s.text(1, hash);
    s.text(2, id);
    s.row();
}

bool Mailbox::acknowledge(ByteView token) {
    if (token.size() != 32)
        return false;
    std::vector<std::string> ids;
    {
        Statement s(db_, "SELECT id FROM outbox WHERE ack_token=? AND state NOT IN "
                         "('cancelled','acknowledged')");
        s.blob(1, token);
        while (s.row())
            ids.push_back(s.text(0));
    }
    for (const auto &id : chatAckIds(token))
        ids.push_back(id);
    if (ids.empty())
        return false;
    Transaction t(db_);
    for (const auto &id : ids) {
        deliveryUpdate(id, "acknowledged",
                       "Recipient acknowledgment received; this is not a read receipt", 0);
        Statement jobs(db_, "DELETE FROM network_jobs WHERE owner=?");
        jobs.text(1, id);
        jobs.row();
    }
    t.commit();
    return true;
}

void Mailbox::retry(const std::string &id, int64_t expires) {
    auto o = outgoing(id);
    require(!o.chat, "Chats cannot be retried after their 24-hour expiry");
    require(o.state != "acknowledged", "This letter has already been acknowledged");
    Transaction t(db_);
    Statement clear(db_, "DELETE FROM network_jobs WHERE owner=?");
    clear.text(1, id);
    clear.row();
    Statement s(
        db_,
        "UPDATE outbox SET expires=?,ack_object=X'',object_hash='',attempts=attempts+1 WHERE id=?");
    s.number(1, expires);
    s.text(2, id);
    s.row();
    Statement move(db_, "UPDATE messages SET folder='Outbox' WHERE hash=?");
    move.text(1, id);
    move.row();
    deliveryUpdate(id, "queued", "Delivery retried; previously published copies cannot be recalled",
                   0);
    t.commit();
}

void Mailbox::cancel(const std::string &id) {
    auto o = outgoing(id);
    require(o.state != "acknowledged" && o.state != "published", "Delivery is already complete");
    Transaction t(db_);
    Statement s(db_, "DELETE FROM network_jobs WHERE owner=?");
    s.text(1, id);
    s.row();
    deliveryUpdate(id, "cancelled",
                   "Further work stopped; already published copies remain on the network", 0);
    t.commit();
}

std::vector<DeliveryEvent> Mailbox::events(const std::string &id) const {
    Statement s(db_, "SELECT ts,state,detail FROM delivery_events WHERE id=? ORDER BY seq");
    s.text(1, id);
    std::vector<DeliveryEvent> r;
    while (s.row())
        r.push_back({s.number(0), s.text(1), s.text(2)});
    return r;
}

void Mailbox::recordMilestone(const std::string &id, const std::string &state,
                              const std::string &detail) {
    if (id.rfind("chat-", 0) == 0)
        return;
    Statement s(db_, "INSERT INTO delivery_events(id,ts,state,detail) SELECT ?,?,?,? WHERE NOT "
                     "EXISTS (SELECT 1 FROM delivery_events WHERE id=? AND state=?)");
    s.text(1, id);
    s.number(2, now());
    s.text(3, state);
    s.text(4, detail);
    s.text(5, id);
    s.text(6, state);
    s.row();
}

std::string Mailbox::addJob(NetworkJob job) {
    if (job.id.empty())
        job.id = uuid();
    if (job.state.empty())
        job.state = "queued";
    require(job.payload.size() <= 262144 && !job.payload.empty(), "Invalid network object size");
    require(job.nonceTrials >= 1000 && job.nonceTrials <= 1000000 && job.extraBytes >= 1000 &&
                job.extraBytes <= 1000000,
            "Recipient proof-of-work requirement exceeds supported limits");
    Statement s(db_, "INSERT OR IGNORE INTO network_jobs VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    s.text(1, job.id);
    s.text(2, job.owner);
    s.text(3, job.kind);
    s.text(4, job.address);
    s.text(5, job.state);
    s.text(6, job.hash);
    s.text(7, job.error);
    s.blob(8, job.payload);
    s.number(9, job.expires);
    s.number(10, job.nonceTrials);
    s.number(11, job.extraBytes);
    s.row();
    return job.id;
}

std::vector<NetworkJob> Mailbox::jobs() const {
    Statement s(db_, "SELECT id,owner,kind,address,state,hash,error,payload,expires,trials,extra "
                     "FROM network_jobs ORDER BY CASE WHEN kind='incoming-ack' THEN 0 WHEN "
                     "kind='pubkey' THEN 1 ELSE 2 END,rowid");
    std::vector<NetworkJob> r;
    while (s.row()) {
        NetworkJob j;
        j.id = s.text(0);
        j.owner = s.text(1);
        j.kind = s.text(2);
        j.address = s.text(3);
        j.state = s.text(4);
        j.hash = s.text(5);
        j.error = s.text(6);
        j.payload = s.blob(7);
        j.expires = s.number(8);
        j.nonceTrials = uint64_t(s.number(9));
        j.extraBytes = uint64_t(s.number(10));
        r.push_back(std::move(j));
    }
    return r;
}

void Mailbox::updateJob(const std::string &id, const std::string &state, const Bytes *payload,
                        const std::string &hash, const std::string &error) {
    Statement s(db_, "UPDATE network_jobs SET state=?,payload=CASE WHEN ? THEN ? ELSE payload "
                     "END,hash=CASE WHEN ? THEN ? ELSE hash END,error=? WHERE id=?");
    s.text(1, state);
    s.number(2, payload != nullptr);
    s.blob(3, payload ? ByteView(*payload) : ByteView());
    s.number(4, !hash.empty());
    s.text(5, hash);
    s.text(6, error);
    s.text(7, id);
    s.row();
}

void Mailbox::removeJob(const std::string &id) {
    Statement s(db_, "DELETE FROM network_jobs WHERE id=?");
    s.text(1, id);
    s.row();
}

void Mailbox::savePublicKey(const StoredPublicKey &k) {
    require(k.signingKey.size() == 65 && k.encryptionKey.size() == 65, "Invalid public-key length");
    Statement s(db_,
                "INSERT INTO public_keys VALUES(?,?,?,?,?,?,?) ON CONFLICT(address) DO UPDATE SET "
                "signing=excluded.signing,encryption=excluded.encryption,trials=excluded.trials,"
                "extra=excluded.extra,behaviors=excluded.behaviors,expires=excluded.expires");
    s.text(1, k.address);
    s.blob(2, k.signingKey);
    s.blob(3, k.encryptionKey);
    s.number(4, int64_t(k.nonceTrials));
    s.number(5, int64_t(k.extraBytes));
    s.number(6, k.behaviors);
    s.number(7, k.expires);
    s.row();
}

std::optional<StoredPublicKey> Mailbox::publicKey(const std::string &address, int64_t time) const {
    Statement s(db_, "SELECT address,signing,encryption,trials,extra,behaviors,expires FROM "
                     "public_keys WHERE address=? AND expires>?");
    s.text(1, address);
    s.number(2, time);
    if (!s.row())
        return {};
    return StoredPublicKey{s.text(0),
                           s.blob(1),
                           s.blob(2),
                           uint64_t(s.number(3)),
                           uint64_t(s.number(4)),
                           uint32_t(s.number(5)),
                           s.number(6)};
}

std::vector<Subscription> Mailbox::subscriptions() const {
    Statement s(db_, "SELECT address,label FROM subscriptions ORDER BY label");
    std::vector<Subscription> r;
    while (s.row())
        r.push_back({s.text(0), s.text(1)});
    return r;
}

void Mailbox::subscribe(const std::string &address, const std::string &label) {
    require(!address.empty(), "Enter a broadcast address");
    Transaction t(db_);
    Statement s(db_, "INSERT INTO subscriptions VALUES(?,?) ON CONFLICT(address) DO UPDATE SET "
                     "label=excluded.label");
    s.text(1, address);
    s.text(2, label);
    s.row();
    // No rescan from here: the scan notices the new subscription and reads the
    // kept broadcasts again on its own (Delivery::catchUpBroadcasts).
    t.commit();
}

void Mailbox::unsubscribe(const std::string &address) {
    Statement s(db_, "DELETE FROM subscriptions WHERE address=?");
    s.text(1, address);
    s.row();
}

std::vector<Contact> Mailbox::contacts() const {
    Statement s(db_,
                "SELECT address,label,added FROM contacts ORDER BY label COLLATE NOCASE,address");
    std::vector<Contact> r;
    while (s.row())
        r.push_back({s.text(0), s.text(1), s.number(2)});
    return r;
}

void Mailbox::saveContact(const std::string &address, const std::string &label) {
    require(!address.empty() && !text::trim(label).empty(),
            "A contact needs an address and a name");
    Statement s(db_, "INSERT INTO contacts VALUES(?,?,?) ON CONFLICT(address) DO UPDATE SET "
                     "label=excluded.label");
    s.text(1, address);
    s.text(2, text::trim(label));
    s.number(3, now());
    s.row();
}

void Mailbox::removeContact(const std::string &address) {
    Statement s(db_, "DELETE FROM contacts WHERE address=?");
    s.text(1, address);
    s.row();
}

void Mailbox::moveMessage(const std::string &id, const std::string &folder) {
    require(folder == "Archive" || folder == "Trash", "Invalid destination folder");
    auto m = message(id);
    if (folder == "Trash") {
        Statement pending(db_, "SELECT state FROM outbox WHERE id=?");
        pending.text(1, id);
        if (pending.row())
            require(isTerminal(pending.text(0)),
                    "Cancel active delivery before moving this letter to Trash");
    }
    Statement s(db_,
                "UPDATE messages SET previous_folder=CASE WHEN folder NOT IN ('Archive','Trash') "
                "THEN folder ELSE previous_folder END,folder=? WHERE hash=?");
    s.text(1, folder);
    s.text(2, id);
    s.row();
}

void Mailbox::restoreMessage(const std::string &id) {
    Statement s(db_, "UPDATE messages SET folder=previous_folder WHERE hash=? AND folder IN "
                     "('Archive','Trash')");
    s.text(1, id);
    s.row();
}

void Mailbox::deleteMessage(const std::string &id) {
    require(message(id).folder == "Trash", "Move the letter to Trash first");
    Transaction t(db_);
    Statement jobs(db_, "DELETE FROM network_jobs WHERE owner=?");
    jobs.text(1, id);
    jobs.row();
    Statement s(db_, "DELETE FROM messages WHERE hash=?");
    s.text(1, id);
    s.row();
    t.commit();
}

void Mailbox::markRead(const std::string &id) {
    Statement s(db_, "UPDATE messages SET unread=0 WHERE hash=? AND unread<>0");
    s.text(1, id);
    s.row();
}

int Mailbox::markRead(const std::vector<std::string> &ids) {
    if (ids.empty())
        return 0;
    Transaction t(db_);
    int changed = 0;
    {
        Statement s(db_, "UPDATE messages SET unread=0 WHERE hash=? AND unread<>0");
        for (const auto &id : ids) {
            s.reset();
            s.text(1, id);
            s.row();
            changed += sqlite3_changes(db_);
        }
    }
    t.commit();
    return changed;
}

std::map<std::string, int> Mailbox::unreadByFolder() const {
    std::map<std::string, int> counts;
    Statement s(db_, "SELECT folder, COUNT(*) FROM messages WHERE unread<>0 AND "
                     "folder IN ('Inbox','Channels','Broadcasts') GROUP BY folder");
    while (s.row())
        counts[s.text(0)] = int(s.number(1));
    return counts;
}

int64_t Mailbox::dataVersion() const {
    Statement s(db_, "PRAGMA data_version");
    return s.row() ? s.number(0) : 0;
}

int Mailbox::unreadLetters() const {
    int total = 0;
    for (const auto &[folder, count] : unreadByFolder())
        total += count;
    return total;
}

bool Mailbox::unread(const std::string &id) const {
    Statement s(db_, "SELECT unread FROM messages WHERE hash=?");
    s.text(1, id);
    return s.row() && s.number(0) != 0;
}

std::string Mailbox::setting(const std::string &name, const std::string &fallback) const {
    Statement s(db_, "SELECT value FROM settings WHERE name=?");
    s.text(1, name);
    return s.row() ? s.text(0) : fallback;
}

void Mailbox::setSetting(const std::string &name, const std::string &value) {
    Statement s(
        db_,
        "INSERT INTO settings VALUES(?,?) ON CONFLICT(name) DO UPDATE SET value=excluded.value");
    s.text(1, name);
    s.text(2, value);
    s.row();
}

std::string letterFingerprint(std::string_view sender, std::string_view recipient,
                              std::string_view subject, std::string_view body) {
    Hasher hasher(Digest::Sha256);
    // Each field with its length in front: where one ends and the next begins is part of the
    // answer.
    for (const auto part : {sender, recipient, subject, body}) {
        Bytes length;
        putBE(length, part.size(), 4);
        hasher.add(length);
        hasher.add(view(part));
    }
    const auto digest = hasher.result();
    return std::string(digest.begin(), digest.end());
}

ImportCounts Mailbox::importLetters(const std::vector<ImportedLetter> &letters) {
    ImportCounts counts;
    static const char *const folders[] = {"Inbox",  "Channels", "Broadcasts", "Sent",
                                          "Drafts", "Trash",    "Archive"};
    for (const auto &l : letters) {
        bool known = false;
        for (const char *folder : folders)
            known = known || l.folder == folder;
        if (l.id.empty() || !known)
            throw Error(Code::Argument,
                        "A letter to import needs an id and a folder of the mailbox");
    }
    if (letters.empty())
        return counts;
    Transaction t(db_);
    {
        Statement insert(db_, "INSERT OR IGNORE INTO messages(hash,sender,recipient,subject,body,"
                              "folder,received,unread,previous_folder,object_hash) "
                              "VALUES(?,?,?,?,?,?,?,?,?,?)");
        Statement kind(db_, "INSERT OR IGNORE INTO settings(name,value) VALUES(?,'broadcast')");
        for (const auto &l : letters) {
            insert.reset();
            insert.text(1, l.id);
            insert.text(2, l.sender);
            insert.text(3, l.recipient);
            insert.text(4, l.subject);
            insert.text(5, l.body);
            insert.text(6, l.folder);
            insert.number(7, l.received);
            // What was sent or written here is never unread; a letter in Trash keeps its flag, as
            // moving one there does.
            insert.number(8, l.unread && l.folder != "Sent" && l.folder != "Drafts" ? 1 : 0);
            insert.text(9, l.previousFolder.empty() ? std::string("Inbox") : l.previousFolder);
            insert.text(10, l.objectHash);
            insert.row();
            if (sqlite3_changes(db_) == 0) {
                ++counts.present;
                continue;
            }
            ++counts.added;
            if (l.broadcast) {
                kind.reset();
                kind.text(1, "draftkind:" + l.id);
                kind.row();
            }
        }
    }
    t.commit();
    return counts;
}

int Mailbox::importContacts(const std::vector<Contact> &contacts) {
    int added = 0;
    Transaction t(db_);
    {
        Statement s(db_, "INSERT OR IGNORE INTO contacts(address,label,added) VALUES(?,?,?)");
        for (const auto &c : contacts) {
            const auto name = text::trim(c.label);
            if (c.address.empty() || name.empty()) // a contact needs an address and a name
                continue;
            s.reset();
            s.text(1, c.address);
            s.text(2, name);
            s.number(3, c.added > 0 ? c.added : now());
            s.row();
            if (sqlite3_changes(db_) > 0)
                ++added;
        }
    }
    t.commit();
    return added;
}

int Mailbox::importSubscriptions(const std::vector<Subscription> &subscriptions) {
    int added = 0;
    Transaction t(db_);
    {
        Statement s(db_, "INSERT OR IGNORE INTO subscriptions(address,label) VALUES(?,?)");
        for (const auto &c : subscriptions) {
            if (c.address.empty())
                continue;
            s.reset();
            s.text(1, c.address);
            s.text(2, c.label);
            s.row();
            if (sqlite3_changes(db_) > 0)
                ++added;
        }
    }
    t.commit();
    return added;
}

std::unordered_set<std::string>
Mailbox::letterFingerprints(const std::vector<std::string> &folders) const {
    std::unordered_set<std::string> fingerprints;
    for (const auto &folder : folders) {
        Statement s(db_, "SELECT sender,recipient,subject,body FROM messages WHERE folder=?");
        s.text(1, folder);
        while (s.row())
            fingerprints.insert(letterFingerprint(s.text(0), s.text(1), s.text(2), s.text(3)));
    }
    return fingerprints;
}
} // namespace bmc
