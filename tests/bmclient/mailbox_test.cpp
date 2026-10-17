#include "storage_fixtures.h"
#include <bmc/clock.h>
#include <bmc/mailbox.h>
#include <bmc/random.h>
#include <bmc/test.h>
#include <filesystem>
#include <sqlcipher/sqlite3.h>

using bmc::Mailbox;

namespace {
bmc::Secret key32(uint8_t fill = 0x01) {
    bmc::Secret key(32);
    for (int i = 0; i < 32; ++i)
        key.data()[i] = fill;
    return key;
}
std::string aNewMailbox(const char *name = "m.bmmail") {
    return bmc::test::tempDir() + "/" + name;
}
// A mailbox file the tests open with the raw SQLCipher API, to put it into a state the public API
// would never make: another format version, or metadata that is not there at all. The key is the
// same 32 bytes the mailbox itself keys the file with.
void editMailbox(const std::string &path, const bmc::Secret &key, const char *sql) {
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
        bmc::test::fail(__FILE__, __LINE__, "cannot open the mailbox file");
    const bool keyed = sqlite3_key(db, key.data(), 32) == SQLITE_OK;
    const bool ran = keyed && sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    const std::string why = ran ? std::string() : sqlite3_errmsg(db);
    sqlite3_close(db);
    if (!ran)
        bmc::test::fail(__FILE__, __LINE__, why);
}
} // namespace

TEST(a_new_mailbox_has_the_schema_and_a_key_id) {
    const auto path = aNewMailbox();
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    CHECK(m.isOpen());
    CHECK_EQ(m.keyId(), std::string("key-1"));
    CHECK_EQ(m.checkpoint(), int64_t(0));
    CHECK_EQ(m.messages().size(), size_t(0));
    m.close();
    CHECK(!m.isOpen());
}

TEST(a_letter_round_trips_and_unread_is_tracked) {
    const auto path = aNewMailbox();
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    m.store("hash-a", "BM-2cA", "BM-2cB", "Subject A", "Body A", -1);
    m.store("hash-b", "BM-2cC", "BM-2cB", "Subject B", "Body B", -1);
    CHECK_EQ(m.messages().size(), size_t(2));
    CHECK(m.unread("hash-a"));
    // A letter stored by a catch-up pass lands behind the checkpoint and leaves it be.
    m.store("hash-c", "BM-2cA", "BM-2cB", "Subject C", "Body C", 99);
    CHECK_EQ(m.checkpoint(), int64_t(99));
    CHECK_EQ(m.messageCount("Inbox", ""), 3);
    m.markRead("hash-a");
    CHECK(!m.unread("hash-a"));
    CHECK(m.unread("hash-b"));
    CHECK_EQ(m.messageSummaries(1).size(), size_t(1));
    const auto summaries = m.messageSummaries("Inbox", "", 0, 10);
    CHECK_EQ(summaries.size(), size_t(3));
    CHECK_EQ(summaries.front().folder, std::string("Inbox"));
    CHECK_EQ(m.message("hash-b").subject, std::string("Subject B"));
    const auto hashes = m.messageHashes("Inbox");
    CHECK_EQ(hashes.size(), size_t(3));
    CHECK(m.messagePosition("hash-a", "Inbox") >= 0);
    CHECK_EQ(m.messagePosition("nope", "Inbox"), -1);
    m.close();
}

TEST(a_letter_moves_between_folders_and_is_deleted_from_trash) {
    const auto path = aNewMailbox();
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    m.store("hash-a", "BM-2cA", "BM-2cB", "Subject A", "Body A", -1);
    m.moveMessage("hash-a", "Archive");
    CHECK_EQ(m.message("hash-a").folder, std::string("Archive"));
    m.restoreMessage("hash-a");
    CHECK_EQ(m.message("hash-a").folder, std::string("Inbox"));
    m.moveMessage("hash-a", "Trash");
    CHECK_THROWS(m.deleteMessage("hash-b")); // not in Trash
    m.deleteMessage("hash-a");
    CHECK_THROWS(m.message("hash-a")); // "Letter no longer exists"
    CHECK_EQ(m.messages().size(), size_t(0));
    m.close();
}

