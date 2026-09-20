#include "delivery.h"
#include "protocol.h"
#include "protocol_wire.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
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
static void cacheObject(const QString &root, const QByteArray &bytes) {
    QDir().mkpath(root + "/objects");
    write(root + "/objects/" + Protocol::inventoryHash(bytes), bytes);
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
        ac.discover();
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
            bc.discover();
            if (!bv.unlocked()) {
                for (const auto &cached : bc.after(0, 100)) {
                    QFile f(cached.path);
                    require(f.open(QIODevice::ReadOnly), "locked retained object");
                    auto raw = f.readAll();
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
            ac.discover();
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
        cacheObject(b, broadcast);
        // discover() is a bounded, incremental scan (like Session::tick() calls it in
        // production): one call isn't guaranteed to finish, especially under slow I/O.
        bool found = false;
        const auto broadcastDeadline = QDateTime::currentMSecsSinceEpoch() + 5000;
        while (!found && QDateTime::currentMSecsSinceEpoch() < broadcastDeadline) {
            bc.discover();
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
            bc.discover();
            bd.scan(bc, bm, bv, 100);
            for (const auto &m : bm.messages())
                if (m.folder == "Channels" && m.subject == "Chan announcement")
                    chanFound = true;
            if (!chanFound)
                QThread::msleep(5);
        }
        require(chanFound, "chan member decodes an Anonymous-mode broadcast without subscribing");
        ad.stop();
        bd.stop();
        std::cout << "PASS: key lookup, real PoW, lock/reopen, encrypted handoff, receive, ACK, "
                     "rescan, reply, cancel/retry, broadcasts, chan broadcasts\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
