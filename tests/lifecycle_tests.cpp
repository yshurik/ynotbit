#include "cache.h"
#include "protocol.h"
#include "scanner.h"
#include "storage.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
using namespace bm;
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir d;
    try {
        Vault vault;
        const auto vp = d.filePath("vault.bmvault");
        vault.create(vp, "testing password");
        vault.addChannel("general", "General");
        auto key = vault.addMailboxKey();
        auto mp = d.filePath("mail.bmmail");
        Mailbox mailbox;
        mailbox.create(mp, key, vault.mailboxKey(key));
        Cache cache(d.filePath("node"));
        mailbox.bindCache(cache.id());
        auto sender = Protocol::identity("Alice");
        auto object = Protocol::encodeMessage(sender, vault.identities()[0], "Queued while locked",
                                              "Retained correspondence", 1700000000);
        mailbox.close();
        vault.lock();
        auto hash = Protocol::inventoryHash(object);
        QFile f(d.filePath("node/objects/") + hash);
        require(f.open(QIODevice::WriteOnly), "cache file");
        f.write(object);
        f.close();
        cache.discover();
        require(cache.count() == 1, "objects cached while locked");
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
        QFile oldObject(d.filePath("node/objects/") + Protocol::inventoryHash(retained));
        require(oldObject.open(QIODevice::WriteOnly), "retained foreign object");
        oldObject.write(retained);
        oldObject.close();
        cache.discover();
        scanMailbox(cache, mailbox, vault);
        require(mailbox.messages().size() == 1, "unknown identity not decoded yet");
        mailbox.close();
        vault.addChannel("a later imported chan", "Later");
        mailbox.open(mp, vault.mailboxKey(key));
        mailbox.bindCache(cache.id());
        scanMailbox(cache, mailbox, vault);
        require(mailbox.messages().size() == 2,
                "identity added with mailbox closed must rescan retained objects");
        Cache other(d.filePath("other-node"));
        mailbox.bindCache(other.id());
        require(mailbox.checkpoint() == 0,
                "moving mailbox resets checkpoint for a different cache");
        auto before = cache.after(0).first().sequence;
        cache.prune(0, 90);
        require(cache.count() == 0 && !QFile::exists(f.fileName()), "bounded cache pruning");
        auto second = Protocol::encodeMessage(sender, vault.identities()[0], "New", "After pruning",
                                              1700000000);
        QFile next(d.filePath("node/objects/") + Protocol::inventoryHash(second));
        require(next.open(QIODevice::WriteOnly), "new cache file");
        next.write(second);
        next.close();
        cache.discover();
        require(cache.after(0).first().sequence > before, "cache sequences never reused");
        // The two-byte coordinate size used to permit an out-of-bounds read in notbit.
        auto malformed = object;
        malformed[40] = char(0xff);
        malformed[41] = char(0xff);
        require(!Protocol::decodeMessage(malformed, vault.identities()[0]),
                "reject oversized EC coordinate");
        std::cout << "PASS: locked collection, unlock scan, expired local object, crash replay, "
                     "cache relocation, retention, malformed ECIES\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
