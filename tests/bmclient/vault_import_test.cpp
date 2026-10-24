// Taking the identities and chans of another client's keys file into the vault: one report entry
// each, one save for all, nothing added when the save fails.
#include "pybm_fixtures.h"
#include <bmc/error.h>
#include <bmc/protocol.h>
#include <bmc/pybitmessage.h>
#include <bmc/random.h>
#include <bmc/test.h>
#include <bmc/vault.h>
#include <deque>
#include <filesystem>
extern "C" {
#include <ntb-address.h>
}

using bmc::IdentityImport;
using bmc::Vault;
using Result = IdentityImport::Result;
using Why = IdentityImport::Why;

namespace {
const bmc::Identity &someone(int n) {
    static std::deque<bmc::Identity> made;
    while (int(made.size()) <= n)
        made.push_back(bmc::Protocol::identity("x"));
    return made[size_t(n)];
}
std::vector<bmc::pybm::KeyEntry> entriesOf(const std::string &keys) {
    const auto path = bmc::test::tempDir() + "/keys.dat";
    bmc::test::writeFile(path, bmc::view(keys));
    return std::move(bmc::pybm::readKeys(path).entries);
}
std::string newVault(Vault &v, const char *name = "v.bmvault") {
    const auto path = bmc::test::tempDir() + "/" + name;
    v.create(path, "password");
    return path;
}
bool makeReadOnly(const std::string &dir) {
    std::filesystem::permissions(dir, std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_exec);
    bmc::test::writeFile(dir + "/probe", bmc::view(std::string("x")));
    const bool enforced = !std::filesystem::exists(dir + "/probe");
    if (!enforced)
        std::filesystem::permissions(dir, std::filesystem::perms::owner_all);
    return enforced;
}
std::string describe(const Vault &v) {
    std::string out;
    for (const auto &i : v.identities())
        out += i.address + "|" + i.label + "|" + (i.chan ? "c" : "-") + (i.isDefault ? "d" : "-") +
               ";";
    return out;
}
} // namespace

TEST(identities_and_chans_of_a_keys_file_are_added_with_a_report) {
    const auto &a = someone(0), &b = someone(1), &c = someone(2), &d = someone(3);
    std::string keys = pybmfix::kSettings + pybmfix::section(a, "Work");
    keys += pybmfix::section(b, "Old address", false, false); // switched off over there
    keys += pybmfix::section(c, "[chan] general", true);
    keys += pybmfix::section(d, "Club") + "mailinglist = true\n";
    keys += "[BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr]\nlabel = Half\n"
            "privsigningkey = 5Jnbdwc4u4DG9ipJxYLznXSvemkRFueQJNHujAQamtDDoX3N1eQ\n\n";
    keys += "[BM-2cUHrvWBVLWZGCADpH3QVMfCWfXKMdGUP6]\nlabel = Broken\n"
            "privsigningkey = 5Jnbdwc4u4DG9ipJxYLznXSvemkRFueQJNHujAQamtDDoX3N1eR\n"
            "privencryptionkey = 5JrDcFtQDv5ydcHRW6dfGUEvThoxCCLNEUaxQfy8LXXgTJzVAcq\n\n";
    Vault v;
    const auto path = newVault(v);
    const auto items = v.importEntries(entriesOf(keys));
    CHECK_EQ(items.size(), size_t(6));
    CHECK(items[0].result == Result::Added && items[0].why == Why::None);
    CHECK_EQ(items[0].address, a.address);
    CHECK_EQ(items[0].label, std::string("Work"));
    CHECK(items[1].result == Result::Added &&
          items[1].wasDisabled); // comes in, and says it was off
    CHECK(items[2].result == Result::Added && items[2].chan);
    CHECK_EQ(items[2].label, std::string("#general")); // named as a chan
    CHECK(items[3].result == Result::Added && items[3].mailingList);
    CHECK(items[4].result == Result::Unusable && items[4].why == Why::MissingKey);
    CHECK(items[5].result == Result::Unusable && items[5].why == Why::BadKey);
    CHECK_EQ(items[5].label, std::string("Broken"));
    CHECK_EQ(v.identities().size(), size_t(4));
    CHECK_EQ(v.identities()[0].address, a.address);
    CHECK(v.identities()[0].isDefault); // the first of an empty vault is its default
    CHECK(!v.identities()[1].isDefault);
    CHECK(v.identities()[2].chan && !v.identities()[0].chan);
    // The keys are the ones of the file.
    CHECK(std::memcmp(v.identities()[1].keys.data(), b.keys.data(), 64) == 0);
    // And it is saved.
    v.lock();
    Vault again;
    again.unlock(path, "password");
    CHECK_EQ(again.identities().size(), size_t(4));
    CHECK_EQ(again.identities()[2].label, std::string("#general"));
}

