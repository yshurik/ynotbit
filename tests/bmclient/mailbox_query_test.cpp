// The mailbox's list queries: the shared filter (search, recipient, unread, anonymous only) as the
// summaries, the count, the hashes and the position use it; the summaries of given hashes; and the
// two bindings a scan keeps (its cache id and the fingerprint of the identities it scanned with).
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
std::string aNewMailbox(const char *name = "q.bmmail") {
    return bmc::test::tempDir() + "/" + name;
}
// The letters are stored a second apart on a clock of the test's own, so the list's order is the
// one of `received` and does not depend on how fast the rows were inserted.
int64_t fakeClock = 1'700'000'000'000;
int64_t testClock() {
    return fakeClock;
}
void storeThree(Mailbox &m) {
    m.store("hash-a", "BM-2cAlice", "BM-2cBob", "Subject alpha", "Body one", -1);
    fakeClock += 1000;
    m.store("hash-b", "BM-2cBob", "BM-2cBob", "Subject beta", "Body alpha two", -1);
    fakeClock += 1000;
    m.store("hash-c", "BM-2cCarol", "BM-2cDave", "Third", "Body three", -1, "Archive");
    fakeClock += 1000;
}
} // namespace

TEST(the_list_filters_by_search_recipient_and_unread) {
    const auto path = aNewMailbox();
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    bmc::setClockForTesting(testClock);
    storeThree(m);
    // Folder first, then the three flags of the filter.
    CHECK_EQ(m.messageCount("Inbox", ""), 2);
    CHECK_EQ(m.messageCount("Archive", ""), 1);
    CHECK_EQ(m.messageCount("Inbox", "", "BM-2cBob"), 2); // both were sent to Bob
    CHECK_EQ(m.messageCount("Inbox", "", "BM-2cAlice"),
             0); // Alice is the sender, not the recipient
    CHECK_EQ(m.messageCount("Inbox", "", "", false, true), 1); // anonymous: sender==recipient
    CHECK_EQ(m.messageCount("Inbox", "", "", true), 2);
    m.markRead("hash-a");
    CHECK_EQ(m.messageCount("Inbox", "", "", true), 1);
    CHECK_EQ(m.messageCount("Inbox", "", "", true, true), 1);
    // The search looks in the subject, the sender, the recipient and the body, ignoring case.
    CHECK_EQ(m.messageCount("Inbox", "alpha"), 2);
    CHECK_EQ(m.messageCount("Inbox", "ALPHA"), 2);
    CHECK_EQ(m.messageCount("Inbox", "Body one"), 1);
    CHECK_EQ(m.messageCount("Inbox", "BM-2cAlice"), 1);
    CHECK_EQ(m.messageCount("Inbox", "nothing like it"), 0);
    // The same filter, through the summaries, the hashes and the position.
    const auto unread = m.messageSummaries("Inbox", "", 0, 10, "", true);
    CHECK_EQ(unread.size(), size_t(1));
    CHECK_EQ(unread.front().hash, std::string("hash-b"));
    const auto anonymous = m.messageHashes("Inbox", "", false, true);
    CHECK_EQ(anonymous.size(), size_t(1));
    CHECK_EQ(anonymous.front(), std::string("hash-b"));
    CHECK_EQ(m.messagePosition("hash-b", "Inbox"), 0);
    CHECK_EQ(m.messagePosition("hash-a", "Inbox"), 1);
    CHECK_EQ(m.messagePosition("hash-a", "Inbox", "", true), -1); // read: not in the unread list
    CHECK_EQ(m.messagePosition("hash-a", "Inbox", "", false, true), -1); // and not anonymous
    CHECK_EQ(m.messagePosition("hash-b", "Inbox", "", false, true), 0);
    // Newest first, and a window of the list from an offset.
    const auto all = m.messageHashes("Inbox");
    CHECK_EQ(all.size(), size_t(2));
    CHECK_EQ(all.front(), std::string("hash-b"));
    const auto window = m.messageSummaries("Inbox", "", 1, 10);
    CHECK_EQ(window.size(), size_t(1));
    CHECK_EQ(window.front().hash, std::string("hash-a"));
    CHECK_EQ(m.messageSummaries("Inbox", "", 0, 1).size(), size_t(1));
    m.close();
    bmc::setClockForTesting(nullptr);
}

TEST(summaries_by_hash_are_the_rows_the_list_shows) {
    const auto path = aNewMailbox();
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    bmc::setClockForTesting(testClock);
    storeThree(m);
    // In the order they were asked for, from any folder, and without the whole body.
    const auto asked = m.messageSummaries({"hash-c", "hash-a"});
    CHECK_EQ(asked.size(), size_t(2));
    CHECK_EQ(asked[0].hash, std::string("hash-c"));
    CHECK_EQ(asked[0].folder, std::string("Archive"));
    CHECK_EQ(asked[1].subject, std::string("Subject alpha"));
    CHECK_EQ(asked[1].from, std::string("BM-2cAlice"));
    CHECK_EQ(asked[1].to, std::string("BM-2cBob"));
    CHECK(asked[1].received > 0);
    // A hash that is not there is skipped, not an error, and a long body is cut where the list
    // cuts it.
    CHECK(m.messageSummaries({"nope"}).empty());
    m.store("hash-long", "BM-2cAlice", "BM-2cBob", std::string(300, 'S'), std::string(300, 'b'),
            -1);
    const auto long_one = m.messageSummaries({"hash-long"});
    CHECK_EQ(long_one.size(), size_t(1));
    CHECK_EQ(long_one.front().subject.size(), size_t(240));
    CHECK_EQ(long_one.front().body.size(), size_t(240));
    m.close();
    bmc::setClockForTesting(nullptr);
}

