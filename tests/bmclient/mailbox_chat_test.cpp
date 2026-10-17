#include "storage_fixtures.h"
#include <bmc/chat_format.h>
#include <bmc/clock.h>
#include <bmc/mailbox.h>
#include <bmc/test.h>

using bmc::Mailbox;

namespace {
bmc::Secret key32(uint8_t fill = 0x01) {
    bmc::Secret key(32);
    for (int i = 0; i < 32; ++i)
        key.data()[i] = fill;
    return key;
}
int64_t soon() {
    return bmc::now() + 3600;
}
} // namespace

TEST(a_queued_chat_gets_a_millisecond_number_that_keeps_growing) {
    const auto path = bmc::test::tempDir() + "/chats.bmmail";
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    const auto first = m.queueChat("BM-local", "BM-peer", "one", soon());
    const auto second = m.queueChat("BM-local", "BM-peer", "two", soon());
    auto thread = m.chatMessages("BM-local", "BM-peer");
    CHECK_EQ(thread.size(), size_t(2));
    CHECK_EQ(thread.front().id, first); // oldest first
    CHECK(thread[1].seqno > thread[0].seqno);
    CHECK(thread[1].seqno > 1'000'000'000'000LL); // milliseconds, not a count
    CHECK_EQ(thread.front().state, std::string("queued"));
    CHECK(thread.front().outgoing);
    m.close();

    // A restored mailbox has lost its counters: the next number is still past the last one sent.
    const auto lastSent = thread[1].seqno;
    Mailbox reopened;
    reopened.open(path, key);
    reopened.queueChat("BM-local", "BM-peer", "three", soon());
    const auto grown = reopened.chatMessages("BM-local", "BM-peer");
    CHECK_EQ(grown.size(), size_t(3));
    CHECK(grown.back().seqno > lastSent);
    reopened.close();
}

TEST(chats_that_break_the_rules_are_refused_or_dropped) {
    const auto path = bmc::test::tempDir() + "/chat-rules.bmmail";
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);

    CHECK_THROWS(m.queueChat("", "BM-peer", "hi", soon()));                 // no sender
    CHECK_THROWS(m.queueChat("BM-local", "", "hi", soon()));                // no peer
    CHECK_THROWS(m.queueChat("BM-local", "BM-peer", "   ", soon()));        // empty body
    CHECK_THROWS(m.queueChat("BM-local", "BM-peer", std::string(8193, 'x'), // too long
                             soon()));
    CHECK_THROWS(m.queueChat("BM-local", "BM-peer", "hi", bmc::now() - 1)); // expiry in the past
    CHECK_THROWS(m.queueChat("BM-local", "BM-peer", "hi",                   // expiry too far
                             bmc::now() + bmc::kChatTtlSeconds + 1));
    CHECK_EQ(m.chatMessages("BM-local", "BM-peer").size(), size_t(0));

    // storeChat drops the same cases silently, and stores what passes.
    m.storeChat("chat-a", "BM-peer", "BM-local", 1, "   ", soon());
    m.storeChat("chat-b", "BM-peer", "BM-local", 1, std::string(8193, 'x'), soon());
    m.storeChat("chat-c", "BM-peer", "BM-local", 0, "hi", soon()); // seqno 0
    m.storeChat("chat-d", "BM-peer", "BM-local", 1, "hi", bmc::now() - 1);
    CHECK_EQ(m.chatMessages("BM-local", "BM-peer").size(), size_t(0));
    m.storeChat("chat-e", "BM-peer", "BM-local", 5, "incoming", soon());
    CHECK_EQ(m.chatMessages("BM-local", "BM-peer").size(), size_t(1));
    // A replay of the same sender sequence is ignored.
    m.storeChat("chat-f", "BM-peer", "BM-local", 5, "replay", soon());
    CHECK_EQ(m.chatMessages("BM-local", "BM-peer").size(), size_t(1));
    m.close();
}

TEST(store_routed_sends_a_chat_to_the_chats_and_a_letter_to_the_inbox) {
    const auto path = bmc::test::tempDir() + "/chat-routed.bmmail";
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    const auto expiry = soon();
    // A chat is routed away from the letters; the checkpoint still advances.
    m.storeRouted("hash-chat", "BM-peer", "BM-local", {}, bmc::chatMessageBody(2, "routed"), 4,
                  "Inbox", expiry);
    CHECK_EQ(m.messageCount("Inbox", ""), 0);
    CHECK_EQ(m.chatMessages("BM-local", "BM-peer").size(), size_t(1));
    CHECK_EQ(m.checkpoint(), int64_t(4));
    // Ours but broken: still not a letter, and the checkpoint advances.
    m.storeRouted("hash-invalid", "BM-peer", "BM-local", {},
                  "{\"app\":\"ynotbit\",\"type\":\"chat\"}", 5, "Inbox", expiry);
    CHECK_EQ(m.messageCount("Inbox", ""), 0);
    CHECK_EQ(m.checkpoint(), int64_t(5));
    // An ordinary letter stays a letter, behind its checkpoint.
    m.storeRouted("hash-letter", "BM-peer", "BM-local", "Subject", "Body", 6, "Inbox", expiry);
    CHECK_EQ(m.messageCount("Inbox", ""), 1);
    CHECK_EQ(m.checkpoint(), int64_t(6));
    m.close();
}