TEST(a_draft_is_saved_and_queued_for_delivery) {
    const auto path = aNewMailbox();
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    const auto id = m.saveDraft({}, "BM-2cA", "BM-2cB", "Draft subject", "Draft body");
    CHECK_EQ(m.message(id).folder, std::string("Drafts"));
    CHECK_THROWS(m.queueDraft(id, "direct", bmc::now() - 1)); // "Message expiry is in the past"
    m.queueDraft(id, "direct", bmc::now() + 3600);
    CHECK_EQ(m.message(id).folder, std::string("Outbox"));
    const auto o = m.outgoing(id);
    CHECK_EQ(o.kind, std::string("direct"));
    CHECK_EQ(o.state, std::string("queued"));
    CHECK_EQ(o.attempts, 1);
    CHECK(!o.chat);
    CHECK_EQ(m.outbox().size(), size_t(1));
    const auto events = m.events(id);
    CHECK(!events.empty());
    CHECK_EQ(events.front().state, std::string("queued"));
    m.setObjectHash(id, "object-hash");
    m.setAck(id, bmc::Bytes(32, 0x07));
    CHECK(m.acknowledge(bmc::Bytes(32, 0x07)));
    CHECK(!m.acknowledge(bmc::Bytes(32, 0x08)));
    m.close();
}

TEST(a_job_and_a_public_key_round_trip) {
    const auto path = aNewMailbox();
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    bmc::NetworkJob job;
    job.owner = "owner-1";
    job.kind = "pubkey";
    job.address = "BM-2cB";
    job.payload = bmc::Bytes(10, 0x09);
    const auto id = m.addJob(job);
    CHECK(!id.empty());
    const auto jobs = m.jobs();
    CHECK_EQ(jobs.size(), size_t(1));
    CHECK_EQ(jobs.front().nonceTrials, uint64_t(1000));
    m.updateJob(id, "running");
    CHECK_EQ(m.jobs().front().state, std::string("running"));
    m.removeJob(id);
    CHECK_EQ(m.jobs().size(), size_t(0));

    bmc::StoredPublicKey pk;
    pk.address = "BM-2cB";
    pk.signingKey = bmc::Bytes(65, 0x02);
    pk.encryptionKey = bmc::Bytes(65, 0x03);
    pk.expires = bmc::now() + 3600;
    m.savePublicKey(pk);
    const auto back = m.publicKey("BM-2cB", bmc::now());
    CHECK(back.has_value());
    CHECK_EQ(back->signingKey.size(), size_t(65));
    CHECK(!m.publicKey("BM-2cB", bmc::now() + 7200).has_value()); // expired
    m.close();
}

TEST(subscriptions_contacts_and_settings_round_trip) {
    const auto path = aNewMailbox();
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    CHECK_THROWS(m.subscribe("", "label")); // "Enter a broadcast address"
    m.subscribe("BM-2cBroadcaster", "Release notes");
    CHECK_EQ(m.subscriptions().size(), size_t(1));
    m.unsubscribe("BM-2cBroadcaster");
    CHECK_EQ(m.subscriptions().size(), size_t(0));
    CHECK_THROWS(m.saveContact("BM-2cA", "  ")); // "A contact needs an address and a name"
    m.saveContact("BM-2cA", "  Ada  ");
    CHECK_EQ(m.contacts().front().label, std::string("Ada")); // trimmed
    m.removeContact("BM-2cA");
    CHECK_EQ(m.contacts().size(), size_t(0));
    CHECK_EQ(m.setting("theme"), std::string());
    CHECK_EQ(m.setting("theme", "dark"), std::string("dark"));
    m.setSetting("theme", "light");
    CHECK_EQ(m.setting("theme"), std::string("light"));
    m.close();
}

TEST(channels_are_listed_with_their_unread_state) {
    const auto path = aNewMailbox();
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    m.store("hash-1", "BM-2cChan", "BM-2cChan", "On a chan", "Body", -1, "Channels");
    m.store("hash-2", "BM-2cElse", "BM-2cOther", "A letter", "Body", -1, "Inbox");
    CHECK_EQ(m.channelAddresses().size(), size_t(1));
    CHECK_EQ(m.channelAddresses().front(), std::string("BM-2cChan"));
    CHECK(m.channelUnread("BM-2cChan"));
    m.close();
}

TEST(a_backup_is_a_usable_mailbox) {
    const auto path = aNewMailbox();
    const auto copy = aNewMailbox("copy.bmmail");
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    m.store("hash-a", "BM-2cA", "BM-2cB", "Subject A", "Body A", -1);
    m.backup(copy, key);
    Mailbox other;
    other.open(copy, key);
    CHECK_EQ(other.messages().size(), size_t(1));
    CHECK_EQ(other.keyId(), std::string("key-1"));
    other.close();
    m.close();
}

