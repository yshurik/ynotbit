#include "protocol.h"
#include "storage.h"
#include <QCoreApplication>
#include <QDataStream>
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
        require(vault.identities()[0].isDefault, "sole identity is the default");
        auto work = vault.addIdentity("Work");
        require(!vault.identities()[1].isDefault, "second identity is not the default yet");
        vault.setDefaultIdentity(work);
        require(!vault.identities()[0].isDefault && vault.identities()[1].isDefault,
                "default moves to the chosen identity");
        rejects([&] { vault.setDefaultIdentity("BM-does-not-exist"); });
        vault.deleteIdentity(id);
        require(vault.identities().size() == 1 && vault.identities()[0].address == work,
                "identity removed");
        require(vault.identities()[0].isDefault,
                "deleting the default identity promotes the remaining one");
        rejects([&] { vault.deleteIdentity("BM-does-not-exist"); });
        {
            auto legacyPath = d.filePath("legacy.bmvault");
            QByteArray salt(16, 0);
            randombytes_buf(reinterpret_cast<unsigned char *>(salt.data()), 16);
            QByteArray password = "legacy test password";
            unsigned char key[32];
            require(crypto_pwhash(key, 32, password.constData(), password.size(),
                                  reinterpret_cast<const unsigned char *>(salt.constData()), 3,
                                  64 * 1024 * 1024, crypto_pwhash_ALG_ARGON2ID13) == 0,
                    "derive legacy key");
            QByteArray plain;
            QDataStream out(&plain, QIODevice::WriteOnly);
            out.setVersion(QDataStream::Qt_6_0);
            out << quint32(1);
            out << QString("Legacy") << QString("BM-2cLegacyAddressForTestOnly") << false;
            char keyBytes[64] = {0};
            out.writeRawData(keyBytes, 64);
            out << quint32(0);
            QByteArray header("BMVAULT1", 8);
            header += salt;
            QByteArray nonce(24, 0);
            randombytes_buf(reinterpret_cast<unsigned char *>(nonce.data()), nonce.size());
            header += nonce;
            QByteArray cipher(plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, 0);
            unsigned long long clen = 0;
            crypto_aead_xchacha20poly1305_ietf_encrypt(
                reinterpret_cast<unsigned char *>(cipher.data()), &clen,
                reinterpret_cast<const unsigned char *>(plain.constData()), plain.size(),
                reinterpret_cast<const unsigned char *>(header.constData()), header.size(),
                nullptr, reinterpret_cast<const unsigned char *>(nonce.constData()), key);
            QFile f(legacyPath);
            require(f.open(QIODevice::WriteOnly), "write legacy vault file");
            f.write(header);
            f.write(cipher);
            f.close();
            Vault legacyVault;
            legacyVault.unlock(legacyPath, password);
            require(legacyVault.identities().size() == 1, "legacy vault loads one identity");
            require(legacyVault.identities()[0].label == "Legacy", "legacy identity label");
            require(legacyVault.identities()[0].isDefault,
                    "legacy identity becomes the default on load");
        }
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
