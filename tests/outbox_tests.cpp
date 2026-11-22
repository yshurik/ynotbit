// The Qt face of the mailbox's delivery side (src/mailbox_ops.cpp): a draft through its queue, its
// acknowledgment and its cancel and retry, once, with the QString and QByteArray they take and give.
// What the mailbox keeps, refuses and migrates is tests/bmclient/mailbox_*_test.cpp's.
#include "storage.h"
#include <QCoreApplication>
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
        m.close();
        std::cout << "PASS: editable drafts, durable outbox/ack, cancel/retry\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
