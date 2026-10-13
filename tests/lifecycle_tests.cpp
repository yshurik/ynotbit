#include "cache.h"
#include "ntb-object-db.h"
#include "protocol.h"
#include "scanner.h"
#include "storage.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <functional>
#include <iostream>
#include <sodium.h>
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
// Gives every identity in a vault file the label set_label returns, as a vault saved
// before chan names took a '#' would hold them. Same layout and crypto as Vault::save.
static void relabelVaultFile(const QString &path, const QByteArray &password,
                             const std::function<QString(const QString &)> &set_label) {
    QFile f(path);
    require(f.open(QIODevice::ReadOnly), "read the vault file");
    const auto file = f.readAll();
    f.close();
    const auto header = file.left(48), salt = file.mid(8, 16), nonce = file.mid(24, 24);
    unsigned char key[32];
    require(crypto_pwhash(key, 32, password.constData(), password.size(),
                          reinterpret_cast<const unsigned char *>(salt.constData()), 3,
                          64 * 1024 * 1024, crypto_pwhash_ALG_ARGON2ID13) == 0,
            "derive the vault key");
    QByteArray plain(file.size() - 48 - 16, 0);
    unsigned long long n = 0;
    require(crypto_aead_xchacha20poly1305_ietf_decrypt(
                reinterpret_cast<unsigned char *>(plain.data()), &n, nullptr,
                reinterpret_cast<const unsigned char *>(file.constData() + 48), file.size() - 48,
                reinterpret_cast<const unsigned char *>(header.constData()), 48,
                reinterpret_cast<const unsigned char *>(nonce.constData()), key) == 0,
            "decrypt the vault");
    QDataStream in(plain);
    in.setVersion(QDataStream::Qt_6_0);
    QByteArray rewritten;
    QDataStream out(&rewritten, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    quint32 count = 0;
    in >> count;
    out << count;
    for (quint32 j = 0; j < count; ++j) {
        QString label, address;
        bool chan = false, isDefault = false;
        in >> label >> address >> chan >> isDefault;
        QByteArray keys(64, 0);
        in.readRawData(keys.data(), 64);
        out << set_label(label) << address << chan << isDefault;
        out.writeRawData(keys.constData(), 64);
    }
    const auto rest = plain.mid(int(in.device()->pos()));
    out.writeRawData(rest.constData(), rest.size());
    QByteArray cipher(rewritten.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, 0);
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char *>(cipher.data()), &n,
        reinterpret_cast<const unsigned char *>(rewritten.constData()), rewritten.size(),
        reinterpret_cast<const unsigned char *>(header.constData()), 48, nullptr,
        reinterpret_cast<const unsigned char *>(nonce.constData()), key);
    require(f.open(QIODevice::WriteOnly) && f.write(header + cipher) == 48 + cipher.size(),
            "write the vault file");
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
        require(vault.identities().back().label == "#notbit",
                "a chan joined without a label is named after its phrase, with a '#'");
        require(vault.identities()[vault.identities().size() - 2].label == "#General",
                "a chan's label takes a '#'");
        {
            // A vault from before: "[chan] <phrase>" and bare names.
            Vault old;
            const auto op = d.filePath("old.bmvault");
            old.create(op, "testing password");
            old.addIdentity("Personal");
            old.addChannel("bitmessage", {});
            old.addChannel("general", {});
            old.addChannel("friends phrase", "#friends");
            old.lock();
            relabelVaultFile(op, "testing password", [](const QString &label) {
                return label == "#bitmessage" ? "[chan] bitmessage"
                       : label == "#general"  ? "general"
                                              : label;
            });
            old.unlock(op, "testing password");
            QStringList labels;
            for (const auto &i : old.identities())
                labels << i.label;
            require(labels == QStringList{"Personal", "#bitmessage", "#general", "#friends"},
                    "opening an older vault names its chans #<name>, and only its chans");
            old.lock();
            old.unlock(op, "testing password");
            require(old.identities()[1].label == "#bitmessage", "and saves the new names");
            old.renameIdentity(old.identities()[2].address, "chat");
            require(old.identities()[2].label == "#chat", "a renamed chan keeps the '#'");
            old.renameIdentity(old.identities()[0].address, "Me");
            require(old.identities()[0].label == "Me", "an identity's name takes no '#'");
            old.lock();
        }
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
