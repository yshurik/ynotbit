#include "protocol.h"
#include "storage.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <functional>
#include <iostream>
#include <sqlcipher/sqlite3.h>
using namespace bm;
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
static void rejects(const std::function<void()> &f) {
    bool bad = false;
    try {
        f();
    } catch (const std::exception &) {
        bad = true;
    }
    require(bad, "expected rejection");
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir d;
    try {
        Vault vault;
        auto path = d.filePath("personal.bmvault");
        vault.create(path, "long test password");
        auto id = vault.addIdentity("Personal");
        require(id.startsWith("BM-"), "identity address");
        rejects([&] { vault.addIdentity(QString(3 * 1024 * 1024, QChar('X'))); });
        require(vault.identities().size() == 1, "oversized vault update rolled back");
        auto keyId = vault.addMailboxKey();
        auto mail = d.filePath("letters.bmmail");
        Mailbox box;
        box.create(mail, keyId, vault.mailboxKey(keyId));
        box.store("object-a", "Alice", "Personal", "A private subject", "secret message marker",
                  10);
        box.store("object-a", "Alice", "Personal", "A private subject", "secret message marker",
                  10);
        require(box.messages().size() == 1, "duplicate delivery");
        require(box.checkpoint() == 10, "checkpoint commit");
        box.close();
        sqlite3 *injected = nullptr;
        require(sqlite3_open(QFile::encodeName(mail).constData(), &injected) == SQLITE_OK,
                "open transaction test database");
        require(sqlite3_key(injected, vault.mailboxKey(keyId).data(), 32) == SQLITE_OK,
                "key transaction test database");
        require(sqlite3_exec(
                    injected,
                    "CREATE TRIGGER reject_checkpoint BEFORE UPDATE OF checkpoint ON meta WHEN "
                    "NEW.checkpoint=999 BEGIN SELECT RAISE(ABORT,'checkpoint write failed'); END",
                    nullptr, nullptr, nullptr) == SQLITE_OK,
                "install failure trigger");
        sqlite3_close(injected);
        box.open(mail, vault.mailboxKey(keyId));
        rejects([&] {
            box.store("rejected-object", "Alice", "Personal", "Must roll back", "uncommitted", 999);
        });
        require(box.messages().size() == 1 && box.checkpoint() == 10,
                "message and checkpoint roll back together");
        box.close();
        vault.lock();
        rejects([&] { vault.mailboxKey(keyId); });
        rejects([&] { box.messages(); });
        rejects([&] { vault.unlock(path, "incorrect password"); });
        vault.unlock(path, "long test password");
        box.open(mail, vault.mailboxKey(keyId));
        require(box.messages().size() == 1, "reopen persisted mailbox");
        require(box.messages()[0].body == "secret message marker", "decrypted message");
        auto second = vault.addMailboxKey();
        Mailbox wrong;
        rejects([&] { wrong.open(mail, vault.mailboxKey(second)); });
        auto backup = d.filePath("backup.bmmail");
        box.backup(backup, vault.mailboxKey(keyId));
        Mailbox restored;
        restored.open(backup, vault.mailboxKey(keyId));
        require(restored.messages().size() == 1, "backup restore");
        restored.close();
        vault.changePassword("a changed strong password");
        box.close();
        vault.lock();
        rejects([&] { vault.unlock(path, "long test password"); });
        vault.unlock(path, "a changed strong password");
        require(vault.identities().size() == 1, "identity survives password change");
        vault.lock();
        QFile vf(path);
        require(vf.open(QIODevice::ReadWrite), "vault read");
        auto bytes = vf.readAll();
        require(!bytes.contains("Personal"), "vault metadata plaintext leak");
        bytes[bytes.size() - 1] ^= 1;
        vf.seek(0);
        vf.write(bytes);
        vf.close();
        rejects([&] { vault.unlock(path, "a changed strong password"); });
        QFile mf(mail);
        require(mf.open(QIODevice::ReadOnly), "mail read");
        bytes = mf.readAll();
        require(!bytes.contains("secret message marker") && !bytes.contains("A private subject"),
                "mail plaintext leak");
        require(!bytes.startsWith("SQLite format"), "mailbox must be SQLCipher");
        auto chan = Protocol::channel("general", "general", 3);
        require(chan.address == "BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr",
                "reference general chan address");
        Vault imported;
        auto importedPath = d.filePath("imported.bmvault");
        imported.create(importedPath, "import test password");
        QFile sourceKeys(d.filePath("keys.dat"));
        require(sourceKeys.open(QIODevice::WriteOnly), "create import fixture");
        sourceKeys.write(
            "[BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr]\nlabel = General\nchan = true\nprivsigningkey "
            "= 5Jnbdwc4u4DG9ipJxYLznXSvemkRFueQJNHujAQamtDDoX3N1eQ\nprivencryptionkey = "
            "5JrDcFtQDv5ydcHRW6dfGUEvThoxCCLNEUaxQfy8LXXgTJzVAcq\n");
        sourceKeys.close();
        imported.importKeys(sourceKeys.fileName());
        require(imported.identities().size() == 1 &&
                    imported.identities()[0].address == chan.address,
                "import known keys.dat fixture");
        require(QFile::exists(sourceKeys.fileName()), "import preserves source");
        imported.lock();
        imported.unlock(importedPath, "import test password");
        require(imported.identities()[0].chan, "imported chan persists");
        auto recipient = Protocol::identity("Recipient");
        auto sender = Protocol::identity("Sender");
        auto object =
            Protocol::encodeMessage(sender, recipient, "Test subject", "Test body", 1700000000);
        auto decoded = Protocol::decodeMessage(object, recipient);
        require(decoded.has_value() && decoded->body == "Test body" &&
                    decoded->subject == "Test subject",
                "notbit ECIES roundtrip");
        require(!Protocol::decodeMessage(object, sender).has_value(), "wrong recipient");
        object[object.size() - 5] ^= 1;
        require(!Protocol::decodeMessage(object, recipient).has_value(), "tampered ECIES");
        std::cout << "PASS: vault, password rotation, SQLCipher, deduplication, backup, chan "
                     "fixture, authenticated message codec\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
