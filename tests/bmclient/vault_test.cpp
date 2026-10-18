#include "storage_fixtures.h"
#include <bmc/test.h>
#include <bmc/vault.h>
#include <filesystem>
#include <iostream>

using bmc::Vault;

namespace {
// Make a directory read-only and answer whether the permission bits actually bind this user. Root
// is not bound by them (a container runs as root), so a test of a failed write probes with a file
// of its own rather than reporting a failure that belongs to the user and not to the vault.
bool makeReadOnly(const std::string &dir) {
    const auto readOnly = std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec;
    std::filesystem::permissions(dir, readOnly);
    bmc::test::writeFile(dir + "/probe", bmc::view(std::string("x")));
    const bool enforced = !std::filesystem::exists(dir + "/probe");
    if (!enforced)
        std::filesystem::permissions(dir, std::filesystem::perms::owner_all);
    return enforced;
}
} // namespace

TEST(a_vault_round_trips_through_the_core) {
    const auto path = bmc::test::tempDir() + "/v.bmvault";
    Vault v;
    v.create(path, "correct horse");
    CHECK(v.unlocked());
    const auto address = v.addIdentity("me");
    CHECK(!address.empty());
    v.addIdentity("\xc3\x9cn\xc3\xaf\xc3\xa7\xc3\xb8"
                  "d\xc3\xa9");
    const auto id = v.addMailboxKey();
    v.lock();
    CHECK(!v.unlocked());
    Vault again;
    again.unlock(path, "correct horse");
    CHECK_EQ(again.identities().size(), size_t(2));
    CHECK_EQ(again.identities().front().label, std::string("me"));
    CHECK(again.identities().front().isDefault);
    CHECK_EQ(again.identities()[1].label, std::string("\xc3\x9cn\xc3\xaf\xc3\xa7\xc3\xb8"
                                                      "d\xc3\xa9"));
    CHECK_EQ(again.mailboxKey(id).size(), size_t(32));
    CHECK_EQ(again.mailboxIds().size(), size_t(1));
}

TEST(the_golden_v2_vault_of_the_previous_release_opens) {
    const auto path = bmc::test::tempDir() + "/golden.bmvault";
    const auto file = fixtures::goldenVault2();
    bmc::test::writeFile(path, file);
    Vault v;
    v.unlock(path, fixtures::goldenPassword());
    CHECK_EQ(v.identities().size(), size_t(1));
    CHECK_EQ(v.identities().front().label, std::string("golden"));
    CHECK(v.identities().front().isDefault);
    CHECK(!v.identities().front().chan);
    CHECK_EQ(v.mailboxIds().size(), size_t(1));
    CHECK_EQ(v.mailboxKey("mailbox-1").size(), size_t(32));
}

TEST(the_golden_v1_vault_of_the_previous_release_opens) {
    const auto path = bmc::test::tempDir() + "/golden1.bmvault";
    bmc::test::writeFile(path, fixtures::goldenVault1());
    Vault v;
    v.unlock(path, fixtures::goldenPassword());
    CHECK_EQ(v.identities().size(), size_t(1));
    CHECK_EQ(v.identities().front().label, std::string("golden1"));
    CHECK(v.identities().front().isDefault); // v1 had no flag; the first identity is the default
    CHECK_EQ(v.mailboxIds().size(), size_t(0));
}

TEST(a_chan_with_the_old_name_is_renamed_on_open) {
    const auto path = bmc::test::tempDir() + "/old.bmvault";
    bmc::test::writeFile(path, fixtures::goldenVaultOldLabel());
    Vault v;
    v.unlock(path, fixtures::goldenPassword());
    CHECK_EQ(v.identities().front().chan, true);
    CHECK_EQ(v.identities().front().label, std::string("#x"));
}

TEST(a_read_only_vault_still_opens) {
    const auto dir = bmc::test::tempDir();
    const auto path = dir + "/ro.bmvault";
    bmc::test::writeFile(path, fixtures::goldenVaultOldLabel());
    const auto before = bmc::test::readFile(path);
    // The vault is this user's to read but not to write: the directory is read-only, so the atomic
    // write cannot even make its temporary file, and the rename that opening wants is the fallback
    // path -- kept in memory, with the file on disk left as it was.
    if (!makeReadOnly(dir)) {
        std::cerr << "skipped: the permission bits are not enforced for this user" << std::endl;
        return;
    }
    Vault v;
    v.unlock(path, fixtures::goldenPassword());
    CHECK_EQ(v.identities().front().label, std::string("#x")); // renamed in memory
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all);
    CHECK_EQ(bmc::test::readFile(path), before); // the save failed, so the file is unchanged
}

TEST(a_failed_save_keeps_the_previous_vault) {
    const auto dir = bmc::test::tempDir();
    const auto path = dir + "/big.bmvault";
    Vault v;
    v.create(path, "pw");
    v.addIdentity("me");
    const auto before = bmc::test::readFile(path);
    // A read-only directory makes the atomic write fail before the target is touched.
    if (!makeReadOnly(dir)) {
        std::cerr << "skipped: the permission bits are not enforced for this user" << std::endl;
        return;
    }
    CHECK_THROWS(v.addIdentity("second"));
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all);
    CHECK_EQ(bmc::test::readFile(path), before);
}

TEST(vault_limits_are_refused) {
    // The vault holds at most 1024 mailbox keys: the 1025th is refused, the in-memory map is rolled
    // back, and the file on disk keeps exactly the 1024 that were saved. The identity count is
    // guarded by the same require.
    const auto path = bmc::test::tempDir() + "/limits.bmvault";
    Vault v;
    v.create(path, "pw");
    for (int i = 0; i < 1024; ++i)
        v.addMailboxKey();
    CHECK_EQ(v.mailboxIds().size(), size_t(1024));
    CHECK_THROWS(v.addMailboxKey());
    CHECK_EQ(v.mailboxIds().size(), size_t(1024)); // rolled back, not left one too large
    v.lock();
    Vault again;
    again.unlock(path, "pw");
    CHECK_EQ(again.mailboxIds().size(), size_t(1024));
}

TEST(a_wrong_password_is_refused) {
    const auto path = bmc::test::tempDir() + "/w.bmvault";
    Vault v;
    v.create(path, "right");
    v.lock();
    Vault again;
    CHECK_THROWS(again.unlock(path, "wrong"));
    CHECK(!again.unlocked());
}

TEST(a_vault_of_a_later_format_is_refused) {
    // Only BMVAULT1 and BMVAULT2 exist; anything else is refused by name, before any decryption,
    // so the refusal is the format's and not the password's.
    const auto path = bmc::test::tempDir() + "/future.bmvault";
    bmc::Bytes file(256, 0x00);
    std::copy_n(reinterpret_cast<const uint8_t *>("BMVAULT3"), 8, file.begin());
    bmc::test::writeFile(path, file);
    Vault v;
    try {
        v.unlock(path, "any password at all");
        bmc::test::fail(__FILE__, __LINE__, "BMVAULT3 was accepted");
    } catch (const bmc::Error &e) {
        CHECK_EQ(std::string(e.what()), std::string("Unsupported vault format"));
        CHECK_EQ(int(e.code()), int(bmc::Code::Invalid));
    }
    CHECK(!v.unlocked());
}
