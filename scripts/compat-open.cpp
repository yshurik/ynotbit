// The Qt half of the compatibility gate (scripts/compat-check.sh). The script copies this file into
// a worktree of the pre-slice commit, adds a CMake target for it beside bmcore's tests, builds it,
// and runs it against the same files the Qt-free tests/bmclient/storage_gen.cpp writes -- and the
// other way round. The two reports must come out identical: that is the previous release opening
// what this refactor wrote, and this refactor opening what the previous release wrote.
//
// Usage, the same as the core's:
//   compat_open create <dir>
//   compat_open open <vault> <password> <mailbox> <key|vault>
#include "chat_format.h"
#include "storage.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QVector>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>

using namespace bm;

namespace {
const char *kPassword = "slice2-compat";
const char *kPeer = "BM-2cSenderTwo";
const char *kBroadcaster = "BM-2cBroadcaster";

Secret unhex(const QString &text) {
    const auto bytes = QByteArray::fromHex(text.toLatin1());
    Secret out(size_t(bytes.size()));
    std::copy(bytes.begin(), bytes.end(), out.data());
    return out;
}

// The fixture both sides write, step for step, with the same fixed values.
void create(const QString &dir) {
    Vault vault;
    vault.create(dir + "/dossier.bmvault", kPassword);
    vault.addIdentity("compat");
    const auto keyId = vault.addMailboxKey();
    const auto identity = vault.identities().front().address;

    Mailbox m;
    m.create(dir + "/dossier.bmmail", keyId, vault.mailboxKey(keyId));
    m.store("hash-welcome", "BM-2cSenderOne", identity, "Welcome", "The first letter.", -1);
    m.store("hash-second", kPeer, identity, "Second letter", "More text in it.", 42);
    m.markRead("hash-welcome");
    const auto draft = m.saveDraft({}, identity, "BM-2cSenderOne", "A draft", "Not sent yet.");
    m.queueDraft(draft, "direct", QDateTime::currentSecsSinceEpoch() + 86400);
    const auto expiry = QDateTime::currentSecsSinceEpoch() + 3600;
    m.storeChat("chat-in-1", kPeer, identity, 1, "hello from the peer", expiry);
    m.storeRouted("hash-chat", kPeer, identity, {}, chatMessageBody(2, "routed chat"), 43, "Inbox",
                  expiry);
    m.subscribe(kBroadcaster, "Release notes");
    m.saveContact("BM-2cSenderOne", "Ada");
    StoredPublicKey key;
    key.address = kPeer;
    key.signingKey = QByteArray(65, char(0x02));
    key.encryptionKey = QByteArray(65, char(0x03));
    key.expires = 2000000000;
    m.savePublicKey(key);
    m.setSetting("theme", "light");
    m.close();
}

void reportVault(std::ostream &out, Vault &vault) {
    out << "vault identities=" << vault.identities().size() << "\n";
    out << "vault address=" << vault.identities().front().address.toStdString() << "\n";
    auto ids = vault.mailboxIds();
    std::sort(ids.begin(), ids.end());
    out << "vault mailbox-keys=" << ids.size() << "\n";
    for (const auto &id : ids)
        out << "  key " << id.toStdString() << "\n";
}

void reportMailbox(std::ostream &out, Mailbox &m) {
    out << "mailbox key-id=" << m.keyId().toStdString() << " checkpoint=" << m.checkpoint() << "\n";
    auto letters = m.messages();
    std::sort(letters.begin(), letters.end(),
              [](const Message &a, const Message &b) { return a.hash < b.hash; });
    out << "letters=" << letters.size() << "\n";
    for (const auto &letter : letters)
        out << "  letter " << letter.hash.toStdString() << " " << letter.folder.toStdString()
            << " unread=" << m.unread(letter.hash) << " from=" << letter.from.toStdString()
            << " to=" << letter.to.toStdString() << " subject=" << letter.subject.toStdString()
            << "\n";
    for (const char *folder : {"Inbox", "Drafts", "Outbox", "Sent"})
        out << "count " << folder << "=" << m.messageCount(folder, {}) << "\n";
    auto boxes = m.outbox();
    std::sort(boxes.begin(), boxes.end(),
              [](const OutboxItem &a, const OutboxItem &b) { return a.id < b.id; });
    out << "outbox=" << boxes.size() << "\n";
    for (const auto &o : boxes)
        out << "  box " << o.id.toStdString() << " kind=" << o.kind.toStdString()
            << " state=" << o.state.toStdString() << " chat=" << o.chat
            << " expires=" << (o.expires > 0) << "\n";
    auto chats = m.chatSummaries();
    std::sort(chats.begin(), chats.end(),
              [](const ChatSummary &a, const ChatSummary &b) { return a.peer < b.peer; });
    out << "chats=" << chats.size() << " unread=" << m.chatUnread() << "\n";
    for (const auto &c : chats)
        out << "  chat " << c.peer.toStdString() << " state=" << c.state.toStdString()
            << " outgoing=" << c.outgoing << " unread=" << c.unread
            << " body=" << c.body.toStdString() << "\n";
    for (const auto &u : m.unreadChats(10))
        out << "  unread-chat " << u.id.toStdString() << " " << u.localAddress.toStdString() << "->"
            << u.peerAddress.toStdString() << " " << u.body.toStdString() << "\n";
    for (const auto &s : m.subscriptions())
        out << "subscription " << s.address.toStdString() << " " << s.label.toStdString() << "\n";
    for (const auto &c : m.contacts())
        out << "contact " << c.address.toStdString() << " " << c.label.toStdString() << "\n";
    if (const auto key = m.publicKey(kPeer, 1700000000))
        out << "public-key " << key->address.toStdString() << " signing=" << key->signingKey.size()
            << " encryption=" << key->encryptionKey.size() << "\n";
    out << "setting theme=" << m.setting("theme").toStdString() << "\n";
}

int open(const QString &vaultPath, const QString &password, const QString &mailboxPath,
         const QString &key) {
    Vault v;
    v.unlock(vaultPath, password.toUtf8());
    reportVault(std::cout, v);
    const auto ids = v.mailboxIds();
    Secret fromHex;
    const Secret *material = nullptr;
    if (key == "vault") {
        if (ids.isEmpty())
            throw std::runtime_error("the vault holds no mailbox key");
        material = &v.mailboxKey(ids.front());
    } else {
        fromHex = unhex(key);
        material = &fromHex;
    }
    Mailbox m;
    m.open(mailboxPath, *material);
    reportMailbox(std::cout, m);
    m.close();
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && std::string(argv[1]) == "create") {
        create(argv[2]);
        return 0;
    }
    if (argc == 6 && std::string(argv[1]) == "open")
        return open(argv[2], argv[3], argv[4], argv[5]);
    std::fprintf(
        stderr, "usage: %s create <dir>\n       %s open <vault> <password> <mailbox> <key|vault>\n",
        argv[0], argv[0]);
    return 2;
}
