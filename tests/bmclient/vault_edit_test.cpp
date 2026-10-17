// The vault's editing API: an identity made elsewhere, a chan from its phrase, the keys.dat of the
// previous release, renames, deletions, the default identity, and a password change. Until now
// these were reached only through the Qt wrapper, so the Qt-free build had none of them.
#include "storage_fixtures.h"
#include <bmc/error.h>
#include <bmc/protocol.h>
#include <bmc/test.h>
#include <bmc/vault.h>
#include <cstdio>
#include <filesystem>

using bmc::Vault;

namespace {
// The "general" chan of the previous release's own fixture (tests/core_tests.cpp): its address at
// version 3, and the keys.dat it was exported to there.
constexpr const char *kGeneralChan = "BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr";
constexpr const char *kKeysDat =
    "[BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr]\n"
    "label = General\n"
    "chan = true\n"
    "privsigningkey = 5Jnbdwc4u4DG9ipJxYLznXSvemkRFueQJNHujAQamtDDoX3N1eQ\n"
    "privencryptionkey = 5JrDcFtQDv5ydcHRW6dfGUEvThoxCCLNEUaxQfy8LXXgTJzVAcq\n";

std::string writeTemp(const std::string &name, const std::string &text) {
    const auto path = bmc::test::tempDir() + "/" + name;
    bmc::test::writeFile(path, bmc::view(text));
    return path;
}

std::string hexOf(const bmc::Secret &secret) {
    return bmc::hex(bmc::ByteView(secret.data(), secret.size()));
}
} // namespace

TEST(a_password_change_keeps_the_vault) {
    const auto path = bmc::test::tempDir() + "/pw.bmvault";
    Vault v;
    v.create(path, "first password");
    v.addIdentity("me");
    v.addMailboxKey();
    v.changePassword("second password");
    v.lock();
    Vault wrong;
    CHECK_THROWS(wrong.unlock(path, "first password"));
    Vault again;
    again.unlock(path, "second password");
    CHECK_EQ(again.identities().size(), size_t(1));
    CHECK_EQ(again.identities().front().label, std::string("me"));
    CHECK_EQ(again.mailboxIds().size(), size_t(1));
}

TEST(an_identity_from_elsewhere_is_added_by_its_keys) {
    const auto path = bmc::test::tempDir() + "/id.bmvault";
    Vault v;
    v.create(path, "password");
    auto mine = bmc::Protocol::identity("mine");
    CHECK_EQ(v.addIdentity(mine), mine.address);
    CHECK(v.identities().front().isDefault); // the first one there is the default
    CHECK_THROWS(v.addIdentity(mine));       // the same identity twice
    auto tampered = bmc::Protocol::identity("elsewhere");
    tampered.address = mine.address; // keys that are not the ones of that address
    CHECK_THROWS(v.addIdentity(tampered));
    CHECK_EQ(v.identities().size(), size_t(1));
    v.lock();
    Vault again;
    again.unlock(path, "password");
    CHECK_EQ(again.identities().size(), size_t(1));
    CHECK_EQ(again.identities().front().address, mine.address);
    CHECK_EQ(hexOf(again.identities().front().keys), hexOf(mine.keys));
}

TEST(a_channel_joins_with_its_phrase) {
    const auto path = bmc::test::tempDir() + "/chan.bmvault";
    Vault v;
    v.create(path, "password");
    // The address of the phrase, as the previous release's fixture has it, so the version the
    // address pins is the one the keys are made at.
    CHECK_EQ(v.addChannel("general", "general", kGeneralChan), std::string(kGeneralChan));
    CHECK(v.identities().front().chan);
    CHECK_EQ(v.identities().front().label, std::string("#general"));
    CHECK(v.identities().front().isDefault);
    // An empty label names the chan after its phrase.
    v.addChannel("bitmessage", "");
    CHECK_EQ(v.identities()[1].label, std::string("#bitmessage"));
    CHECK(v.identities()[1].chan);
    // A chan joins once, and the phrase has to be the one of the address it was given.
    CHECK_THROWS(v.addChannel("general", "general", kGeneralChan));
    CHECK_THROWS(v.addChannel("a different phrase", "general", kGeneralChan));
    CHECK_THROWS(v.addChannel("general", "general", "BM-not-an-address"));
    CHECK_EQ(v.identities().size(), size_t(2));
    Vault locked;
    CHECK_THROWS(locked.addChannel("general", "general"));
}

