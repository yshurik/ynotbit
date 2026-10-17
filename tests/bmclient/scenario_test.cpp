// Two clients, two vaults, two mailboxes, two object stores, and the fake relay between them.
// This is the mobile/headless shape the whole refactor exists for: no Qt, no node.
#include "fake_node.h"
#include <bmc/chat_format.h>
#include <bmc/client.h>
#include <bmc/clock.h>
#include <bmc/error.h>
#include <bmc/mailbox.h>
#include <bmc/vault.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
// The test's own clock. The mailbox, the delivery engine and the protocol all read bmc::now(), so
// a wall clock would tie every expiry to how long a proof of work happens to take. The test's
// clock moves only when the test says so, a second a round: still far past real time within one
// test, but under a chat's 24-hour expiry however many rounds a proof of work needs.
int64_t gNow = 1'700'000'000;
int64_t testNow() {
    return gNow;
}

// One end of the conversation: a vault with one identity, its mailbox, and a client over its own
// node store. The two cores are shared with the client the way Session shares them.
struct End {
    std::string root, identity, vaultPath, mailPath, key;
    std::shared_ptr<bmc::Vault> vault = std::make_shared<bmc::Vault>();
    std::shared_ptr<bmc::Mailbox> mailbox = std::make_shared<bmc::Mailbox>();
    std::unique_ptr<bmc::Client> client;

    End(const std::string &name, const char *label) : root(bmc::test::tempDir() + "/" + name) {
        std::filesystem::create_directories(bmc::path(root));
        vaultPath = root + "/v.bmvault";
        mailPath = root + "/m.bmmail";
        vault->create(vaultPath, "slice3-scenario");
        key = vault->addMailboxKey();
        mailbox->create(mailPath, key, vault->mailboxKey(key));
        identity = vault->addIdentity(label);
        client = std::make_unique<bmc::Client>(vault, mailbox, root);
        client->bindMailboxToCache();
    }
};

// The recipient's public half, kept by the sender as a peer's key is: the sender then skips the
// getpubkey round trip. The first test is the one that covers that round trip; this keeps the
// others from paying for its 28-day proof of work again.
void keepKeyOf(End &sender, const End &recipient) {
    const auto p = bmc::Wire::publicIdentity(bmc::IdentityRef(recipient.vault->identities()[0]));
    sender.mailbox->savePublicKey({p.address, p.signingKey, p.encryptionKey, p.nonceTrials,
                                   p.extraBytes, p.behaviors, testNow() + 28 * 86400});
}

// One round: each side reads its store and ticks, then the relay offers what each spooled to the
// other side. A receipt is therefore read a round before the answer it caused comes back.
void round(End &a, End &b) {
    const auto now = testNow();
    a.client->step(now, true);
    b.client->step(now, true);
    fake::deliver(a.root, b.root, now);
    fake::deliver(b.root, a.root, now);
}

// Rounds until `done` or the deadline: the proof of work runs on its own threads, so the wall
// clock is what a round can do for it. Bounded, with the caller checking the end state: a
// regression fails instead of spinning.
bool pump(End &a, End &b, const std::function<bool()> &done, int ms = 90000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        round(a, b);
        gNow += 1;
        if (!done())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done();
}
} // namespace

TEST(a_letter_crosses_between_two_clients) {
    bmc::setClockForTesting(testNow);
    End a("alice", "Alice"), b("bob", "Bob");
    const auto id = a.mailbox->saveDraft({}, a.identity, b.identity, "Hello Bob", "Private body");
    a.mailbox->setSetting("draftkind:" + id, "direct");
    a.client->sendLetter(id, testNow());
    CHECK_EQ(a.mailbox->outgoing(id).state, std::string("queued"));

    // The first round asks for the recipient's key: without it the letter never leaves the outbox.
    round(a, b);
    gNow += 1;
    CHECK_EQ(a.mailbox->outgoing(id).state, std::string("awaiting_pubkey"));
    bool asked = false;
    for (const auto &j : a.mailbox->jobs())
        if (j.kind == "getpubkey" && j.owner == id)
            asked = true;
    CHECK(asked);

    CHECK(pump(a, b, [&] { return a.mailbox->outgoing(id).state == "acknowledged"; }));
    CHECK_EQ(a.mailbox->outgoing(id).state, std::string("acknowledged"));
    CHECK(!a.mailbox->outgoing(id).objectHash.empty());
    CHECK_EQ(a.mailbox->message(id).folder, std::string("Sent"));

    const auto received = b.mailbox->messages();
    CHECK_EQ(received.size(), size_t(1));
    CHECK_EQ(received[0].subject, std::string("Hello Bob"));
    CHECK_EQ(received[0].body, std::string("Private body"));
    CHECK_EQ(received[0].from, a.identity);
    CHECK_EQ(received[0].folder, std::string("Inbox"));
    bmc::setClockForTesting(nullptr);
}

