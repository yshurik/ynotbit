// The node's object store, read in std: what the scan sees, what it passes over, and what a store
// the node has not made (or has made differently) reads as.
#include <bmc/cache.h>
#include <bmc/hash.h>
#include <bmc/mailbox.h>
#include <bmc/path.h>
#include <bmc/protocol.h>
#include <bmc/secret.h>
#include <bmc/test.h>
#include <bmc/vault.h>
#include <filesystem>
#include <fstream>
#include <ntb-object-db.h>
#include <sqlcipher/sqlite3.h>

namespace {
int64_t g_received = 1'700'000'000;

// Writes an object the way the node does: through its own store.
void cacheObject(const std::string &root, bmc::ByteView payload) {
    std::filesystem::create_directories(root);
    auto *db = ntb_object_db_open(root.c_str());
    CHECK(db);
    const auto hash = bmc::unhex(bmc::Protocol::inventoryHash(payload));
    const bool saved =
        ntb_object_db_save(db, hash.data(), payload.data(), payload.size(), g_received++);
    ntb_object_db_close(db);
    CHECK(saved);
}
bmc::Secret key32(uint8_t fill = 0x01) {
    bmc::Secret key(32);
    for (int i = 0; i < 32; ++i)
        key.data()[i] = fill;
    return key;
}
void writeText(const std::string &path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), std::streamsize(text.size()));
}
} // namespace

TEST(a_missing_store_reads_as_empty) {
    const auto root = bmc::test::tempDir();
    bmc::Cache cache(root);
    CHECK_EQ(cache.after(0).size(), size_t(0));
    CHECK_EQ(cache.after(0, 256).size(), size_t(0));
    CHECK(!cache.hasAfter(0));
    CHECK_EQ(cache.firstSequence(), int64_t(0));
    CHECK_EQ(cache.id(), std::string());
}

TEST(a_store_reads_back_in_order) {
    const auto root = bmc::test::tempDir();
    const auto first = bmc::bytesOf("the first object");
    const auto second = bmc::bytesOf("the second object");
    const auto third = bmc::bytesOf("the third object");
    cacheObject(root, first);
    cacheObject(root, second);
    cacheObject(root, third);

    bmc::Cache cache(root);
    const auto all = cache.after(0, 32);
    CHECK_EQ(all.size(), size_t(3));
    CHECK_EQ(all[0].sequence, int64_t(1));
    CHECK_EQ(all[1].sequence, int64_t(2));
    CHECK_EQ(all[2].sequence, int64_t(3));
    // The hash is the node's own, lower-case hex of the same bytes Protocol uses.
    CHECK_EQ(all[0].hash, bmc::Protocol::inventoryHash(first));
    CHECK_EQ(all[2].hash, bmc::Protocol::inventoryHash(third));
    CHECK_EQ(all[1].payload, bmc::bytesOf(second));
    CHECK_EQ(cache.after(2).size(), size_t(1));
    CHECK_EQ(cache.after(2)[0].payload, bmc::bytesOf(third));
    CHECK_EQ(cache.after(3).size(), size_t(0));
    CHECK(cache.hasAfter(1));
    CHECK(!cache.hasAfter(3));
    CHECK_EQ(cache.firstSequence(), int64_t(1));
    CHECK_EQ(cache.id().size(), size_t(32)); // lower(hex(randomblob(16)))
}

TEST(a_foreign_store_reads_as_empty) {
    const auto root = bmc::test::tempDir();
    std::filesystem::create_directories(root);
    const auto file = root + "/objects.sqlite";
    sqlite3 *db = nullptr;
    CHECK_EQ(
        sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr),
        SQLITE_OK);
    CHECK_EQ(sqlite3_exec(db,
                          "CREATE TABLE objects(seq INTEGER PRIMARY KEY); PRAGMA user_version=99",
                          nullptr, nullptr, nullptr),
             SQLITE_OK);
    sqlite3_close(db);

    bmc::Cache cache(root);
    CHECK_EQ(cache.after(0).size(), size_t(0));
    CHECK(!cache.hasAfter(0));
    CHECK_EQ(cache.firstSequence(), int64_t(0));
    CHECK_EQ(cache.id(), std::string());
}

TEST(a_store_that_is_not_a_store_reads_as_empty) {
    const auto root = bmc::test::tempDir();
    std::filesystem::create_directories(root);
    writeText(root + "/objects.sqlite", "this is not a database at all, not even close");

    bmc::Cache cache(root);
    CHECK_THROWS(cache.after(0));
    CHECK_THROWS(cache.firstSequence());
    CHECK_THROWS(cache.id());
}

TEST(the_old_index_is_removed) {
    const auto root = bmc::test::tempDir();
    writeText(root + "/cache.sqlite", "the 0.5.1 index");
    writeText(root + "/cache.sqlite-journal", "its journal");
    bmc::Cache cache(root);
    CHECK(!std::filesystem::exists(bmc::path(root + "/cache.sqlite")));
    CHECK(!std::filesystem::exists(bmc::path(root + "/cache.sqlite-journal")));
}

TEST(an_oversized_object_is_passed_over) {
    const auto root = bmc::test::tempDir();
    std::vector<uint8_t> oversized(size_t(NTB_OBJECT_DB_MAX_OBJECT_SIZE) + 1, 0x7f);
    cacheObject(root, oversized);

    bmc::Vault vault;
    vault.create(root + "/v.bmvault", "slice3-cache");
    vault.addIdentity("me");
    bmc::Mailbox mailbox;
    mailbox.create(root + "/m.bmmail", "key-1", key32());

    bmc::Cache cache(root);
    CHECK_EQ(bmc::scanMailbox(cache, mailbox, vault), 0);
    // Larger than the protocol allows: not a letter, so only the checkpoint moved.
    CHECK_EQ(mailbox.checkpoint(), int64_t(1));
}
