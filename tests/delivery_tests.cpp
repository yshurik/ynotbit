#include "delivery.h"
#include "protocol.h"
#include "protocol_wire.h"
#include "updates.h"
#include "ntb-object-db.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>
using namespace bm;
static void require(bool b, const char *s) {
    if (!b)
        throw std::runtime_error(s);
}
static void write(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    require(f.open(QIODevice::WriteOnly), "open fixture");
    require(f.write(bytes) == bytes.size(), "write fixture");
}
// Writes an object the way the node does: through its own store.
static void cacheObject(const QString &root, const QByteArray &bytes) {
    QDir().mkpath(root);
    auto db = ntb_object_db_open(QFile::encodeName(root).constData());
    require(db, "open the node's object store");
    const auto hash = QByteArray::fromHex(Protocol::inventoryHash(bytes).toLatin1());
    const bool saved = ntb_object_db_save(db, reinterpret_cast<const uint8_t *>(hash.constData()),
                                          reinterpret_cast<const uint8_t *>(bytes.constData()),
                                          size_t(bytes.size()), QDateTime::currentSecsSinceEpoch());
    ntb_object_db_close(db);
    require(saved, "save a fixture object");
}
static int transfer(const QString &source, const QString &destination) {
    int count = 0;
    for (const auto &entry : QDir(source + "/publish").entryInfoList({"*.object"}, QDir::Files)) {
        QFile f(entry.absoluteFilePath());
        require(f.open(QIODevice::ReadOnly), "read spool");
        auto object = f.readAll();
        require(ProofOfWork::valid(object, QDateTime::currentSecsSinceEpoch()),
                "real network proof of work");
        cacheObject(destination, object);
        cacheObject(source, object);
        QDir().mkpath(source + "/receipts");
        write(source + "/receipts/" + entry.completeBaseName() + ".json",
              QJsonDocument(
                  QJsonObject{{"state", "offered"}, {"hash", Protocol::inventoryHash(object)}})
                  .toJson());
        QFile::remove(entry.absoluteFilePath());
        ++count;
    }
    return count;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    try {
        const auto a = temp.filePath("alice"), b = temp.filePath("bob");
        Vault av, bv;
        av.create(temp.filePath("a.bmvault"), "test password");
        bv.create(temp.filePath("b.bmvault"), "test password");
        auto alice = av.addIdentity("Alice"), bob = bv.addIdentity("Bob");
        auto ak = av.addMailboxKey(), bk = bv.addMailboxKey();
        Mailbox am, bm;
        am.create(temp.filePath("a.bmmail"), ak, av.mailboxKey(ak));
        bm.create(temp.filePath("b.bmmail"), bk, bv.mailboxKey(bk));
        Cache ac(a), bc(b);
        am.bindCache(ac.id());
        bm.bindCache(bc.id());
        Delivery ad(a), bd(b);
        auto expires = QDateTime::currentSecsSinceEpoch() + 3600;
        auto id = am.saveDraft({}, alice, bob, "Hello Bob", "Private controller end-to-end body");
        am.queueDraft(id, "direct", expires);
        ad.tick(am, av, false);
        require(am.outgoing(id).state == "awaiting_pubkey", "unknown recipient waits for key");
        require(am.jobs().size() == 1 && am.jobs()[0].kind == "getpubkey", "durable key request");
        require(Wire::requestsIdentity(am.jobs()[0].payload, bv.identities()[0]),
                "key request matches recipient");
        ad.stop();
        // Existing retained pubkey: authenticated by the codec, even if cached before send.
        cacheObject(a, Wire::encodePubkey(bv.identities()[0], expires));
        ad.scan(ac, am, av);
        require(bool(am.publicKey(bob, QDateTime::currentSecsSinceEpoch())),
                "recipient public key saved");
        // Lock during preparation and reopen the durable job. No plaintext spool or ack template.
        ad.tick(am, av, false);
        ad.stop();
        am.close();
        av.lock();
        require(QDir(a + "/publish").entryList({"*.object"}, QDir::Files).isEmpty(),
                "no premature publication while offline");
        av.unlock(temp.filePath("a.bmvault"), "test password");
        am.open(temp.filePath("a.bmmail"), av.mailboxKey(ak));
        const auto deadline = QDateTime::currentMSecsSinceEpoch() + 90000;
        bool received = false, receivedWhileLocked = false;
        bm.close();
        bv.lock();
        while (QDateTime::currentMSecsSinceEpoch() < deadline &&
               am.outgoing(id).state != "acknowledged") {
            ad.tick(am, av, true);
            transfer(a, b);
            if (!bv.unlocked()) {
                for (const auto &cached : bc.after(0, 100)) {
                    const auto raw = cached.payload;
                    auto header = Wire::header(raw);
                    if (header && header->type == 2 && Wire::acknowledgmentToken(raw).isEmpty()) {
                        require(!bm.isOpen(), "mailbox remains closed during receipt");
                        receivedWhileLocked = true;
                        bv.unlock(temp.filePath("b.bmvault"), "test password");
                        bm.open(temp.filePath("b.bmmail"), bv.mailboxKey(bk));
                        break;
                    }
                }
            }
            bd.scan(bc, bm, bv);
            if (bm.isOpen() && !bm.messages().isEmpty())
                received = true;
            // Bob's incoming ACK is already solved; no recipient identity goes to the relay.
            bd.tick(bm, bv, true);
            transfer(b, a);
            ad.scan(ac, am, av);
            QThread::msleep(5);
        }
        require(received && receivedWhileLocked,
                "locked recipient cached, unlocked and decrypted letter");
        require(am.outgoing(id).state == "acknowledged", "sender receives acknowledgment");
        require(am.message(id).folder == "Sent", "acknowledged message moved to Sent");
        require(bm.messages().size() == 1 &&
                    bm.messages()[0].body == "Private controller end-to-end body",
                "exact decrypted body");
        bm.advance(0);
        bd.scan(bc, bm, bv, 100);
        require(bm.messages().size() == 1, "rescan deduplication");
        auto reply =
            bm.saveDraft({}, bob, alice, "Reply", "Authenticated sender key permits replying");
        bm.queueDraft(reply, "direct", expires);
        bd.tick(bm, bv, false);
        require(bm.outgoing(reply).state != "awaiting_pubkey",
                "incoming sender key supports reply");
        bd.cancel(bm, reply);
        require(bm.outgoing(reply).state == "cancelled", "cancel pending send");
        require(bm.jobs().empty() || bm.jobs()[0].owner != reply, "cancel removes private jobs");
        bd.retry(bm, reply);
        require(bm.outgoing(reply).state == "queued", "manual retry");
        bd.cancel(bm, reply);
        auto broadcast =
            Wire::encodeBroadcast(av.identities()[0], "Announcement", "Subscriber only", expires);
        bm.subscribe(alice, "Alice's announcements");
        // A peer's object larger than the protocol allows: skipped, and the
        // broadcast stored after it is still read.
        cacheObject(b, QByteArray(NTB_OBJECT_DB_MAX_OBJECT_SIZE + 1, 'z'));
        cacheObject(b, broadcast);
        // discover() is a bounded, incremental scan (like Session::tick() calls it in
        // production): one call isn't guaranteed to finish, especially under slow I/O.
        bool found = false;
        const auto broadcastDeadline = QDateTime::currentMSecsSinceEpoch() + 5000;
        while (!found && QDateTime::currentMSecsSinceEpoch() < broadcastDeadline) {
            bd.scan(bc, bm, bv, 100);
            for (const auto &m : bm.messages())
                if (m.folder == "Broadcasts" && m.subject == "Announcement")
                    found = true;
            if (!found)
                QThread::msleep(5);
        }
        require(found, "subscribed broadcast decoded");
        // Chan members shouldn't need a separate manual subscribe to hear their own
        // chan's "Anonymous"/broadcast-mode posts -- joining the chan is already that.
        auto alicesChan = av.addChannel("delivery test chan phrase", "Test Chan", {});
        auto bobsChan = bv.addChannel("delivery test chan phrase", "Test Chan", {});
        require(alicesChan == bobsChan, "same phrase derives the same chan address");
        auto chanBroadcast = Wire::encodeBroadcast(av.identities().back(), "Chan announcement",
                                                    "Anonymous-mode post to the chan", expires);
        cacheObject(b, chanBroadcast);
        bool chanFound = false;
        const auto chanDeadline = QDateTime::currentMSecsSinceEpoch() + 5000;
        while (!chanFound && QDateTime::currentMSecsSinceEpoch() < chanDeadline) {
            bd.scan(bc, bm, bv, 100);
            for (const auto &m : bm.messages())
                if (m.folder == "Channels" && m.subject == "Chan announcement")
                    chanFound = true;
            if (!chanFound)
                QThread::msleep(5);
        }
        require(chanFound, "chan member decodes an Anonymous-mode broadcast without subscribing");
        // Release announcements: every mailbox hears the publisher without subscribing.
        require(updates::announcedVersion("ynotbit 0.6.0") == QString("0.6.0") &&
                    updates::announcedVersion("ynotbit v1.2") == QString("1.2") &&
                    !updates::announcedVersion("ynotbit 0.6.0 is out, get it at evil.example") &&
                    !updates::announcedVersion("Re: ynotbit 0.6.0"),
                "only a bare \"ynotbit <version>\" subject announces a version");
        require(updates::compareVersions("0.10.0", "0.9.9") > 0 &&
                    updates::compareVersions("0.5", "0.5.0") == 0 &&
                    updates::compareVersions("0.5.1", "0.6") < 0,
                "versions compare numerically");
        const auto publisher = av.addIdentity("ynotbit releases");
        updates::setPublisherAddressForTesting(publisher);
        auto announce = [&](const QString &subject) {
            for (const auto &i : av.identities())
                if (i.address == publisher)
                    cacheObject(b, Wire::encodeBroadcast(i, subject, "Release notes", expires));
        };
        auto scanFor = [&](const std::function<bool()> &done, int ms) {
            const auto deadline = QDateTime::currentMSecsSinceEpoch() + ms;
            while (!done() && QDateTime::currentMSecsSinceEpoch() < deadline) {
                bd.scan(bc, bm, bv, 100);
                if (!done())
                    QThread::msleep(5);
            }
            return done();
        };
        auto latest = [&] { return bm.setting(updates::kLatestSetting); };
        announce("ynotbit 9.9.9");
        require(scanFor([&] { return latest() == "9.9.9"; }, 5000),
                "a signed release announcement records the version, unsubscribed");
        bool letter = false;
        for (const auto &m : bm.messages())
            letter |= m.folder == "Broadcasts" && m.subject == "ynotbit 9.9.9";
        require(letter, "the announcement is also kept as a letter");
        announce("ynotbit 1.0");
        scanFor([] { return false; }, 300);
        require(latest() == "9.9.9", "an older announcement does not replace a newer one");
        bm.setSetting(updates::kNotifySetting, "off");
        announce("ynotbit 99.0");
        scanFor([] { return false; }, 300);
        require(latest() == "9.9.9", "turned off, announcements are not heard");
        updates::setPublisherAddressForTesting({});
        // Subscribing later reads only the kept broadcasts again: the checkpoint
        // (how far every message has been tried against every identity) stays.
        const auto carol = av.addIdentity("Carol");
        for (const auto &i : av.identities())
            if (i.address == carol)
                cacheObject(b, Wire::encodeBroadcast(i, "Carol's news", "Old post", expires));
        auto drain = [&] {
            const auto deadline = QDateTime::currentMSecsSinceEpoch() + 5000;
            while (QDateTime::currentMSecsSinceEpoch() < deadline) {
                bd.scan(bc, bm, bv, 100);
                if (bc.after(bm.checkpoint(), 1).isEmpty())
                    return;
                QThread::msleep(5);
            }
        };
        drain();
        auto hasCarol = [&] {
            for (const auto &msg : bm.messages())
                if (msg.subject == "Carol's news" && msg.folder == "Broadcasts")
                    return true;
            return false;
        };
        require(!hasCarol(), "an unsubscribed sender's broadcast is passed over");
        const auto reached = bm.checkpoint();
        require(reached > 0, "the scan has reached the end of the cache");
        bm.subscribe(carol, "Carol");
        bd.scan(bc, bm, bv, 1); // one object: a rescan from the start would show
        require(bm.checkpoint() == reached,
                "subscribing does not send the full scan back to the start");
        for (int i = 0; i < 20 && !hasCarol(); ++i)
            bd.scan(bc, bm, bv, 100);
        require(hasCarol(), "the new subscription's kept broadcast is found by the catch-up");
        // Mailboxes from before carry one fingerprint of identities and
        // subscriptions together: upgrading must not rescan everything.
        {
            QStringList entries;
            for (const auto &i : bv.identities())
                entries << i.address;
            for (const auto &sub : bm.subscriptions())
                entries << "subscription:" + sub.address;
            entries.sort();
            bm.bindIdentities(QString::fromLatin1(QCryptographicHash::hash(
                                  entries.join('\n').toUtf8(), QCryptographicHash::Sha256)
                                                      .toHex()),
                              true);
        }
        bd.scan(bc, bm, bv, 1);
        require(bm.checkpoint() == reached, "an older mailbox keeps its checkpoint on upgrade");
        ad.stop();
        bd.stop();
        std::cout << "PASS: key lookup, real PoW, lock/reopen, encrypted handoff, receive, ACK, "
                     "rescan, reply, cancel/retry, broadcasts, chan broadcasts, release announcements\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
