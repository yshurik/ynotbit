// bmc::Delivery without Qt: the outbox state machine, the spool and the receipts the relay writes,
// and the scan that reads the node's store. The fixtures are the same as tests/delivery_tests.cpp,
// which pins the same behaviour through the Qt veneer.
#include "fake_node.h"
#include <bmc/cache.h>
#include <bmc/chat_format.h>
#include <bmc/clock.h>
#include <bmc/delivery.h>
#include <bmc/mailbox.h>
#include <bmc/secret.h>
#include <bmc/test.h>
#include <bmc/updates.h>
#include <bmc/vault.h>
#include <bmc/wire.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <ntb-object-db.h>
#include <thread>
#include <vector>

namespace {
// One simulated instant for the whole test: the proof of work needs real time, the protocol does
// not need the clock to move. It is anchored on the real clock, because the mailbox's own expiry
// checks read bmc::now().
int64_t kNow() {
    static const int64_t value = bmc::now();
    return value;
}

// One end of the conversation: a vault with one identity, its mailbox, the node's object store and
// the delivery engine over them.
struct Side {
    std::string root, identity, vaultPath, mailPath, key;
    bmc::Vault vault;
    bmc::Mailbox mailbox;
    std::unique_ptr<bmc::Cache> cache;
    std::unique_ptr<bmc::Delivery> delivery;

    Side(const std::string &name, const char *label) : root(bmc::test::tempDir() + "/" + name) {
        std::filesystem::create_directories(bmc::path(root));
        vaultPath = root + "/v.bmvault";
        mailPath = root + "/m.bmmail";
        vault.create(vaultPath, "slice3-delivery");
        key = vault.addMailboxKey();
        mailbox.create(mailPath, key, vault.mailboxKey(key));
        identity = vault.addIdentity(label);
        cache = std::make_unique<bmc::Cache>(root);
        delivery = std::make_unique<bmc::Delivery>(root);
        mailbox.bindCache(cache->id());
    }
};

// Turns both ends until `done`; the proof of work runs on its own threads, so the wall clock is the
// only deadline.
bool pump(Side &a, Side &b, const std::function<bool()> &done, int ms = 60000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        // A reads the receipts the relay wrote and spools what is ready.
        a.delivery->tick(a.mailbox, a.vault, true, kNow());
        fake::deliver(a.root, b.root, kNow());
        // B reads what arrived, then answers.
        b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
        b.delivery->tick(b.mailbox, b.vault, true, kNow());
        // A reads B's objects from the round before, so A's own receipt is always processed
        // before B's acknowledgment reaches it.
        a.delivery->scan(*a.cache, a.mailbox, a.vault, kNow());
        fake::deliver(b.root, a.root, kNow());
        if (!done())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done();
}
} // namespace

TEST(an_unknown_recipient_asks_for_its_key) {
    Side a("alice", "Alice"), b("bob", "Bob");
    const auto id = a.mailbox.saveDraft({}, a.identity, b.identity, "Hello Bob", "Body");
    a.mailbox.queueDraft(id, "direct", kNow() + 3600);

    a.delivery->tick(a.mailbox, a.vault, false, kNow());
    CHECK_EQ(a.mailbox.outgoing(id).state, std::string("awaiting_pubkey"));
    const auto jobs = a.mailbox.jobs();
    CHECK_EQ(jobs.size(), size_t(1));
    CHECK_EQ(jobs[0].kind, std::string("getpubkey"));
    CHECK(bmc::Wire::requestsIdentity(jobs[0].payload, b.vault.identities()[0]));
    a.delivery->stop();
}