TEST(keys_dat_of_the_previous_release_is_imported) {
    const auto path = bmc::test::tempDir() + "/imported.bmvault";
    const auto keys = writeTemp("keys.dat", kKeysDat);
    Vault v;
    v.create(path, "password");
    CHECK_THROWS(v.importKeys(keys + ".not-there"));
    v.importKeys(keys);
    CHECK_EQ(v.identities().size(), size_t(1));
    CHECK_EQ(v.identities().front().address, std::string(kGeneralChan));
    CHECK(v.identities().front().chan);
    CHECK_EQ(v.identities().front().label, std::string("#General")); // named as a chan
    CHECK(std::filesystem::exists(keys)); // the source stays where it was
    v.lock();
    Vault again;
    again.unlock(path, "password");
    CHECK_EQ(again.identities().size(), size_t(1));
    CHECK_EQ(again.identities().front().address, std::string(kGeneralChan));
    // Importing what is already there finds nothing new and says so (the app shows that line), and
    // a section with a signing key only is skipped rather than half-imported.
    try {
        again.importKeys(keys);
        bmc::test::fail(__FILE__, __LINE__, "an already imported file was taken as new");
    } catch (const bmc::Error &e) {
        CHECK_EQ(std::string(e.what()),
                 std::string("No new private identities found in this file"));
    }
    CHECK_EQ(again.identities().size(), size_t(1));
    const auto half = writeTemp(
        "half.dat", "[BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr]\nlabel = General\nchan = true\n"
                    "privsigningkey = 5Jnbdwc4u4DG9ipJxYLznXSvemkRFueQJNHujAQamtDDoX3N1eQ\n");
    try {
        again.importKeys(half);
        bmc::test::fail(__FILE__, __LINE__, "a section with one key was imported");
    } catch (const bmc::Error &e) {
        CHECK_EQ(std::string(e.what()),
                 std::string("No new private identities found in this file"));
    }
    // A key that does not check out is refused by name, not imported anyway.
    const auto bad = writeTemp(
        "bad.dat", "[BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr]\nlabel = General\n"
                   "privsigningkey = 5Jnbdwc4u4DG9ipJxYLznXSvemkRFueQJNHujAQamtDDoX3N1eR\n"
                   "privencryptionkey = 5JrDcFtQDv5ydcHRW6dfGUEvThoxCCLNEUaxQfy8LXXgTJzVAcq\n");
    try {
        again.importKeys(bad);
        bmc::test::fail(__FILE__, __LINE__, "a key with a broken checksum was imported");
    } catch (const bmc::Error &e) {
        CHECK_EQ(std::string(e.what()), std::string("Private key checksum mismatch"));
    }
    CHECK_EQ(again.identities().size(), size_t(1));
}

TEST(identities_are_renamed_and_removed) {
    const auto path = bmc::test::tempDir() + "/edit.bmvault";
    Vault v;
    v.create(path, "password");
    const auto first = v.addIdentity("first");
    const auto second = v.addIdentity("second");
    v.addChannel("general", "general", kGeneralChan);
    CHECK(v.identities().front().isDefault);
    // A plain identity takes the label as it is; a chan is named as a chan.
    v.renameIdentity(second, "Second");
    CHECK_EQ(v.identities()[1].label, std::string("Second"));
    v.renameIdentity(kGeneralChan, "talk");
    CHECK_EQ(v.identities()[2].label, std::string("#talk"));
    CHECK_THROWS(v.renameIdentity("BM-not-one-of-ours", "x"));
    CHECK_THROWS(v.renameIdentity(second, std::string(257, 'x')));
    // Removing the default moves it to the first identity left.
    v.deleteIdentity(first);
    CHECK_EQ(v.identities().size(), size_t(2));
    CHECK_EQ(v.identities().front().address, second);
    CHECK(v.identities().front().isDefault);
    CHECK_THROWS(v.deleteIdentity(first));
    Vault locked;
    CHECK_THROWS(locked.deleteIdentity(second));
    v.lock();
    Vault again;
    again.unlock(path, "password");
    CHECK_EQ(again.identities().size(), size_t(2));
    CHECK_EQ(again.identities().front().label, std::string("Second"));
    CHECK(again.identities().front().isDefault);
    CHECK_EQ(again.identities()[1].label, std::string("#talk"));
}

TEST(the_default_identity_moves) {
    const auto path = bmc::test::tempDir() + "/default.bmvault";
    Vault v;
    v.create(path, "password");
    v.addIdentity("first");
    const auto second = v.addIdentity("second");
    v.setDefaultIdentity(second);
    CHECK(!v.identities()[0].isDefault);
    CHECK(v.identities()[1].isDefault);
    CHECK_THROWS(v.setDefaultIdentity("BM-not-one-of-ours"));
    Vault locked;
    CHECK_THROWS(locked.setDefaultIdentity(second));
    v.lock();
    Vault again;
    again.unlock(path, "password");
    CHECK_EQ(again.identities()[1].address, second);
    CHECK(again.identities()[1].isDefault);
    CHECK(!again.identities()[0].isDefault);
}
