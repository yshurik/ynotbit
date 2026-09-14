#include "sqlite_helpers.h"
#include "storage.h"
#include <QDateTime>
#include <QSet>
#include <QUuid>
namespace bm {
using detail::Statement;
using detail::Transaction;
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
static qint64 now() {
    return QDateTime::currentSecsSinceEpoch();
}
static QString uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
static bool column(sqlite3 *db, const char *table, const QString &name) {
    auto query = QString("PRAGMA table_info(%1)").arg(table).toUtf8();
    Statement s(db, query.constData());
    while (s.row())
        if (s.text(1) == name)
            return true;
    return false;
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
        "CREATE TABLE IF NOT EXISTS settings(name TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS delivery_events_id ON delivery_events(id,seq); PRAGMA "
        "user_version=2;");
    t.commit();
}
QString Mailbox::saveDraft(const QString &id, const QString &from, const QString &to,
                           const QString &subject, const QString &body) {
    require(subject.toUtf8().size() <= 8192 && body.toUtf8().size() <= 200000,
            "Letter is too large");
    auto draft = id.isEmpty() ? "draft-" + uuid() : id;
    Transaction t(db_);
    if (!id.isEmpty()) {
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
Message Mailbox::message(const QString &id) const {
    Statement s(
        db_,
        "SELECT hash,sender,recipient,subject,body,folder,received FROM messages WHERE hash=?");
    s.text(1, id);
    require(s.row(), "Letter no longer exists");
    return {s.text(0), s.text(1), s.text(2), s.text(3), s.text(4), s.text(5), s.number(6)};
}
void Mailbox::queueDraft(const QString &id, const QString &kind, qint64 expires) {
    require(kind == "direct" || kind == "broadcast", "Invalid delivery type");
    auto m = message(id);
    require(m.folder == "Drafts", "Only a draft can be sent");
    require(!m.from.isEmpty() && (kind == "broadcast" || !m.to.isEmpty()),
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
QVector<OutboxItem> Mailbox::outbox() const {
    Statement s(
        db_,
        "SELECT id,kind,state,error,object_hash,ack_token,ack_object,expires,next_attempt,attempts "
        "FROM outbox ORDER BY rowid");
    QVector<OutboxItem> result;
    while (s.row()) {
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
        o.message = message(o.id);
        result << o;
    }
    return result;
}
OutboxItem Mailbox::outgoing(const QString &id) const {
    Statement s(
        db_,
        "SELECT id,kind,state,error,object_hash,ack_token,ack_object,expires,next_attempt,attempts "
        "FROM outbox WHERE id=?");
    s.text(1, id);
    require(s.row(), "No delivery record for this letter");
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
    o.message = message(o.id);
    return o;
}
void Mailbox::deliveryUpdate(const QString &id, const QString &state, const QString &detail,
                             qint64 next) {
    static const QSet<QString> states = {
        "queued",       "awaiting_pubkey", "calculating_ack", "calculating_message",
        "ready",        "publishing",      "awaiting_ack",    "published",
        "acknowledged", "cancelled",       "failed",          "expired"};
    require(states.contains(state), "Invalid delivery state");
    Statement s(db_, "UPDATE outbox SET state=?,error=?,next_attempt=? WHERE id=?");
    s.text(1, state);
    s.text(2, state == "failed" || state == "expired" ? detail : QString());
    s.number(3, next);
    s.text(4, id);
    s.row();
    require(sqlite3_changes(db_) == 1, "Delivery no longer exists");
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
void Mailbox::setDelivery(const QString &id, const QString &state, const QString &detail,
                          qint64 next) {
    Transaction t(db_);
    deliveryUpdate(id, state, detail, next);
    t.commit();
}
void Mailbox::setAck(const QString &id, const QByteArray &token, const QByteArray &object) {
    require(token.size() == 32, "Invalid acknowledgment token");
    Statement s(db_, "UPDATE outbox SET ack_token=?,ack_object=? WHERE id=?");
    s.blob(1, token);
    s.blob(2, object);
    s.text(3, id);
    s.row();
}
void Mailbox::setObjectHash(const QString &id, const QString &hash) {
    Statement s(db_, "UPDATE outbox SET object_hash=? WHERE id=?");
    s.text(1, hash);
    s.text(2, id);
    s.row();
}
bool Mailbox::acknowledge(const QByteArray &token) {
    if (token.size() != 32)
        return false;
    QStringList ids;
    {
        Statement s(db_, "SELECT id FROM outbox WHERE ack_token=? AND state NOT IN "
                         "('cancelled','acknowledged')");
        s.blob(1, token);
        while (s.row())
            ids << s.text(0);
    }
    if (ids.isEmpty())
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
void Mailbox::retry(const QString &id, qint64 expires) {
    auto o = outgoing(id);
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
void Mailbox::cancel(const QString &id) {
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
QVector<DeliveryEvent> Mailbox::events(const QString &id) const {
    Statement s(db_, "SELECT ts,state,detail FROM delivery_events WHERE id=? ORDER BY seq");
    s.text(1, id);
    QVector<DeliveryEvent> r;
    while (s.row())
        r << DeliveryEvent{s.number(0), s.text(1), s.text(2)};
    return r;
}
void Mailbox::recordMilestone(const QString &id, const QString &state, const QString &detail) {
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
QString Mailbox::addJob(NetworkJob job) {
    if (job.id.isEmpty())
        job.id = uuid();
    if (job.state.isEmpty())
        job.state = "queued";
    require(job.payload.size() <= 262144 && !job.payload.isEmpty(), "Invalid network object size");
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
QVector<NetworkJob> Mailbox::jobs() const {
    Statement s(db_, "SELECT id,owner,kind,address,state,hash,error,payload,expires,trials,extra "
                     "FROM network_jobs ORDER BY CASE WHEN kind='incoming-ack' THEN 0 WHEN "
                     "kind='pubkey' THEN 1 ELSE 2 END,rowid");
    QVector<NetworkJob> r;
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
        j.nonceTrials = s.number(9);
        j.extraBytes = s.number(10);
        r << j;
    }
    return r;
}
void Mailbox::updateJob(const QString &id, const QString &state, const QByteArray &payload,
                        const QString &hash, const QString &error) {
    Statement s(db_, "UPDATE network_jobs SET state=?,payload=CASE WHEN ? THEN ? ELSE payload "
                     "END,hash=CASE WHEN ? THEN ? ELSE hash END,error=? WHERE id=?");
    s.text(1, state);
    s.number(2, !payload.isNull());
    s.blob(3, payload);
    s.number(4, !hash.isEmpty());
    s.text(5, hash);
    s.text(6, error);
    s.text(7, id);
    s.row();
}
void Mailbox::removeJob(const QString &id) {
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
    s.number(4, k.nonceTrials);
    s.number(5, k.extraBytes);
    s.number(6, k.behaviors);
    s.number(7, k.expires);
    s.row();
}
std::optional<StoredPublicKey> Mailbox::publicKey(const QString &address, qint64 time) const {
    Statement s(db_, "SELECT address,signing,encryption,trials,extra,behaviors,expires FROM "
                     "public_keys WHERE address=? AND expires>?");
    s.text(1, address);
    s.number(2, time);
    if (!s.row())
        return {};
    return StoredPublicKey{s.text(0),
                           s.blob(1),
                           s.blob(2),
                           quint64(s.number(3)),
                           quint64(s.number(4)),
                           quint32(s.number(5)),
                           s.number(6)};
}
QVector<Subscription> Mailbox::subscriptions() const {
    Statement s(db_, "SELECT address,label FROM subscriptions ORDER BY label");
    QVector<Subscription> r;
    while (s.row())
        r << Subscription{s.text(0), s.text(1)};
    return r;
}
void Mailbox::subscribe(const QString &address, const QString &label) {
    require(!address.isEmpty(), "Enter a broadcast address");
    Transaction t(db_);
    Statement s(db_, "INSERT INTO subscriptions VALUES(?,?) ON CONFLICT(address) DO UPDATE SET "
                     "label=excluded.label");
    s.text(1, address);
    s.text(2, label);
    s.row();
    advance(0);
    t.commit();
}
void Mailbox::unsubscribe(const QString &address) {
    Statement s(db_, "DELETE FROM subscriptions WHERE address=?");
    s.text(1, address);
    s.row();
}
void Mailbox::moveMessage(const QString &id, const QString &folder) {
    require(folder == "Archive" || folder == "Trash", "Invalid destination folder");
    auto m = message(id);
    if (folder == "Trash") {
        Statement pending(db_, "SELECT state FROM outbox WHERE id=?");
        pending.text(1, id);
        if (pending.row())
            require(
                QStringList{"acknowledged", "published", "cancelled", "failed", "expired"}.contains(
                    pending.text(0)),
                "Cancel active delivery before moving this letter to Trash");
    }
    Statement s(db_,
                "UPDATE messages SET previous_folder=CASE WHEN folder NOT IN ('Archive','Trash') "
                "THEN folder ELSE previous_folder END,folder=? WHERE hash=?");
    s.text(1, folder);
    s.text(2, id);
    s.row();
}
void Mailbox::restoreMessage(const QString &id) {
    Statement s(db_, "UPDATE messages SET folder=previous_folder WHERE hash=? AND folder IN "
                     "('Archive','Trash')");
    s.text(1, id);
    s.row();
}
void Mailbox::deleteMessage(const QString &id) {
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
void Mailbox::markRead(const QString &id) {
    Statement s(db_, "UPDATE messages SET unread=0 WHERE hash=? AND unread<>0");
    s.text(1, id);
    s.row();
}
bool Mailbox::unread(const QString &id) const {
    Statement s(db_, "SELECT unread FROM messages WHERE hash=?");
    s.text(1, id);
    return s.row() && s.number(0) != 0;
}
QString Mailbox::setting(const QString &name, const QString &fallback) const {
    Statement s(db_, "SELECT value FROM settings WHERE name=?");
    s.text(1, name);
    return s.row() ? s.text(0) : fallback;
}
void Mailbox::setSetting(const QString &name, const QString &value) {
    Statement s(
        db_,
        "INSERT INTO settings VALUES(?,?) ON CONFLICT(name) DO UPDATE SET value=excluded.value");
    s.text(1, name);
    s.text(2, value);
    s.row();
}
void Vault::renameIdentity(const QString &address, const QString &label) {
    require(unlocked_, "Vault is locked");
    require(label.size() <= 256, "Identity label is too long");
    for (auto &i : identities_)
        if (i.address == address) {
            auto old = i.label;
            i.label = label;
            try {
                save();
            } catch (...) {
                i.label = old;
                throw;
            }
            return;
        }
    throw std::runtime_error("Identity not found");
}
} // namespace bm