TEST(unread_chats_are_the_partial_index_in_received_order) {
    const auto path = bmc::test::tempDir() + "/chat-unread.bmmail";
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    for (int i = 1; i <= 3; ++i)
        m.storeChat("chat-in-" + std::to_string(i), "BM-peer", "BM-local", i,
                    "in " + std::to_string(i), soon());
    // A sent chat is queued for the network: it has a delivery record; a stored one does not.
    const auto outgoingId = m.queueChat("BM-local", "BM-peer", "out", soon());
    CHECK_EQ(m.chatUnread(), 3);
    CHECK_EQ(m.unreadChats(2).size(), size_t(2));
    CHECK_EQ(m.unreadChats(10).front().id, std::string("chat-in-1")); // oldest first
    CHECK_EQ(m.unreadChats(10).back().id, std::string("chat-in-3"));
    CHECK(m.markChatMessageRead("chat-in-1"));
    CHECK(!m.markChatMessageRead("chat-in-1")); // already read
    CHECK(!m.markChatMessageRead(outgoingId));  // ours, never unread
    CHECK_EQ(m.chatUnread(), 2);
    CHECK_EQ(m.markChatRead("BM-local", "BM-peer"), 2);
    CHECK_EQ(m.chatUnread(), 0);
    CHECK_EQ(m.markChatRead("BM-local", "BM-peer"), 0);

    // The conversation's newest message is what its summary shows.
    const auto summary = m.chatSummaries();
    CHECK_EQ(summary.size(), size_t(1));
    CHECK_EQ(summary.front().local, std::string("BM-local"));
    CHECK_EQ(summary.front().peer, std::string("BM-peer"));
    CHECK_EQ(summary.front().body, std::string("out"));
    CHECK(summary.front().outgoing);
    CHECK_EQ(summary.front().unread, 0);

    // A window is the newest messages, oldest first, and says whether older ones are left.
    bool more = false;
    const auto window = m.chatMessages("BM-local", "BM-peer", 2, &more);
    CHECK_EQ(window.size(), size_t(2));
    CHECK(more);
    CHECK_EQ(window.back().id, outgoingId);
    bool none = true;
    const auto whole = m.chatMessages("BM-local", "BM-peer", 0, &none);
    CHECK_EQ(whole.size(), size_t(4));
    CHECK(!none);

    // Past its expiry, a chat's network job ends; the chat and its delivery record stay.
    bmc::NetworkJob job;
    job.owner = outgoingId;
    job.kind = "pubkey";
    job.address = "BM-peer";
    job.payload = bmc::Bytes(10, 0x09);
    m.addJob(job);
    CHECK_EQ(m.jobs().size(), size_t(1));
    m.endChatDeliveries(bmc::now()); // not expired yet
    CHECK_EQ(m.jobs().size(), size_t(1));
    m.endChatDeliveries(bmc::now() + 7200);
    CHECK_EQ(m.jobs().size(), size_t(0));
    CHECK_EQ(m.chatState(outgoingId), std::string("queued"));
    CHECK_EQ(m.outbox().size(), size_t(1));
    // A finished delivery leaves the outbox list; the chat itself stays.
    m.setDelivery(outgoingId, "acknowledged");
    CHECK_EQ(m.outbox().size(), size_t(0));
    CHECK_EQ(m.chatMessages("BM-local", "BM-peer").size(), size_t(4));
    // An overnight drop is a direct message, so it cannot be retried.
    CHECK_THROWS(m.retry(outgoingId, soon()));
    m.close();
}

TEST(the_golden_mailbox_chats_of_the_previous_release_read) {
    const auto path = bmc::test::tempDir() + "/golden-chats.bmmail";
    bmc::test::writeFile(path, fixtures::goldenMailbox2());
    auto key = fixtures::goldenMailboxKey();
    Mailbox m;
    m.open(path, key);
    CHECK_EQ(m.chatUnread(), 1);
    const auto unread = m.unreadChats(10);
    CHECK_EQ(unread.size(), size_t(1));
    CHECK_EQ(unread.front().id, std::string("chat-golden-in"));
    CHECK_EQ(unread.front().localAddress, std::string("BM-2cRecipientOne"));
    CHECK_EQ(unread.front().peerAddress, std::string("BM-2cSenderTwo"));
    CHECK_EQ(unread.front().state, std::string("received"));
    const auto summary = m.chatSummaries();
    CHECK_EQ(summary.size(), size_t(1));
    CHECK_EQ(summary.front().peer, std::string("BM-2cSenderTwo"));
    CHECK_EQ(summary.front().unread, 1);
    // The queued chat is still in the outbox: nothing has published it.
    const auto boxes = m.outbox();
    CHECK_EQ(boxes.size(), size_t(1));
    CHECK(boxes.front().chat);
    CHECK_EQ(boxes.front().state, std::string("queued"));
    CHECK_EQ(boxes.front().message.folder, std::string("Chats"));
    CHECK_EQ(m.outgoing("chat-golden-out").id, std::string("chat-golden-out"));
    CHECK_EQ(m.chatState("chat-golden-out"), std::string("queued"));
    // The chat has no delivery-event row: those belong to letters.
    CHECK_EQ(m.events("chat-golden-out").size(), size_t(0));
    m.close();
}
