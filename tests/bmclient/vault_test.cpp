#include "storage_fixtures.h"
#include <bmc/test.h>
#include <bmc/vault.h>
#include <filesystem>

using bmc::Vault;

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
    std::filesystem::permissions(path, std::filesystem::perms::owner_read);
    Vault v;
    v.unlock(path, fixtures::goldenPassword());
    CHECK_EQ(v.identities().front().label, std::string("#x")); // renamed in memory
    std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write);
}

TEST(a_failed_save_keeps_the_previous_vault) {
    const auto dir = bmc::test::tempDir();
    const auto path = dir + "/big.bmvault";
    Vault v;
    v.create(path, "pw");
    v.addIdentity("me");
    const auto before = bmc::test::readFile(path);
    // A read-only directory makes the atomic write fail before the target is touched.
    std::filesystem::permissions(dir, std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_exec);
    CHECK_THROWS(v.addIdentity("second"));
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all);
    CHECK_EQ(bmc::test::readFile(path), before);
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