TEST(what_the_vault_has_is_left_as_it_is) {
    const auto &a = someone(0), &b = someone(1);
    Vault v;
    newVault(v);
    // The vault knows `a` under a name of its own, and has a default of its own.
    bmc::Identity mine = bmc::Protocol::fromPrivate("Mine", bmc::ByteView(a.keys.data(), 32),
                                                    bmc::ByteView(a.keys.data() + 32, 32));
    v.addIdentity(mine);
    const auto keys = pybmfix::section(a, "Theirs") + pybmfix::section(b, "Second");
    const auto items = v.importEntries(entriesOf(keys));
    CHECK(items[0].result == Result::AlreadyThere);
    CHECK(items[1].result == Result::Added);
    CHECK_EQ(v.identities().size(), size_t(2));
    CHECK_EQ(v.identities()[0].label, std::string("Mine"));
    CHECK(v.identities()[0].isDefault);
    CHECK(!v.identities()[1].isDefault);
    // The same file again finds nothing new, changes nothing, and says so.
    const auto before = describe(v);
    const auto again = v.importEntries(entriesOf(keys));
    CHECK(again[0].result == Result::AlreadyThere && again[1].result == Result::AlreadyThere);
    CHECK_EQ(describe(v), before);
    // Twice in one file is once in the vault.
    Vault w;
    newVault(w, "w.bmvault");
    const auto twice =
        w.importEntries(entriesOf(pybmfix::section(a, "x") + pybmfix::section(a, "y")));
    CHECK_EQ(w.identities().size(), size_t(1));
    CHECK(twice[0].result == Result::Added && twice[1].result == Result::AlreadyThere);
}

TEST(keys_that_do_not_belong_to_their_address_are_refused_and_the_rest_go_in) {
    const auto &a = someone(0), &b = someone(1), &c = someone(2);
    // b's address over a's keys, a section named by something that is no address, and a good one.
    std::string keys = "[" + b.address +
                       "]\nlabel = Mixed up\nprivsigningkey = " + pybmfix::wifOf(a.keys.data()) +
                       "\nprivencryptionkey = " + pybmfix::wifOf(a.keys.data() + 32) + "\n\n";
    keys +=
        "[BM-notanaddress]\nlabel = Nonsense\nprivsigningkey = " + pybmfix::wifOf(a.keys.data()) +
        "\nprivencryptionkey = " + pybmfix::wifOf(a.keys.data() + 32) + "\n\n";
    keys += pybmfix::section(c, "Good");
    Vault v;
    newVault(v);
    const auto items = v.importEntries(entriesOf(keys));
    CHECK_EQ(items.size(), size_t(3));
    CHECK(items[0].result == Result::Unusable && items[0].why == Why::WrongKeys);
    CHECK(items[1].result == Result::Unusable && items[1].why == Why::Unsupported);
    CHECK(items[2].result == Result::Added);
    CHECK_EQ(v.identities().size(), size_t(1));
    CHECK_EQ(v.identities()[0].address, c.address);
}

