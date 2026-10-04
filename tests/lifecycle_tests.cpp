#include "cache.h"
#include "ntb-object-db.h"
#include "protocol.h"
#include "scanner.h"
#include "storage.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <sqlcipher/sqlite3.h>
using namespace bm;
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
// Writes an object the way the node does: through its own store.
static void putObject(const QString &root, const QByteArray &object) {
    QDir().mkpath(root);
    auto db = ntb_object_db_open(QFile::encodeName(root).constData());
    require(db, "open the node's object store");
    const auto hash = QByteArray::fromHex(Protocol::inventoryHash(object).toLatin1());
    const bool saved = ntb_object_db_save(db, reinterpret_cast<const uint8_t *>(hash.constData()),
                                          reinterpret_cast<const uint8_t *>(object.constData()),
                                          size_t(object.size()), QDateTime::currentSecsSinceEpoch());
    ntb_object_db_close(db);
    require(saved, "save an object");
}
static void pruneAll(const QString &root) {
    auto db = ntb_object_db_open(QFile::encodeName(root).constData());
    require(db, "open the node's object store");
    ntb_object_db_prune(db, 0, 0);
    ntb_object_db_close(db);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir d;
    try {
        Vault vault;
        const auto vp = d.filePath("vault.bmvault");
        vault.create(vp, "testing password");
        vault.addChannel("general", "General");
        vault.addChannel("notbit", {});
        require(vault.identities().back().label == "[chan] notbit",
                "a chan joined without a label is named \"[chan] <phrase>\", as in PyBitmessage");
        auto key = vault.addMailboxKey();
        auto mp = d.filePath("mail.bmmail");
        Mailbox mailbox;
        mailbox.create(mp, key, vault.mailboxKey(key));
        const auto node = d.filePath("node");
        QDir().mkpath(node);
        {
            QFile old(node + "/cache.sqlite");
            require(old.open(QIODevice::WriteOnly), "an index left by the file-based version");
        }
        Cache cache(node);
        require(!QFile::exists(node + "/cache.sqlite"),
                "the file-based version's cache.sqlite is removed");
        require(cache.id().isEmpty() && cache.after(0).isEmpty() && cache.firstSequence() == 0,
                "nothing to read before the store exists");
        mailbox.bindCache(cache.id());
        auto sender = Protocol::identity("Alice");
        auto object = Protocol::encodeMessage(sender, vault.identities()[0], "Queued while locked",
                                              "Retained correspondence", 1700000000);
        mailbox.close();
        vault.lock();
        putObject(node, object);
        require(cache.after(0).size() == 1 && cache.after(0).first().payload == object,
                "objects stored while locked are readable, bytes intact");
        require(cache.after(0).first().hash == Protocol::inventoryHash(object),
                "a cached object's hash is the lowercase hex inventory hash");
        require(!cache.id().isEmpty(), "the node's store has a cache id");
        require(scanMailbox(cache, mailbox, vault) == 0, "locked scanner must be idle");
        vault.unlock(vp, "testing password");
        mailbox.open(mp, vault.mailboxKey(key));
        mailbox.bindCache(cache.id());
        require(scanMailbox(cache, mailbox, vault) == 1,
                "unlock processes retained expired object");
        require(mailbox.messages().size() == 1, "one decrypted message");
        require(mailbox.messages()[0].body == "Retained correspondence", "decrypted body");
        require(scanMailbox(cache, mailbox, vault) == 0, "checkpoint skips prior objects");
        mailbox.advance(0);
        scanMailbox(cache, mailbox, vault);
        require(mailbox.messages().size() == 1, "crash replay deduplicated");
        auto later = Protocol::channel("a later imported chan", "Later");
        auto retained = Protocol::encodeMessage(sender, later, "Earlier object",
                                                "A newly joined identity", 1700000000);
        putObject(node, retained);
        scanMailbox(cache, mailbox, vault);
        require(mailbox.messages().size() == 1, "unknown identity not decoded yet");
        mailbox.close();
        vault.addChannel("a later imported chan", "Later");
        mailbox.open(mp, vault.mailboxKey(key));
        mailbox.bindCache(cache.id());
        scanMailbox(cache, mailbox, vault);
        require(mailbox.messages().size() == 2,
                "identity added with mailbox closed must rescan retained objects");
        // A peer's object larger than the protocol allows is skipped, not fatal.
        putObject(node, QByteArray(NTB_OBJECT_DB_MAX_OBJECT_SIZE + 1, 'z'));
        const auto oversized = cache.after(mailbox.checkpoint()).last().sequence;
        scanMailbox(cache, mailbox, vault);
        require(mailbox.checkpoint() == oversized, "an oversized object is skipped");
        putObject(d.filePath("other-node"), retained);
        Cache other(d.filePath("other-node"));
        mailbox.bindCache(other.id());
        require(mailbox.checkpoint() == 0,
                "moving mailbox resets checkpoint for a different cache");
        auto before = cache.after(0).last().sequence;
        pruneAll(node);
        require(cache.after(0).isEmpty() && cache.firstSequence() == 0, "pruned objects are gone");
        auto second = Protocol::encodeMessage(sender, vault.identities()[0], "New", "After pruning",
                                              1700000000);
        putObject(node, second);
        require(cache.after(0).first().sequence > before, "cache sequences never reused");
        require(cache.firstSequence() == cache.after(0).first().sequence,
                "the first sequence is the oldest object still stored");
        require(cache.hasAfter(before) && !cache.hasAfter(cache.after(0).first().sequence),
                "hasAfter says whether anything newer is stored");
        // The node empties the store on a schema change; the app's open
        // connection must see the new cache id.
        const auto idBefore = cache.id();
        {
            sqlite3 *raw = nullptr;
            require(sqlite3_open(QFile::encodeName(node + "/" NTB_OBJECT_DB_FILE).constData(), &raw) ==
                        SQLITE_OK,
                    "open the store directly");
            require(sqlite3_exec(raw, "PRAGMA user_version=99", nullptr, nullptr, nullptr) ==
                        SQLITE_OK,
                    "mark the store as another schema version");
            sqlite3_close(raw);
        }
        require(cache.id().isEmpty(), "a store of another version is not read");
        ntb_object_db_close(ntb_object_db_open(QFile::encodeName(node).constData()));
        require(!cache.id().isEmpty() && cache.id() != idBefore && cache.after(0).isEmpty(),
                "a recreated store has a new cache id and no objects");
        // The two-byte coordinate size used to permit an out-of-bounds read in notbit.
        auto malformed = object;
        malformed[40] = char(0xff);
        malformed[41] = char(0xff);
        require(!Protocol::decodeMessage(malformed, vault.identities()[0]),
                "reject oversized EC coordinate");
        std::cout << "PASS: locked collection, unlock scan, expired local object, crash replay, "
                     "cache relocation, retention, oversized objects, store recreation, "
                     "malformed ECIES\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