TEST(a_broadcast_is_published_without_a_recipient_acknowledgment) {
    Side a("alice", "Alice"), b("bob", "Bob");
    const auto id = a.mailbox.saveDraft({}, a.identity, b.identity, "Announcement", "Everyone");
    a.mailbox.queueDraft(id, "broadcast", kNow() + 3600);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (a.mailbox.outgoing(id).state != "published" &&
           std::chrono::steady_clock::now() < deadline) {
        a.delivery->tick(a.mailbox, a.vault, true, kNow());
        fake::deliver(a.root, b.root, kNow());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // The relay took the object: no recipient receipt is expected, so the delivery is finished.
    CHECK_EQ(a.mailbox.outgoing(id).state, std::string("published"));
    CHECK(!a.mailbox.outgoing(id).objectHash.empty());
    CHECK_EQ(a.mailbox.message(id).folder, std::string("Sent"));
}

TEST(a_letter_reaches_the_other_side_and_its_acknowledgment_comes_back) {
    Side a("alice", "Alice"), b("bob", "Bob");
    const auto id = a.mailbox.saveDraft({}, a.identity, b.identity, "Hello Bob", "Private body");
    a.mailbox.queueDraft(id, "direct", kNow() + 3600);
    // The key round trip is what the first rounds are for: without it the letter never leaves.
    a.delivery->tick(a.mailbox, a.vault, false, kNow());
    CHECK_EQ(a.mailbox.outgoing(id).state, std::string("awaiting_pubkey"));

    CHECK(pump(a, b, [&] { return a.mailbox.outgoing(id).state == "acknowledged"; }));
    CHECK_EQ(a.mailbox.message(id).folder, std::string("Sent"));
    CHECK(!a.mailbox.outgoing(id).objectHash.empty());

    const auto received = b.mailbox.messages();
    CHECK_EQ(received.size(), size_t(1));
    CHECK_EQ(received[0].body, std::string("Private body"));
    CHECK_EQ(received[0].subject, std::string("Hello Bob"));
    CHECK_EQ(received[0].from, a.identity);
    CHECK_EQ(received[0].folder, std::string("Inbox"));

    // A rescan over the same objects stores nothing twice.
    b.mailbox.advance(0);
    b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    CHECK_EQ(b.mailbox.messages().size(), size_t(1));
}

TEST(a_cancelled_letter_loses_its_jobs_and_a_retry_queues_it_again) {
    Side a("alice", "Alice"), b("bob", "Bob");
    const auto id = a.mailbox.saveDraft({}, a.identity, b.identity, "Hello Bob", "Body");
    a.mailbox.queueDraft(id, "direct", kNow() + 3600);
    a.delivery->tick(a.mailbox, a.vault, false, kNow());
    CHECK_EQ(a.mailbox.jobs().size(), size_t(1));

    a.delivery->cancel(a.mailbox, id);
    CHECK_EQ(a.mailbox.outgoing(id).state, std::string("cancelled"));
    const auto jobs = a.mailbox.jobs();
    CHECK(jobs.empty() || jobs[0].owner != id);

    a.delivery->retry(a.mailbox, id);
    CHECK_EQ(a.mailbox.outgoing(id).state, std::string("queued"));
    CHECK_EQ(a.mailbox.checkpoint(), int64_t(0));
}

TEST(a_subscribed_broadcast_is_stored_after_an_oversized_object) {
    Side a("alice", "Alice"), b("bob", "Bob");
    b.mailbox.subscribe(a.identity, "Alice's announcements");
    // A peer's object larger than the protocol allows: skipped, and the broadcast stored after it
    // is still read.
    fake::cacheObject(b.root, std::vector<uint8_t>(size_t(NTB_OBJECT_DB_MAX_OBJECT_SIZE) + 1, 'z'));
    fake::cacheObject(b.root, bmc::Wire::encodeBroadcast(a.vault.identities()[0], "Announcement",
                                                         "Subscriber only", kNow() + 3600));

    b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    bool found = false;
    for (const auto &m : b.mailbox.messages())
        if (m.folder == "Broadcasts" && m.subject == "Announcement")
            found = true;
    CHECK(found);
    CHECK_EQ(b.mailbox.checkpoint(), int64_t(2));
}

TEST(a_broadcast_already_passed_is_found_by_the_catch_up) {
    Side a("alice", "Alice"), b("bob", "Bob");
    fake::cacheObject(b.root,
                      bmc::Wire::encodeBroadcast(a.vault.identities()[0], "Late announcement",
                                                 "Subscriber only", kNow() + 3600));
    // Without the subscription the scan reads the object and passes over it.
    b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    CHECK_EQ(b.mailbox.checkpoint(), int64_t(1));
    CHECK_EQ(b.mailbox.messages().size(), size_t(0));

    // Subscribing reads the kept objects again, without moving the checkpoint.
    b.mailbox.subscribe(a.identity, "Alice's announcements");
    b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    const auto messages = b.mailbox.messages();
    CHECK_EQ(messages.size(), size_t(1));
    CHECK_EQ(messages[0].subject, std::string("Late announcement"));
    CHECK_EQ(messages[0].folder, std::string("Broadcasts"));
    CHECK_EQ(b.mailbox.checkpoint(), int64_t(1));
}

TEST(a_message_to_a_service_address_is_filed_apart_and_counts_nowhere) {
    Side a("alice", "Alice"), b("bob", "Bob");
    // Two chans both sides have joined. To Bob, one of them is a service's: whatever comes to it is
    // not mail.
    const std::string service = a.vault.addChannel("the service chan of the delivery test", "", "");
    const std::string ordinary =
        a.vault.addChannel("an ordinary chan of the delivery test", "", "");
    CHECK_EQ(b.vault.addChannel("the service chan of the delivery test", "", ""), service);
    CHECK_EQ(b.vault.addChannel("an ordinary chan of the delivery test", "", ""), ordinary);
    b.delivery->setServiceAddresses({service});

    for (const auto &chan : {service, ordinary}) {
        const auto id =
            a.mailbox.saveDraft({}, a.identity, chan, "To the chan", "Everybody reads " + chan);
        a.mailbox.queueDraft(id, "direct", kNow() + 3600);
    }
    CHECK(pump(a, b, [&] { return b.mailbox.messages().size() == 2; }));

    std::map<std::string, std::string> folderOf; // by recipient
    for (const auto &m : b.mailbox.messages())
        folderOf[m.to] = m.folder;
    CHECK_EQ(folderOf[service], std::string("Service"));
    CHECK_EQ(folderOf[ordinary], std::string("Channels"));
    // The service chan is nowhere a person looks: not among the chans, not in the unread counts.
    CHECK_EQ(b.mailbox.channelAddresses("Channels"), std::vector<std::string>{ordinary});
    const auto unread = b.mailbox.unreadByFolder();
    CHECK_EQ(unread.size(), size_t(1));
    CHECK_EQ(unread.count("Channels"), size_t(1));
    CHECK_EQ(b.mailbox.unreadLetters(), 1);
    CHECK_EQ(b.mailbox.incomingLetterCount(), uint64_t(0));

    // A rescan stores nothing twice, with the setting or without it: the object is stored once, and
    // where it was first filed it stays.
    b.mailbox.advance(0);
    b.delivery->setServiceAddresses({});
    b.delivery->scan(*b.cache, b.mailbox, b.vault, kNow());
    CHECK_EQ(b.mailbox.messages().size(), size_t(2));
}

TEST(the_service_folder_is_taken_out_oldest_first_and_in_one_go) {
    Side b("bob", "Bob");
    for (int i = 0; i < 5; ++i)
        b.mailbox.store("service-" + std::to_string(i), "BM-sender", "BM-service", "",
                        "body " + std::to_string(i), -1, "Service");
    b.mailbox.store("mail", "BM-sender", b.identity, "hello", "mail", -1, "Inbox");
    const auto first = b.mailbox.takeService(3);
    CHECK_EQ(first.size(), size_t(3));
    CHECK(first[0].body == "body 0" && first[1].body == "body 1" && first[2].body == "body 2");
    CHECK_EQ(first[0].folder, std::string("Service"));
    const auto rest = b.mailbox.takeService(10);
    CHECK_EQ(rest.size(), size_t(2));
    CHECK_EQ(rest[1].body, std::string("body 4"));
    CHECK(b.mailbox.takeService(10).empty());
    // What was taken is gone for good, and nothing else was touched.
    CHECK_EQ(b.mailbox.messages().size(), size_t(1));
    CHECK_EQ(b.mailbox.messages()[0].folder, std::string("Inbox"));
    CHECK(b.mailbox.takeService(0).empty());
}

// The whole way of a lookup, as the app and a service make it: the app sends a chat from its
// service chan, the service (which has no key of that chan) answers to the chan's address, and what
// comes back is filed apart, not shown as a post of the chan.
TEST(a_chat_from_a_service_chan_is_answered_to_the_chan_and_the_answer_is_filed_apart) {
    Side app("app", "App"), service("service", "Service");
    const std::string chan =
        app.vault.addChannel("the chan of the lookups of the delivery test", "", "");
    app.delivery->setServiceAddresses({chan});
    app.mailbox.queueChat(chan, service.identity, "/address @ben", kNow() + 3600);
    // The service reads the question as a chat from the chan's address.
    CHECK(pump(app, service, [&] { return !service.mailbox.unreadChats(10).empty(); }));
    const auto chats = service.mailbox.unreadChats(10);
    CHECK_EQ(chats.size(), size_t(1));
    CHECK_EQ(chats[0].peerAddress, chan);
    CHECK_EQ(chats[0].localAddress, service.identity);
    CHECK_EQ(chats[0].body, std::string("/address @ben"));
    service.mailbox.markChatMessageRead(chats[0].id);
    // It answers to the address it was written from.
    service.mailbox.queueChat(service.identity, chan, "@ben is nobody.\nnickbot: {\"v\":1}",
                              kNow() + 3600);
    CHECK(pump(app, service, [&] { return !app.mailbox.messages().empty(); }));
    const auto messages = app.mailbox.messages();
    CHECK_EQ(messages.size(), size_t(1));
    CHECK_EQ(messages[0].folder, std::string("Service"));
    CHECK_EQ(messages[0].from, service.identity);
    CHECK_EQ(messages[0].to, chan);
    CHECK(app.mailbox.channelAddresses("Channels").empty());
    CHECK(app.mailbox.unreadByFolder().empty());
    // What the app does with it: the chat inside is the answer, and the folder is emptied.
    const auto taken = app.mailbox.takeService(10);
    CHECK_EQ(taken.size(), size_t(1));
    bmc::ChatPayload payload;
    CHECK(bmc::parseChatMessage(taken[0].subject, taken[0].body, &payload) ==
          bmc::ChatParseResult::Chat);
    CHECK_EQ(payload.body, std::string("@ben is nobody.\nnickbot: {\"v\":1}"));
    CHECK(app.mailbox.messages().empty());
}