TEST(an_address_of_another_stream_is_not_taken) {
    // The client works in stream 1 only; an identity of another stream is reported, not added.
    bmc::pybm::KeyEntry e;
    bmc::randomBytes(e.signing.data(), 32);
    bmc::randomBytes(e.encryption.data(), 32);
    // The address of these keys, with stream 2 written into it.
    const auto own = bmc::Protocol::fromPrivate("x", bmc::ByteView(e.signing.data(), 32),
                                                bmc::ByteView(e.encryption.data(), 32));
    ntb_address a;
    CHECK(ntb_address_decode(&a, own.address.c_str()));
    a.stream = 2;
    char encoded[NTB_ADDRESS_MAX_LENGTH + 1] = {};
    ntb_address_encode(&a, encoded);
    e.address = encoded;
    e.label = "Other stream";
    std::vector<bmc::pybm::KeyEntry> entries;
    entries.push_back(std::move(e));
    Vault v;
    newVault(v);
    const auto items = v.importEntries(entries);
    CHECK(items[0].result == Result::Unusable && items[0].why == Why::Unsupported);
    CHECK(v.identities().empty());
}

TEST(a_vault_takes_no_more_than_its_capacity_and_says_which_were_left) {
    Vault v;
    newVault(v);
    std::vector<bmc::pybm::KeyEntry> entries;
    for (int i = 0; i < 1030; ++i) {
        bmc::pybm::KeyEntry e;
        bmc::randomBytes(e.signing.data(), 32);
        bmc::randomBytes(e.encryption.data(), 32);
        const auto id = bmc::Protocol::fromPrivate("x", bmc::ByteView(e.signing.data(), 32),
                                                   bmc::ByteView(e.encryption.data(), 32));
        e.address = id.address;
        e.label = "n" + std::to_string(i);
        entries.push_back(std::move(e));
    }
    const auto items = v.importEntries(entries);
    CHECK_EQ(v.identities().size(), size_t(1024));
    int added = 0, full = 0;
    for (const auto &i : items) {
        added += i.result == Result::Added;
        full += i.result == Result::Unusable && i.why == Why::Full;
    }
    CHECK_EQ(added, 1024);
    CHECK_EQ(full, 6);
}

TEST(a_save_that_fails_adds_nothing_from_a_keys_file) {
    const auto &a = someone(0), &b = someone(1);
    const auto dir = bmc::test::tempDir();
    Vault v;
    v.create(dir + "/v.bmvault", "password");
    v.addIdentity("first");
    const auto entries = entriesOf(pybmfix::section(a, "A") + pybmfix::section(b, "B"));
    if (!makeReadOnly(dir)) {
        std::puts("  skipped: the permission bits are not enforced for this user");
        return;
    }
    const auto before = describe(v);
    CHECK_THROWS(v.importEntries(entries));
    CHECK_EQ(describe(v), before);
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all);
    CHECK_EQ(v.importEntries(entries).size(), size_t(2)); // and it works once it can save
    CHECK_EQ(v.identities().size(), size_t(3));
}

TEST(an_identity_without_a_label_is_named_by_its_address) {
    const auto &a = someone(0);
    Vault v;
    newVault(v);
    auto keys = pybmfix::section(a, "");
    const auto items = v.importEntries(entriesOf(keys));
    CHECK(items[0].result == Result::Added);
    CHECK_EQ(v.identities()[0].label, a.address);
    CHECK_EQ(items[0].label, a.address);
}

TEST(a_locked_vault_imports_nothing) {
    Vault locked;
    CHECK_THROWS(locked.importEntries(entriesOf(pybmfix::section(someone(0), "A"))));
    // Not even to say that nothing in the file can be used.
    CHECK_THROWS(
        locked.importEntries(entriesOf("[BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr]\nlabel = x\n")));
    Vault v;
    newVault(v);
    CHECK(v.importEntries({}).empty());
    CHECK(v.identities().empty());
}
