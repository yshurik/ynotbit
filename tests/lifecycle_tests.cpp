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
        // Regression: discover() used to rebuild its directory iterator from
        // scratch whenever the objects dir's mtime had changed since that
        // iterator was built, on the theory that a file added mid-backlog
        // shouldn't stay invisible until the backlog finished draining. Under
        // real sustained write traffic the directory's mtime changes on nearly
        // every call, so that reset fired almost every time and progress
        // through a large/growing directory could never get past whatever a
        // fresh scan examines first -- confirmed against a real user's node
        // directory, where 4405 of 17486 cached object files (25%) were never
        // registered despite existing correctly on disk.
        //
        // Assert forward progress deterministically rather than racing a
        // timer: with more objects than one discover() call examines (128),
        // two calls with a write (and thus an mtime bump) in between must
        // register substantially more than one call's worth -- a reset before
        // every call would keep re-scanning close to the same first ~128.
        Cache churn(d.filePath("churn-node"));
        auto writeChurnObject = [&](int n) {
            auto obj = Protocol::encodeMessage(sender, vault.identities()[0],
                                               "Churn " + QString::number(n), "x", 1700000000);
            QFile f(d.filePath("churn-node/objects/") + Protocol::inventoryHash(obj));
            require(f.open(QIODevice::WriteOnly), "churn object file");
            f.write(obj);
        };
        for (int n = 0; n < 300; ++n)
            writeChurnObject(n);
        int written = 300;
        int calls = 0;
        while (churn.count() < written && calls < 60) {
            churn.discover();
            // Session::tick() calls prune() right after discover() on every cycle;
            // with default retention this is a routine no-op call (nothing over
            // the size/age limits), which must not undo discover()'s progress.
            churn.prune(512ll * 1024 * 1024, 90);
            ++calls;
            if (written < 320) {
                writeChurnObject(written);
                ++written;
            }
        }
        require(churn.count() == written,
                "discover() drains a directory within a bounded number of calls even with "
                "a write (and thus an mtime change) and a routine no-op prune() before "
                "almost every call, instead of "
                "resetting progress back toward the same prefix each time");
        std::cout << "PASS: locked collection, unlock scan, expired local object, crash replay, "
                     "cache relocation, retention, malformed ECIES, discover() under write churn\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
