// The mailbox's delivery side: editing a draft, cancelling what the network is still working on,
// the milestones a delivery records (letters only), and the acknowledgment token of a chat. The
// delete-before-Trash rule and the chat's own acknowledgment path live here too.
#include "storage_fixtures.h"
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
std::string aNewMailbox(const char *name = "d.bmmail") {
    return bmc::test::tempDir() + "/" + name;
}
int64_t soon() {
    return bmc::now() + 3600;
}
} // namespace

TEST(a_draft_is_rewritten_and_a_queued_one_is_not) {
    const auto path = aNewMailbox();
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    const auto id = m.saveDraft({}, "BM-2cA", "BM-2cB", "First subject", "First body");
    CHECK_EQ(m.saveDraft(id, "BM-2cA", "BM-2cC", "Second subject", "Second body"), id);
    CHECK_EQ(m.message(id).subject, std::string("Second subject"));
    CHECK_EQ(m.message(id).to, std::string("BM-2cC"));
    CHECK_EQ(m.messageCount("Drafts", ""), 1); // rewritten, not a second letter
    // A letter that is not a draft cannot be edited, and neither can one whose delivery has begun.
    m.store("hash-x", "BM-2cA", "BM-2cB", "Letter", "Body", -1);
    CHECK_THROWS(m.saveDraft("hash-x", "BM-2cA", "BM-2cB", "Edit", "Body"));
    const auto queued = m.saveDraft({}, "BM-2cA", "BM-2cB", "Queued", "Body");
    m.queueDraft(queued, "direct", soon());
    CHECK_THROWS(m.saveDraft(queued, "BM-2cA", "BM-2cB", "Edit", "Body"));
    // And what is too large for the columns is refused before anything is written.
    CHECK_THROWS(m.saveDraft({}, "BM-2cA", "BM-2cB", std::string(8193, 's'), "b"));
    CHECK_EQ(m.messageCount("Drafts", ""), 1);
    m.close();
}

TEST(a_cancelled_delivery_is_recorded_and_stops_the_network_work) {
    const auto path = aNewMailbox();
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    const auto id = m.saveDraft({}, "BM-2cA", "BM-2cB", "Subject", "Body");
    m.queueDraft(id, "direct", soon());
    CHECK_EQ(m.events(id).size(), size_t(1)); // the queue wrote the first one itself
    // A state is recorded once, and never for a chat: the chats keep their own events.
    m.recordMilestone(id, "waiting", "first");
    m.recordMilestone(id, "waiting", "a second time, ignored");
    m.recordMilestone("chat-somewhere", "waiting", "not written here");
    CHECK_EQ(m.events(id).size(), size_t(2));
    CHECK_EQ(m.events(id).back().state, std::string("waiting"));
    CHECK_THROWS(m.outgoing(id + "-missing")); // a letter id with no record
    CHECK_THROWS(m.setDelivery("nope", "failed", "no such delivery"));
    // A letter the network is still working on cannot go to Trash: it has to be cancelled first
    // (isTerminal), and then it can.
    CHECK_THROWS(m.moveMessage(id, "Trash"));
    m.cancel(id);
    CHECK_EQ(m.outgoing(id).state, std::string("cancelled"));
    CHECK(m.events(id).back().detail.find("Further work stopped") != std::string::npos);
    m.moveMessage(id, "Trash");
    CHECK_EQ(m.message(id).folder, std::string("Trash"));
    // What is already published cannot be cancelled, and a token that cannot be one acknowledges
    // nothing.
    const auto other = m.saveDraft({}, "BM-2cA", "BM-2cB", "Other", "Body");
    m.queueDraft(other, "direct", soon());
    m.setDelivery(other, "published", "published");
    CHECK_THROWS(m.cancel(other));
    CHECK(!m.acknowledge(bmc::Bytes(3, 0x01)));
    m.close();
}

TEST(a_chat_delivery_is_acknowledged_too) {
    const auto path = aNewMailbox();
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    const auto chat = m.queueChat("BM-local", "BM-peer", "hello", soon());
    try {
        m.cancel(chat + "-missing");
        bmc::test::fail(__FILE__, __LINE__, "a chat with no delivery record was cancelled");
    } catch (const bmc::Error &e) {
        CHECK_EQ(std::string(e.what()), std::string("No delivery record for this chat"));
    }
    // A chat's milestone is not written into the letters' table.
    m.recordMilestone(chat, "waiting", "x");
    CHECK(m.events(chat).empty());
    // Its acknowledgment token is acknowledged all the same, once.
    bmc::Bytes token(32, 0x11);
    m.setAck(chat, token);
    CHECK(m.acknowledge(token));
    CHECK(!m.acknowledge(token));
    CHECK_EQ(m.outgoing(chat).state, std::string("acknowledged"));
    m.close();
}