TEST(a_chat_crosses_and_is_answered) {
    bmc::setClockForTesting(testNow);
    End a("alice", "Alice"), b("bob", "Bob");
    keepKeyOf(a, b);
    a.client->sendChat(a.identity, b.identity, "hello bob", testNow());

    CHECK(pump(a, b, [&] { return !b.client->unreadChats(10).empty(); }));
    const auto unread = b.client->unreadChats(10);
    CHECK_EQ(unread.size(), size_t(1));
    CHECK_EQ(unread[0].localAddress, b.identity);
    CHECK_EQ(unread[0].peerAddress, a.identity);
    CHECK_EQ(unread[0].body, std::string("hello bob"));
    CHECK(b.client->markChatMessageRead(unread[0].id));
    CHECK(!b.client->markChatMessageRead(unread[0].id));
    CHECK(b.client->unreadChats(10).empty());

    // Bob's mailbox kept Alice's key from the envelope it just read, so this needs no round trip.
    b.client->sendChat(b.identity, a.identity, "hello back", testNow());
    CHECK(pump(a, b, [&] { return !a.client->unreadChats(10).empty(); }));
    const auto arrived = a.client->unreadChats(10);
    CHECK_EQ(arrived.size(), size_t(1));
    CHECK_EQ(arrived[0].peerAddress, b.identity);
    CHECK_EQ(arrived[0].body, std::string("hello back"));
    bmc::setClockForTesting(nullptr);
}

TEST(a_broadcast_reaches_a_subscriber) {
    bmc::setClockForTesting(testNow);
    End a("alice", "Alice"), b("bob", "Bob");
    b.mailbox->subscribe(a.identity, "Alice's announcements");
    const auto id = a.mailbox->saveDraft({}, a.identity, {}, "Announcement", "Everyone");
    a.mailbox->setSetting("draftkind:" + id, "broadcast");
    a.client->sendLetter(id, testNow());

    const auto held = [&] {
        for (const auto &m : b.mailbox->messages())
            if (m.folder == "Broadcasts" && m.subject == "Announcement")
                return true;
        return false;
    };
    CHECK(pump(a, b, held));
    CHECK(held());
    CHECK_EQ(b.mailbox->messages().size(), size_t(1));
    CHECK_EQ(a.mailbox->outgoing(id).state, std::string("published"));
    bmc::setClockForTesting(nullptr);
}

TEST(a_second_scan_stores_nothing_twice) {
    bmc::setClockForTesting(testNow);
    End a("alice", "Alice"), b("bob", "Bob");
    keepKeyOf(a, b);
    const auto id = a.mailbox->saveDraft({}, a.identity, b.identity, "Hello Bob", "Body");
    a.mailbox->setSetting("draftkind:" + id, "direct");
    a.client->sendLetter(id, testNow());

    CHECK(pump(a, b, [&] { return a.mailbox->outgoing(id).state == "acknowledged"; }));
    const auto aCheckpoint = a.mailbox->checkpoint(), bCheckpoint = b.mailbox->checkpoint();
    CHECK_EQ(a.mailbox->messages().size(), size_t(1));
    CHECK_EQ(b.mailbox->messages().size(), size_t(1));

    // Nothing new is spooled, so extra rounds change neither side.
    for (int i = 0; i < 3; ++i) {
        round(a, b);
        gNow += 1;
    }
    CHECK_EQ(a.mailbox->messages().size(), size_t(1));
    CHECK_EQ(b.mailbox->messages().size(), size_t(1));
    CHECK_EQ(a.mailbox->checkpoint(), aCheckpoint);
    CHECK_EQ(b.mailbox->checkpoint(), bCheckpoint);
    bmc::setClockForTesting(nullptr);
}
