#include "storage.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <sqlcipher/sqlite3.h>
using namespace bm;
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
template <class F> static void rejects(F f) {
    bool bad = false;
    try {
        f();
    } catch (...) {
        bad = true;
    }
    require(bad, "expected rejection");
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir d;
    try {
        Vault v;
        v.create(d.filePath("v.bmvault"), "test password");
        auto key = v.addMailboxKey();
        auto path = d.filePath("m.bmmail");
        Mailbox m;
        m.create(path, key, v.mailboxKey(key));
        auto id = m.saveDraft({}, "sender", "recipient", "old subject", "draft body");
        require(m.saveDraft(id, "sender", "recipient", "edited subject", "edited body") == id,
                "draft edit preserves ID");
        require(m.messages().size() == 1 && m.message(id).subject == "edited subject",
                "draft saved in place");
        m.queueDraft(id, "direct", 2000000000);
        require(m.outgoing(id).state == "queued" && m.message(id).folder == "Outbox",
                "draft queued");
        rejects(
            [&] { m.saveDraft(id, "sender", "recipient", "oops", "must not edit active send"); });
        m.setAck(id, QByteArray(32, 'a'));
        m.setDelivery(id, "awaiting_ack", "published", 123);
        m.close();
        m.open(path, v.mailboxKey(key));
        require(m.outgoing(id).ackToken == QByteArray(32, 'a'), "ack state survives reopen");
        require(m.acknowledge(QByteArray(32, 'a')), "matching acknowledgment");
        require(m.outgoing(id).state == "acknowledged" && m.message(id).folder == "Sent",
                "ack marks sent");
        require(!m.acknowledge(QByteArray(32, 'b')), "unrelated ack ignored");
        auto cancel = m.saveDraft({}, "s", "r", "cancel", "body");
        m.queueDraft(cancel, "direct", 2000000000);
        NetworkJob j;
        j.owner = cancel;
        j.kind = "message";
        j.payload = "encrypted object";
        j.state = "ready";
        m.addJob(j);
        m.cancel(cancel);
        require(m.outgoing(cancel).state == "cancelled" && m.jobs().isEmpty(),
                "cancel removes pending jobs");
        m.retry(cancel, 2100000000);
        require(m.outgoing(cancel).state == "queued", "retry restores queued");
        m.cancel(cancel);
        m.moveMessage(cancel, "Trash");
        m.restoreMessage(cancel);
        require(m.message(cancel).folder == "Outbox", "restore former folder");
        m.subscribe("BM-test", "News");
        require(m.subscriptions().size() == 1, "subscription persists");
        m.unsubscribe("BM-test");
        require(m.subscriptions().isEmpty(), "unsubscribe");
        m.close();
        // Migrate a genuine v1 schema with a user's existing draft, including earliest v1 meta
        // variant.
        auto legacy = d.filePath("legacy.bmmail");
        sqlite3 *db = nullptr;
        require(sqlite3_open(QFile::encodeName(legacy).constData(), &db) == SQLITE_OK, "legacy db");
        sqlite3_key(db, v.mailboxKey(key).data(), 32);
        require(
            sqlite3_exec(db,
                         "CREATE TABLE meta(id TEXT NOT NULL,checkpoint INTEGER NOT NULL); INSERT "
                         "INTO meta VALUES('legacy',17);CREATE TABLE messages(hash TEXT PRIMARY "
                         "KEY,sender TEXT NOT NULL,recipient TEXT NOT NULL,subject TEXT NOT "
                         "NULL,body TEXT NOT NULL,folder TEXT NOT NULL,received INTEGER NOT "
                         "NULL);INSERT INTO messages VALUES('draft-old','','BM-to','My existing "
                         "draft','Keep this text','Drafts',123);PRAGMA user_version=1;",
                         nullptr, nullptr, nullptr) == SQLITE_OK,
            "legacy schema");
        sqlite3_close(db);
        m.open(legacy, v.mailboxKey(key));
        require(m.message("draft-old").body == "Keep this text", "migration preserves user draft");
        require(m.checkpoint() == 17, "migration preserves checkpoint");
        m.saveDraft("draft-old", "new sender", "BM-to", "My existing draft", "Edited safely");
        m.close();
        m.open(legacy, v.mailboxKey(key));
        require(m.message("draft-old").body == "Edited safely",
                "migrated draft usable after reopen");
        std::cout << "PASS: editable drafts, durable outbox/ack, cancel/retry, trash/restore, "
                     "subscriptions and v1 migration\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