TEST(the_golden_v1_mailbox_of_the_previous_release_migrates) {
    const auto path = aNewMailbox("golden1.bmmail");
    bmc::test::writeFile(path, fixtures::goldenMailbox1());
    auto key = fixtures::goldenMailboxKey();
    Mailbox m;
    m.open(path, key);
    CHECK(m.isOpen());
    CHECK_EQ(m.keyId(), std::string("key-1"));
    CHECK_EQ(m.messages().size(), size_t(1)); // the letter the generator stored
    CHECK(m.unread("hash-welcome"));          // migrate() gave Inbox letters unread=1
    CHECK_EQ(m.subscriptions().size(), size_t(0));
    m.close();
}

TEST(the_golden_v2_mailbox_of_the_previous_release_opens) {
    const auto path = aNewMailbox("golden2.bmmail");
    bmc::test::writeFile(path, fixtures::goldenMailbox2());
    auto key = fixtures::goldenMailboxKey();
    Mailbox m;
    m.open(path, key);
    CHECK_EQ(m.keyId(), std::string("key-1"));
    CHECK_EQ(m.checkpoint(), int64_t(42));
    CHECK_EQ(m.messages().size(), size_t(4)); // two Inbox letters, a draft, an archived letter
    CHECK_EQ(m.subscriptions().size(), size_t(1));
    CHECK_EQ(m.contacts().size(), size_t(1));
    CHECK_EQ(m.contacts().front().label, std::string("Ada"));
    const auto pk = m.publicKey("BM-2cSenderTwo", 1700000000);
    CHECK(pk.has_value());
    CHECK_EQ(pk->signingKey.size(), size_t(65));
    CHECK_EQ(m.outbox().size(), size_t(1)); // the queued chat
    m.close();
}

TEST(a_wrong_mailbox_key_is_refused) {
    const auto path = aNewMailbox();
    Mailbox m;
    m.create(path, "key-1", key32(0x01));
    m.close();
    Mailbox other;
    CHECK_THROWS(other.open(path, key32(0x02)));
    CHECK(!other.isOpen());
}

TEST(a_mailbox_of_another_format_or_with_no_metadata_is_refused) {
    const auto path = aNewMailbox("format.bmmail");
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    m.close();
    // A file whose user_version is not one this build knows: refused by name, with the file left
    // where it is, as the vault's own format check does.
    editMailbox(path, key, "PRAGMA user_version=3");
    try {
        Mailbox later;
        later.open(path, key);
        bmc::test::fail(__FILE__, __LINE__, "a mailbox of a later format was opened");
    } catch (const bmc::Error &e) {
        CHECK_EQ(std::string(e.what()), std::string("Unsupported mailbox format"));
        CHECK_EQ(int(e.code()), int(bmc::Code::Invalid));
    }
    editMailbox(path, key, "PRAGMA user_version=2");
    // One that claims this format but has no metadata row is not a mailbox either, and was not
    // removed by the open that failed.
    editMailbox(path, key, "DELETE FROM meta");
    try {
        Mailbox empty;
        empty.open(path, key);
        bmc::test::fail(__FILE__, __LINE__, "a mailbox with no metadata was opened");
    } catch (const bmc::Error &e) {
        CHECK_EQ(std::string(e.what()), std::string("Invalid mailbox"));
        CHECK_EQ(int(e.code()), int(bmc::Code::Invalid));
    }
    CHECK(std::filesystem::exists(path));
}

TEST(a_scan_batch_commits_what_it_stored_or_leaves_it_out) {
    const auto path = aNewMailbox("batch.bmmail");
    auto key = key32();
    Mailbox m;
    m.create(path, "key-1", key);
    {
        Mailbox::Batch batch(m);
        m.store("hash-1", "BM-2cA", "BM-2cB", "Subject", "Body", 5);
        batch.commit();
    }
    CHECK_EQ(m.messageCount("Inbox", ""), 1);
    CHECK_EQ(m.checkpoint(), int64_t(5));
    // A batch that is not committed leaves nothing behind at all: the letters and the checkpoint it
    // moved are both back where they were, which is what a scan that throws wants.
    {
        Mailbox::Batch batch(m);
        m.store("hash-2", "BM-2cA", "BM-2cB", "Subject", "Body", 9);
        CHECK_EQ(m.messageCount("Inbox", ""), 2); // visible inside the batch
        CHECK_EQ(m.checkpoint(), int64_t(9));
    }
    CHECK_EQ(m.messageCount("Inbox", ""), 1);
    CHECK_EQ(m.checkpoint(), int64_t(5));
    m.close();
}