TEST(a_new_cache_or_identity_set_restarts_the_scan) {
    const auto path = aNewMailbox();
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    CHECK_EQ(m.boundIdentities(), std::string(""));
    // The first cache id of all is not the one the checkpoint was reached with (the empty one), so
    // the scan starts over; the same id again leaves the checkpoint alone.
    m.advance(42);
    m.bindCache("cache-1");
    CHECK_EQ(m.checkpoint(), int64_t(0));
    m.advance(42);
    m.bindCache("cache-1");
    CHECK_EQ(m.checkpoint(), int64_t(42));
    m.bindCache("cache-2");
    CHECK_EQ(m.checkpoint(), int64_t(0));
    // The identities fingerprint, the same way, and a relabel (keepCheckpoint) that must not move
    // the scan back.
    m.advance(7);
    m.bindIdentities("fp-1");
    CHECK_EQ(m.boundIdentities(), std::string("fp-1"));
    CHECK_EQ(m.checkpoint(), int64_t(0));
    m.advance(7);
    m.bindIdentities("fp-1");
    CHECK_EQ(m.checkpoint(), int64_t(7)); // unchanged: nothing to write
    m.bindIdentities("fp-1-relabelled", true);
    CHECK_EQ(m.boundIdentities(), std::string("fp-1-relabelled"));
    CHECK_EQ(m.checkpoint(), int64_t(7)); // kept on purpose
    m.bindIdentities("fp-2");
    CHECK_EQ(m.checkpoint(), int64_t(0)); // a different set of identities: scan again
    m.close();
}

TEST(a_closed_mailbox_says_so_and_a_short_key_is_refused) {
    Mailbox closed;
    CHECK_THROWS(closed.store("hash", "BM-2cA", "BM-2cB", "s", "b", -1));
    CHECK_THROWS(closed.backup(aNewMailbox("closed.bmmail"), key32()));
    try {
        bmc::Secret short_key(31);
        closed.create(aNewMailbox(), "key-1", short_key);
        bmc::test::fail(__FILE__, __LINE__, "a key that is not 32 bytes was accepted");
    } catch (const bmc::Error &e) {
        CHECK_EQ(std::string(e.what()), std::string("Invalid mailbox key"));
        CHECK_EQ(int(e.code()), int(bmc::Code::Argument));
    }
    const auto path = aNewMailbox();
    Mailbox m;
    m.create(path, "key-1", key32());
    // A second file while one is open, and a file that is already there.
    CHECK_THROWS(m.create(aNewMailbox("second.bmmail"), "key-2", key32()));
    CHECK_THROWS(m.create(path, "key-3", key32()));
    CHECK_EQ(m.keyId(), std::string("key-1")); // the first one is still the open one
    m.close();
}

TEST(the_same_filter_serves_the_summaries_the_hashes_and_the_position) {
    const auto path = aNewMailbox("filters.bmmail");
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    bmc::setClockForTesting(testClock);
    storeThree(m);
    // The summaries' call takes the search and the recipient, the hashes' and the position's the
    // recipient; each binds them into its own query in its own order.
    CHECK_EQ(m.messageSummaries("Inbox", "alpha", 0, 10, "BM-2cBob").size(), size_t(2));
    CHECK_EQ(m.messageSummaries("Inbox", "nothing like it", 0, 10, "BM-2cBob").size(), size_t(0));
    CHECK_EQ(m.messageSummaries("Archive", "", 0, 10, "BM-2cDave").size(), size_t(1));
    CHECK_EQ(m.messageHashes("Inbox", "BM-2cBob").size(), size_t(2));
    CHECK_EQ(m.messageHashes("Inbox", "BM-2cCarol").size(), size_t(0));
    CHECK_EQ(m.messagePosition("hash-a", "Inbox", "BM-2cBob"), 1);
    CHECK_EQ(m.messagePosition("hash-a", "Inbox", "BM-2cCarol"), -1);
    m.close();
    bmc::setClockForTesting(nullptr);
}

TEST(a_closed_mailbox_refuses_the_scan_state_as_well) {
    const auto path = aNewMailbox("closed2.bmmail");
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    m.close();
    CHECK_THROWS(m.advance(5));
    CHECK_THROWS(m.checkpoint());
    CHECK_THROWS(m.bindCache("cache-1"));
    CHECK_THROWS(m.bindIdentities("fp-1"));
}
