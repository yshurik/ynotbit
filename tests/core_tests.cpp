#include "gpu_pow.h"
#include "message_search.h"
#include "pow.h"
#include "protocol.h"
#include "protocol_wire.h"
#include "storage.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
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
        box.saveContact("BM-2cZebraAddressForTestOnly", "zebra");
        box.saveContact("BM-2cAliceAddressForTestOnly", "Alice");
        require(box.contacts().size() == 2 && box.contacts()[0].label == "Alice" &&
                    box.contacts()[1].label == "zebra" && box.contacts()[0].added > 0,
                "contacts are listed by name, case-insensitively, with their add time");
        box.saveContact("BM-2cAliceAddressForTestOnly", "  Alice W.  ");
        require(box.contacts().size() == 2 && box.contacts()[0].label == "Alice W.",
                "saving a known address renames it (trimmed) rather than duplicating it");
        rejects([&] { box.saveContact("BM-2cAliceAddressForTestOnly", "   "); });
        box.close();
        box.open(mail, vault.mailboxKey(keyId));
        require(box.contacts().size() == 2, "contacts persist in the encrypted mailbox");
        box.removeContact("BM-2cZebraAddressForTestOnly");
        require(box.contacts().size() == 1 && box.contacts()[0].address ==
                    "BM-2cAliceAddressForTestOnly", "removing a contact deletes only that one");
        box.removeContact("BM-2cAliceAddressForTestOnly");
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
        {
            Vault filterVault;
            filterVault.create(d.filePath("filter.bmvault"), "filter test password");
            auto filterKey = filterVault.addMailboxKey();
            Mailbox filterBox;
            filterBox.create(d.filePath("filter.bmmail"), filterKey,
                             filterVault.mailboxKey(filterKey));
            // "Anonymous" chan posts are signed by the chan's own shared key, so
            // sender == recipient; a direct message to the chan keeps its own
            // sender identity, so sender != recipient (see delivery.cpp's
            // chanBroadcast handling, which this mirrors from storage).
            filterBox.store("anon-unread", "BM-chan", "BM-chan", "Anon unread", "b", 1, "Channels");
            filterBox.store("anon-read", "BM-chan", "BM-chan", "Anon read", "b", 2, "Channels");
            filterBox.markRead("anon-read");
            filterBox.store("named-unread", "BM-alice", "BM-chan", "Named unread", "b", 3,
                            "Channels");
            filterBox.store("named-read", "BM-alice", "BM-chan", "Named read", "b", 4, "Channels");
            filterBox.markRead("named-read");
            require(filterBox.messageCount("Channels", "", "BM-chan") == 4,
                    "unfiltered channel count");
            require(filterBox.messageCount("Channels", "", "BM-chan", true, false) == 2,
                    "unread-only count spans anonymous and named senders");
            require(filterBox.messageCount("Channels", "", "BM-chan", false, true) == 2,
                    "anonymous-only count spans read and unread");
            require(filterBox.messageCount("Channels", "", "BM-chan", true, true) == 1,
                    "unread and anonymous combine as AND, not OR");
            auto onlyAnonUnread =
                filterBox.messageSummaries("Channels", "", 0, 10, "BM-chan", true, true);
            require(onlyAnonUnread.size() == 1 && onlyAnonUnread[0].hash == "anon-unread",
                    "combined filter returns exactly the matching message");
            // The list's order and positions come from the index, not the bodies.
            const auto order = filterBox.messageHashes("Channels", "BM-chan");
            require(order.size() == 4 && order[0] == "named-read" && order[1] == "named-unread",
                    "hashes in list order, newest first");
            for (int i = 0; i < order.size(); ++i)
                require(filterBox.messagePosition(order[i], "Channels", "BM-chan") == i,
                        "a letter's position is its row in the list");
            require(filterBox.messagePosition("named-read", "Channels", "BM-chan", true) == -1,
                    "a letter the filter hides has no position");
            require(filterBox.messageHashes("Channels", "BM-chan", true) ==
                        QStringList(order).filter("unread"),
                    "hashes honour the unread filter");
            require(filterBox.messageSummaries(QStringList{"named-read", "gone", "named-unread"})
                            .size() == 2,
                    "summaries by hash skip letters that no longer exist");
            {
                // The filter box's search runs on its own thread and connection.
                MessageSearch search;
                search.open(d.filePath("filter.bmmail"), filterVault.mailboxKey(filterKey));
                quint64 current = 0, failedSearch = 0;
                QStringList done, matched;
                QObject::connect(&search, &MessageSearch::checked,
                                 [&](quint64 id, const QStringList &d, const QStringList &m) {
                                     if (id == current) {
                                         done += d;
                                         matched += m;
                                     }
                                 });
                QObject::connect(&search, &MessageSearch::failed,
                                 [&](quint64 id, const QString &) { failedSearch = id; });
                const auto settle = [&](const std::function<bool()> &until) {
                    QElapsedTimer clock;
                    clock.start();
                    while (!until() && clock.elapsed() < 10000)
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    return until();
                };
                current = search.start("NAMED", order);
                require(settle([&] { return done.size() == order.size(); }),
                        "every letter is checked");
                require(done == order && matched == QStringList({"named-read", "named-unread"}),
                        "matches subject case-insensitively, in list order");
                // A newer search abandons the older one at its next letter.
                QStringList many;
                for (int i = 0; i < 20000; ++i)
                    many += order;
                quint64 abandoned = search.start("zzz", many);
                int abandonedDone = 0;
                QObject::connect(&search, &MessageSearch::checked,
                                 [&](quint64 id, const QStringList &d, const QStringList &) {
                                     if (id == abandoned)
                                         abandonedDone += int(d.size());
                                 });
                done.clear();
                matched.clear();
                current = search.start("alice", order);
                require(settle([&] { return done.size() == order.size(); }),
                        "the newer search completes");
                require(matched == QStringList({"named-read", "named-unread"}),
                        "matches the sender too");
                require(abandonedDone < many.size(), "the older search stopped early");
                search.close();
                const auto closedSearch = search.start("x", order);
                require(settle([&] { return failedSearch == closedSearch; }),
                        "a closed search reports failure instead of hanging");
            }
            filterBox.close();
        }
        {
            // Proof of work runs on several cores and still yields one valid object.
            const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
            require(ProofOfWork::workerCount() == std::max(1u, cores - 1),
                    "proof of work leaves one core for the window");
            const auto now = QDateTime::currentSecsSinceEpoch();
            const auto object = Wire::acknowledgment(QByteArray(32, 'p'), now + 3600);
            ProofOfWork pow;
            const auto waitDone = [&](int ms) {
                QElapsedTimer t;
                t.start();
                while (!pow.done() && t.elapsed() < ms)
                    QThread::msleep(5);
                return pow.done();
            };
            pow.start(object);
            require(waitDone(120000), "proof of work finishes");
            const auto solved = pow.take();
            require(ProofOfWork::valid(solved, now), "solved object passes the network check");
            require(solved.mid(8) == object.mid(8), "only the nonce changes");
            require(!pow.done() && pow.take().isEmpty(), "a result is taken once");

            // Stopping a search nobody can finish returns at once, and the
            // same instance then solves the next object.
            pow.start(object, 1000000, 1000000);
            QThread::msleep(100);
            QElapsedTimer stopping;
            stopping.start();
            pow.stop();
            require(stopping.elapsed() < 1000, "stop() ends every worker promptly");
            require(!pow.done() && pow.take().isEmpty(), "a stopped search has no result");
            pow.start(object);
            require(waitDone(120000), "a restarted search finishes");
            require(ProofOfWork::valid(pow.take(), now), "restarted result is valid");

            // Destroying a running search must not hang or crash.
            {
                ProofOfWork running;
                running.start(object, 1000000, 1000000);
                QThread::msleep(50);
            }

            // With the GPU switched off the CPU alone still solves.
            ProofOfWork::setGpuEnabled(false);
            require(!ProofOfWork::gpuEnabled(), "GPU can be switched off");
            pow.start(object);
            require(waitDone(120000), "CPU-only search finishes");
            require(ProofOfWork::valid(pow.take(), now), "CPU-only result is valid");
            ProofOfWork::setGpuEnabled(true);
        }
        {
            // The GPU, when this machine has one, computes the same trial values.
            auto &gpu = GpuSolver::instance();
            if (!gpu.available()) {
                std::cout << "note: no OpenCL GPU here (" << gpu.problem().toStdString()
                          << "); GPU checks skipped\n";
            } else {
                std::cout << "GPU: " << gpu.deviceName().toStdString() << "\n";
                unsigned char initial[64];
                for (int i = 0; i < 64; ++i)
                    initial[i] = static_cast<unsigned char>(i * 37 + 11);
                const quint64 easy = std::numeric_limits<quint64>::max() >> 12;
                std::atomic_bool cancel{false};
                const auto nonce = gpu.search(initial, easy, 12345, cancel);
                require(nonce.has_value(), "GPU finds an easy nonce");
                require(ProofOfWork::trialValue(*nonce, initial) <= easy,
                        "GPU nonce passes the CPU check");

                // Cancelling an impossible search returns within a batch or two.
                std::thread canceller([&] {
                    QThread::msleep(100);
                    cancel = true;
                });
                QElapsedTimer t;
                t.start();
                cancel = false;
                const auto none = gpu.search(initial, 0, 0, cancel);
                canceller.join();
                require(!none.has_value(), "a cancelled GPU search has no result");
                require(t.elapsed() < 1500, "a GPU search stops promptly");
            }
        }
        std::cout << "PASS: vault, password rotation, SQLCipher, deduplication, backup, chan "
                     "fixture, authenticated message codec\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
