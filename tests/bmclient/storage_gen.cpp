// The Qt-free half of the compatibility gate (scripts/compat-check.sh): it writes a vault and a
// mailbox for the previous release to open, and it prints a canonical report of what it reads, for
// the previous release's report to be compared against. scripts/compat-open.cpp is the other half,
// built inside the base worktree with the Qt app's own bm::Vault and bm::Mailbox; the two reports
// must come out identical.
//
// Usage:
//   storage_gen create <dir>                                  writes <dir>/dossier.bmvault and
//                                                             <dir>/dossier.bmmail
//   storage_gen open <vault> <password> <mailbox> <key|vault> prints the report
// <key> is 64 hex characters, or the word "vault" to take the vault's own mailbox key.
#include <algorithm>
#include <bmc/chat_format.h>
#include <bmc/clock.h>
#include <bmc/mailbox.h>
#include <bmc/secret.h>
#include <bmc/vault.h>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr const char *kPassword = "slice2-compat";
constexpr const char *kPeer = "BM-2cSenderTwo";
constexpr const char *kBroadcaster = "BM-2cBroadcaster";

bmc::Bytes unhex(const std::string &text) {
    bmc::Bytes out;
    for (size_t i = 0; i + 1 < text.size(); i += 2)
        out.push_back(uint8_t(std::stoi(text.substr(i, 2), nullptr, 16)));
    return out;
}

// The fixture both sides write, step for step, with the same fixed values.
void create(const std::string &dir) {
    bmc::Vault vault;
    vault.create(dir + "/dossier.bmvault", kPassword);
    vault.addIdentity("compat");
    const auto keyId = vault.addMailboxKey();
    const auto &identity = vault.identities().front();

    bmc::Mailbox m;
    m.create(dir + "/dossier.bmmail", keyId, vault.mailboxKey(keyId));
    m.store("hash-welcome", "BM-2cSenderOne", identity.address, "Welcome", "The first letter.", -1);
    m.store("hash-second", kPeer, identity.address, "Second letter", "More text in it.", 42);
    m.markRead("hash-welcome");
    const auto draft =
        m.saveDraft({}, identity.address, "BM-2cSenderOne", "A draft", "Not sent yet.");
    m.queueDraft(draft, "direct", bmc::now() + 86400);
    m.storeChat("chat-in-1", kPeer, identity.address, 1, "hello from the peer", bmc::now() + 3600);
    m.storeRouted("hash-chat", kPeer, identity.address, {}, bmc::chatMessageBody(2, "routed chat"),
                  43, "Inbox", bmc::now() + 3600);
    m.subscribe(kBroadcaster, "Release notes");
    m.saveContact("BM-2cSenderOne", "Ada");
    bmc::StoredPublicKey key;
    key.address = kPeer;
    key.signingKey = bmc::Bytes(65, 0x02);
    key.encryptionKey = bmc::Bytes(65, 0x03);
    key.expires = 2000000000;
    m.savePublicKey(key);
    m.setSetting("theme", "light");
    m.close();
}

void reportVault(std::ostream &out, bmc::Vault &vault) {
    out << "vault identities=" << vault.identities().size() << "\n";
    out << "vault address=" << vault.identities().front().address << "\n";
    auto ids = vault.mailboxIds();
    std::sort(ids.begin(), ids.end());
    out << "vault mailbox-keys=" << ids.size() << "\n";
    for (const auto &id : ids)
        out << "  key " << id << "\n";
}

void reportMailbox(std::ostream &out, bmc::Mailbox &m) {
    out << "mailbox key-id=" << m.keyId() << " checkpoint=" << m.checkpoint() << "\n";
    auto letters = m.messages();
    std::sort(letters.begin(), letters.end(),
              [](const bmc::Message &a, const bmc::Message &b) { return a.hash < b.hash; });
    out << "letters=" << letters.size() << "\n";
    for (const auto &letter : letters)
        out << "  letter " << letter.hash << " " << letter.folder
            << " unread=" << m.unread(letter.hash) << " from=" << letter.from << " to=" << letter.to
            << " subject=" << letter.subject << "\n";
    for (const char *folder : {"Inbox", "Drafts", "Outbox", "Sent"})
        out << "count " << folder << "=" << m.messageCount(folder, "") << "\n";
    auto boxes = m.outbox();
    std::sort(boxes.begin(), boxes.end(),
              [](const bmc::OutboxItem &a, const bmc::OutboxItem &b) { return a.id < b.id; });
    out << "outbox=" << boxes.size() << "\n";
    for (const auto &o : boxes)
        out << "  box " << o.id << " kind=" << o.kind << " state=" << o.state << " chat=" << o.chat
            << " expires=" << (o.expires > 0) << "\n";
    auto chats = m.chatSummaries();
    std::sort(chats.begin(), chats.end(),
              [](const bmc::ChatSummary &a, const bmc::ChatSummary &b) { return a.peer < b.peer; });
    out << "chats=" << chats.size() << " unread=" << m.chatUnread() << "\n";
    for (const auto &c : chats)
        out << "  chat " << c.peer << " state=" << c.state << " outgoing=" << c.outgoing
            << " unread=" << c.unread << " body=" << c.body << "\n";
    for (const auto &u : m.unreadChats(10))
        out << "  unread-chat " << u.id << " " << u.localAddress << "->" << u.peerAddress << " "
            << u.body << "\n";
    for (const auto &s : m.subscriptions())
        out << "subscription " << s.address << " " << s.label << "\n";
    for (const auto &c : m.contacts())
        out << "contact " << c.address << " " << c.label << "\n";
    if (const auto key = m.publicKey(kPeer, 1700000000))
        out << "public-key " << key->address << " signing=" << key->signingKey.size()
            << " encryption=" << key->encryptionKey.size() << "\n";
    out << "setting theme=" << m.setting("theme") << "\n";
}

int open(const std::string &vaultPath, const std::string &password, const std::string &mailboxPath,
         const std::string &key) {
    bmc::Vault v;
    v.unlock(vaultPath, password);
    reportVault(std::cout, v);
    const auto ids = v.mailboxIds();
    // A Secret never copies; bind whichever one applies.
    bmc::Secret fromHex;
    const bmc::Secret *material = nullptr;
    if (key == "vault") {
        if (ids.empty())
            throw std::runtime_error("the vault holds no mailbox key");
        material = &v.mailboxKey(ids.front());
    } else {
        const auto bytes = unhex(key);
        fromHex = bmc::Secret(bytes.size());
        std::copy(bytes.begin(), bytes.end(), fromHex.data());
        material = &fromHex;
    }
    bmc::Mailbox m;
    m.open(mailboxPath, *material);
    reportMailbox(std::cout, m);
    m.close();
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "create") {
        create(argv[2]);
        return 0;
    }
    if (argc == 6 && std::string(argv[1]) == "open")
        return open(argv[2], argv[3], argv[4], argv[5]);
    std::fprintf(stderr,
                 "usage: %s create <dir>\n       %s open <vault> <password> <mailbox> "
                 "<key|vault>\n",
                 argv[0], argv[0]);
    return 2;
}
