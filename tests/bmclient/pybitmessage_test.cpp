// Reading PyBitmessage's data folder: where it lives on each system, the identities of keys.dat,
// and the letters, contacts and subscriptions of messages.dat -- from files made the way
// PyBitmessage makes them (tests/bmclient/pybm_fixtures.h) and, when YNOTBIT_PYBM_SAMPLE names a
// real folder, from that.
#include "pybm_fixtures.h"
#include <bmc/error.h>
#include <bmc/hash.h>
#include <bmc/protocol.h>
#include <bmc/pybitmessage.h>
#include <bmc/test.h>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <set>
#include <utility>

namespace pybm = bmc::pybm;
using pybmfix::Folder;
using pybmfix::rawHash;

namespace {
using Platform = pybm::Environment::Platform;

std::string joined(const std::vector<std::string> &parts) {
    std::string out;
    for (const auto &p : parts)
        out += (out.empty() ? "" : "|") + p;
    return out;
}
std::string hexOf(const bmc::Secret &s) {
    return bmc::hex(bmc::ByteView(s.data(), s.size()));
}
std::string fileHash(const std::string &path) {
    return bmc::hex(bmc::hash(bmc::Digest::Sha256, bmc::test::readFile(path)));
}
const bmc::Identity &someone(int n) {
    static std::deque<bmc::Identity> made; // references to earlier ones stay valid as it grows
    while (int(made.size()) <= n)
        made.push_back(bmc::Protocol::identity("x"));
    return made[size_t(n)];
}
bool exists(const std::string &folder, const char *name) {
    return std::filesystem::exists(folder + "/" + name);
}
} // namespace

TEST(pybitmessage_folders_follow_its_rules_on_each_platform) {
    pybm::Environment mac;
    mac.platform = Platform::Mac;
    mac.home = "/Users/ann";
    CHECK_EQ(joined(pybm::candidateFolders(mac)),
             std::string("/Users/ann/Library/Application Support/PyBitmessage"));

    pybm::Environment win;
    win.platform = Platform::Windows;
    win.appData = "C:\\Users\\Ann\\AppData\\Roaming";
    CHECK_EQ(joined(pybm::candidateFolders(win)),
             std::string("C:\\Users\\Ann\\AppData\\Roaming\\PyBitmessage"));

    pybm::Environment linux_;
    linux_.platform = Platform::Linux;
    linux_.home = "/home/ann";
    // Older versions kept it in ~/.PyBitmessage; newer ones move it to the config folder.
    CHECK_EQ(joined(pybm::candidateFolders(linux_)),
             std::string("/home/ann/.config/PyBitmessage|/home/ann/.PyBitmessage"));
    linux_.xdgConfigHome = "/home/ann/cfg";
    CHECK_EQ(joined(pybm::candidateFolders(linux_)),
             std::string("/home/ann/cfg/PyBitmessage|/home/ann/.PyBitmessage"));

    // BITMESSAGE_HOME wins on every system, and its closing separator does not matter.
    linux_.bitmessageHome = "/data/bm/";
    CHECK_EQ(pybm::candidateFolders(linux_).front(), std::string("/data/bm"));
    mac.bitmessageHome = "/Volumes/x/bm";
    CHECK_EQ(pybm::candidateFolders(mac).front(), std::string("/Volumes/x/bm"));
    CHECK_EQ(pybm::candidateFolders(mac).size(), size_t(2));
    win.bitmessageHome = "D:\\bm\\";
    CHECK_EQ(pybm::candidateFolders(win).front(), std::string("D:\\bm"));

    // Nothing to go on, nothing to look in (a home folder that is not known).
    CHECK(pybm::candidateFolders(pybm::Environment{Platform::Linux, "", "", "", ""}).empty());
}

TEST(only_folders_that_hold_pybitmessage_data_are_found) {
    const auto home = bmc::test::tempDir();
    pybm::Environment env;
    env.platform = Platform::Linux;
    env.home = home;
    CHECK(pybm::findFolders(env).empty());
    // The old folder with a keys.dat, the new one with a messages.dat, and a third with neither.
    std::filesystem::create_directories(home + "/.PyBitmessage");
    std::filesystem::create_directories(home + "/.config/PyBitmessage");
    std::filesystem::create_directories(home + "/elsewhere");
    CHECK(pybm::findFolders(env).empty()); // folders alone are no data
    bmc::test::writeFile(home + "/.PyBitmessage/keys.dat", bmc::view(std::string("x")));
    bmc::test::writeFile(home + "/.config/PyBitmessage/messages.dat", bmc::view(std::string("x")));
    CHECK_EQ(joined(pybm::findFolders(env)),
             home + "/.config/PyBitmessage|" + home + "/.PyBitmessage");
    env.bitmessageHome = home + "/elsewhere"; // named by the user's environment, but empty
    CHECK_EQ(pybm::findFolders(env).size(), size_t(2));
    bmc::test::writeFile(home + "/elsewhere/keys.dat", bmc::view(std::string("x")));
    CHECK_EQ(pybm::findFolders(env).front(), home + "/elsewhere");
    CHECK_EQ(pybm::findFolders(env).size(), size_t(3));
}

#ifndef _WIN32
TEST(the_environment_of_this_process_is_read) {
    const auto keep = [](const char *name) {
        const char *v = std::getenv(name);
        return v ? std::pair<bool, std::string>{true, v} : std::pair<bool, std::string>{false, ""};
    };
    const auto home = keep("HOME"), xdg = keep("XDG_CONFIG_HOME"), bm = keep("BITMESSAGE_HOME");
    setenv("HOME", "/home/test", 1);
    setenv("XDG_CONFIG_HOME", "/home/test/cfg", 1);
    unsetenv("BITMESSAGE_HOME");
    auto env = pybm::Environment::current();
    CHECK_EQ(env.home, std::string("/home/test"));
    CHECK_EQ(env.xdgConfigHome, std::string("/home/test/cfg"));
    CHECK(env.bitmessageHome.empty());
#if defined(__APPLE__)
    CHECK(env.platform == Platform::Mac);
#else
    CHECK(env.platform == Platform::Linux);
#endif
    setenv("BITMESSAGE_HOME", "/data/bm", 1);
    CHECK_EQ(pybm::Environment::current().bitmessageHome, std::string("/data/bm"));
    const auto restore = [](const char *name, const std::pair<bool, std::string> &v) {
        if (v.first)
            setenv(name, v.second.c_str(), 1);
        else
            unsetenv(name);
    };
    restore("HOME", home);
    restore("XDG_CONFIG_HOME", xdg);
    restore("BITMESSAGE_HOME", bm);
}
#endif

TEST(a_wallet_import_key_is_decoded_and_its_checksum_checked) {
    // The "General" chan's signing key, from the previous release's fixture.
    const auto key = pybm::decodeWif("5Jnbdwc4u4DG9ipJxYLznXSvemkRFueQJNHujAQamtDDoX3N1eQ");
    CHECK_EQ(key.size(), size_t(32));
    CHECK_EQ(pybmfix::wifOf(key.data()),
             std::string("5Jnbdwc4u4DG9ipJxYLznXSvemkRFueQJNHujAQamtDDoX3N1eQ"));
    CHECK_THROWS(pybm::decodeWif("5Jnbdwc4u4DG9ipJxYLznXSvemkRFueQJNHujAQamtDDoX3N1eR")); // sum
    CHECK_THROWS(pybm::decodeWif("not a key"));
    CHECK_THROWS(pybm::decodeWif(""));
    CHECK_THROWS(pybm::decodeWif(std::string(200, '1')));
}

TEST(keys_dat_is_read_entry_by_entry) {
    const auto &a = someone(0), &b = someone(1), &c = someone(2), &d = someone(3);
    Folder f(false);
    std::string text = "\xEF\xBB\xBF" + pybmfix::kSettings + "# a comment\n; another\n\n";
    text += pybmfix::section(a, "Work");
    text += pybmfix::section(b, "100%% sure", false, false); // PyBitmessage doubles a percent sign
    text += pybmfix::section(c, "[chan] general", true);
    // A mailing list, with the extra fields PyBitmessage writes for it.
    text += pybmfix::section(d, "Club") + "mailinglist = true\nmailinglistname = The club\n";
    // A section that lacks its encryption key, and one whose checksum is wrong.
    text += "[BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr]\nlabel = Half\n"
            "privsigningkey = 5Jnbdwc4u4DG9ipJxYLznXSvemkRFueQJNHujAQamtDDoX3N1eQ\n\n";
    text += "[BM-2cUHrvWBVLWZGCADpH3QVMfCWfXKMdGUP6]\nlabel = Broken\n"
            "privsigningkey = 5Jnbdwc4u4DG9ipJxYLznXSvemkRFueQJNHujAQamtDDoX3N1eR\n"
            "privencryptionkey = 5JrDcFtQDv5ydcHRW6dfGUEvThoxCCLNEUaxQfy8LXXgTJzVAcq\n\n";
    f.keys(text);
    const auto file = pybm::readKeys(f.dir + "/keys.dat");
    CHECK(!file.encrypted);
    CHECK_EQ(file.entries.size(), size_t(6)); // [bitmessagesettings] is no identity
    CHECK_EQ(file.entries[0].address, a.address);
    CHECK_EQ(file.entries[0].label, std::string("Work"));
    CHECK(file.entries[0].enabled && !file.entries[0].chan && !file.entries[0].mailingList);
    CHECK(file.entries[0].problem == pybm::KeyProblem::None);
    CHECK_EQ(hexOf(file.entries[0].signing), bmc::hex(bmc::ByteView(a.keys.data(), 32)));
    CHECK_EQ(hexOf(file.entries[0].encryption), bmc::hex(bmc::ByteView(a.keys.data() + 32, 32)));
    CHECK_EQ(file.entries[1].label, std::string("100% sure")); // read back as typed
    CHECK(!file.entries[1].enabled);
    CHECK(file.entries[2].chan);
    CHECK_EQ(file.entries[2].label, std::string("[chan] general")); // the vault names chans
    CHECK(file.entries[3].mailingList);
    CHECK(file.entries[4].problem == pybm::KeyProblem::MissingKey); // no encryption key
    CHECK(file.entries[5].problem == pybm::KeyProblem::BadKey);     // a sum that is wrong
    CHECK_EQ(file.entries[5].label, std::string("Broken"));
    CHECK_THROWS(pybm::readKeys(f.dir + "/not-there.dat"));
}

TEST(keys_dat_in_other_dialects_reads_the_same) {
    const auto &a = someone(0);
    Folder f(false);
    // Windows line ends, a colon as the delimiter, a label with both delimiters in it, an address
    // written without its "BM-", and keys PyBitmessage's older versions wrote in any case.
    auto text = pybmfix::section(a, "Colon: and = equals");
    std::string crlf;
    for (const char ch : text)
        crlf += ch == '\n' ? std::string("\r\n") : std::string(1, ch);
    f.keys(crlf);
    auto file = pybm::readKeys(f.dir + "/keys.dat");
    CHECK_EQ(file.entries.size(), size_t(1));
    CHECK_EQ(file.entries[0].label, std::string("Colon: and = equals"));
    CHECK(file.entries[0].problem == pybm::KeyProblem::None);

    f.keys("[" + a.address.substr(3) +
           "]\nlabel: Plain\nprivsigningkey: " + pybmfix::wifOf(a.keys.data()) +
           "\nprivencryptionkey: " + pybmfix::wifOf(a.keys.data() + 32) + "\n");
    file = pybm::readKeys(f.dir + "/keys.dat");
    CHECK_EQ(file.entries.size(), size_t(1));
    CHECK_EQ(file.entries[0].address, a.address); // "BM-" put back
    CHECK_EQ(file.entries[0].label, std::string("Plain"));
    CHECK(file.entries[0].problem == pybm::KeyProblem::None);

    // A file that says its keys are encrypted is reported as such, not read as broken keys.
    f.keys("[bitmessagesettings]\nkeysencrypted = True\n");
    CHECK(pybm::readKeys(f.dir + "/keys.dat").encrypted);
}

TEST(a_messages_file_is_counted_without_reading_a_letter) {
    Folder f;
    const std::string me = "BM-me", chan = "BM-chan";
    f.inbox(rawHash(1), me, "BM-a", "s", "1767470387", "b", "inbox", false);
    f.inbox(rawHash(2), me, "BM-a", "s", "1767470388", "b", "inbox", false);
    f.inbox(rawHash(3), me, "BM-b", "s", "1767470389", "b", "inbox", true);
    f.inbox(rawHash(4), chan, "BM-c", "s", "1767470390", "b", "inbox", false);
    f.inbox(rawHash(5), pybm::kBroadcastAddress, "BM-d", "s", "1767470391", "b", "inbox", true);
    f.inbox(rawHash(6), pybm::kBroadcastAddress, "BM-d", "s", "1767470392", "b", "inbox", true);
    f.inbox(rawHash(7), me, "BM-a", "s", "1767470393", "b", "trash", true);
    f.sent(rawHash(8), "BM-a", me, "s", "b", rawHash(9), 1767470400, "msgsent");
    f.sent(rawHash(10), "BM-a", me, "s", "b", rawHash(11), 1767470401, "msgsent");
    f.sent(bmc::Bytes(16, 7), "BM-a", me, "s", "b", rawHash(12), 1767470402, "awaitingpubkey");
    f.sent(rawHash(13), "BM-a", me, "s", "b", rawHash(14), 1767470403, "ackreceived", "trash");
    f.subscription("off", "BM-sub1", false);
    f.subscription("on", "BM-sub2", true);
    f.contact("Ann", "BM-ann");
    f.contact("Bob", "BM-bob");
    f.exec("INSERT INTO blacklist VALUES ('bad','BM-bad',1)");

    pybm::MessagesFile file(f.messages);
    const auto census = file.census();
    CHECK_EQ(census.version, 11);
    const auto inbox = [&](const std::string &to, const std::string &folder, bool read) {
        int64_t n = 0;
        for (const auto &g : census.inbox)
            if (g.to == to && g.folder == folder && g.read == read)
                n += g.count;
        return n;
    };
    CHECK_EQ(inbox(me, "inbox", false), int64_t(2));
    CHECK_EQ(inbox(me, "inbox", true), int64_t(1));
    CHECK_EQ(inbox(chan, "inbox", false), int64_t(1));
    CHECK_EQ(inbox(pybm::kBroadcastAddress, "inbox", true), int64_t(2));
    CHECK_EQ(inbox(me, "trash", true), int64_t(1));
    const auto sent = [&](const std::string &status, const std::string &folder) {
        int64_t n = 0;
        for (const auto &g : census.sent)
            if (g.status == status && g.folder == folder)
                n += g.count;
        return n;
    };
    CHECK_EQ(sent("msgsent", "sent"), int64_t(2));
    CHECK_EQ(sent("awaitingpubkey", "sent"), int64_t(1));
    CHECK_EQ(sent("ackreceived", "trash"), int64_t(1));
    CHECK_EQ(census.subscriptions, int64_t(3)); // the default one, and the two
    CHECK_EQ(census.subscriptionsDisabled, int64_t(1));
    CHECK_EQ(census.contacts, int64_t(2));
    CHECK_EQ(census.blacklisted, int64_t(1));
    CHECK_EQ(census.whitelisted, int64_t(0));
}

TEST(inbox_rows_are_read_in_chunks_with_their_bytes_as_stored) {
    Folder f;
    for (int i = 0; i < 7; ++i)
        f.inbox(rawHash(i), "BM-me", "BM-from" + std::to_string(i), "subject " + std::to_string(i),
                std::to_string(1767470387 + i), "body " + std::to_string(i), "inbox", i % 2 == 1);
    pybm::MessagesFile file(f.messages);
    const auto one = file.inbox(0, 3);
    CHECK_EQ(one.size(), size_t(3));
    const auto two = file.inbox(one.back().rowid, 3);
    CHECK_EQ(two.size(), size_t(3));
    const auto three = file.inbox(two.back().rowid, 3);
    CHECK_EQ(three.size(), size_t(1));
    CHECK(file.inbox(three.back().rowid, 3).empty());
    // In rowid order, each row once.
    CHECK(one.front().rowid < one.back().rowid && one.back().rowid < two.front().rowid);
    CHECK_EQ(one[0].msgid, rawHash(0)); // 32 bytes, a NUL among them, though stored as TEXT
    CHECK_EQ(one[0].msgid.size(), size_t(32));
    CHECK_EQ(one[1].msgid, rawHash(1));
    CHECK_EQ(one[0].sighash, rawHash(200));
    CHECK_EQ(one[0].to, std::string("BM-me"));
    CHECK_EQ(one[0].from, std::string("BM-from0"));
    CHECK_EQ(one[0].subject, std::string("subject 0"));
    CHECK_EQ(one[0].body, std::string("body 0"));
    CHECK_EQ(one[0].folder, std::string("inbox"));
    CHECK_EQ(one[0].received, int64_t(1767470387));
    CHECK(one[0].receivedKnown);
    CHECK(!one[0].read && one[1].read); // read = 0 is unread
    CHECK_EQ(three[0].subject, std::string("subject 6"));
}

TEST(folders_and_empty_flags_are_read_as_stored) {
    Folder f;
    f.inbox(rawHash(1), "BM-me", "BM-a", "s", "1767470387", "b", "trash", true);
    f.inbox(rawHash(2), "BM-me", "BM-a", "s", "1767470388", "b", "Inbox",
            false); // not what PyBitmessage writes
    // A read flag that holds nothing counts as read: it is not a reason for an unread count.
    f.exec("INSERT INTO inbox VALUES "
           "(x'0a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728',"
           "'BM-me','BM-a','s','1767470389','b','inbox',2,NULL,x'')");
    pybm::MessagesFile file(f.messages);
    const auto rows = file.inbox(0, 10);
    CHECK_EQ(rows.size(), size_t(3));
    CHECK_EQ(rows[0].folder, std::string("trash"));
    CHECK_EQ(rows[1].folder, std::string("Inbox")); // the importer decides what to do with it
    CHECK(rows[2].read);
    CHECK(rows[2].sighash.empty());
}

TEST(a_database_that_is_locked_is_reported_as_busy_not_as_damaged) {
    Folder f;
    f.inbox(rawHash(1), "BM-me", "BM-a", "s", "1767470387", "b");
    f.exec("BEGIN EXCLUSIVE");
    try {
        pybm::MessagesFile file(f.messages, 50);
        bmc::test::fail(__FILE__, __LINE__, "a locked database was opened");
    } catch (const bmc::Error &e) {
        CHECK(e.code() == bmc::Code::State);
        CHECK(std::string(e.what()).find("try again") != std::string::npos);
    }
    f.exec("ROLLBACK");
    pybm::MessagesFile file(f.messages, 50); // free again
    CHECK_EQ(file.inbox(0, 10).size(), size_t(1));
    // Locked between two reads: the read says so, and works once the lock is gone.
    f.exec("BEGIN EXCLUSIVE");
    try {
        (void)file.inbox(0, 10);
        bmc::test::fail(__FILE__, __LINE__, "read from a locked database");
    } catch (const bmc::Error &e) {
        CHECK(e.code() == bmc::Code::State);
    }
    f.exec("ROLLBACK");
    CHECK_EQ(file.inbox(0, 10).size(), size_t(1));
}

#ifndef _WIN32
TEST(a_journal_left_by_a_killed_pybitmessage_is_not_replayed_by_a_look) {
    // PyBitmessage was killed in the middle of a write: the journal of the unfinished transaction
    // lies beside the file. SQLite would replay it for a reader that may write; a reader that
    // promised not to touch the file says so instead.
    Folder source;
    for (int i = 0; i < 5; ++i)
        source.inbox(rawHash(i), "BM-me", "BM-a", "s", "1767470387", "b");
    source.exec("PRAGMA synchronous=OFF"); // the journal is valid without a sync
    source.exec("BEGIN");
    source.exec("DELETE FROM inbox");
    const auto dir = bmc::test::tempDir();
    std::filesystem::copy_file(source.messages, dir + "/messages.dat");
    std::filesystem::copy_file(source.messages + "-journal", dir + "/messages.dat-journal");
    source.exec("ROLLBACK");
    const auto fileBefore = fileHash(dir + "/messages.dat");
    const auto journalBefore = fileHash(dir + "/messages.dat-journal");
    try {
        pybm::MessagesFile file(dir + "/messages.dat");
        (void)file.inbox(0, 10);
        bmc::test::fail(__FILE__, __LINE__, "a database with a hot journal was read");
    } catch (const bmc::Error &e) {
        CHECK(e.code() == bmc::Code::State);
        CHECK(std::string(e.what()).find("did not close") != std::string::npos);
    }
    CHECK(exists(dir, "messages.dat-journal")); // not replayed, not deleted
    CHECK_EQ(fileHash(dir + "/messages.dat"), fileBefore);
    CHECK_EQ(fileHash(dir + "/messages.dat-journal"), journalBefore);
}
#endif

TEST(text_that_is_not_utf8_and_dates_that_are_not_numbers_are_read_not_refused) {
    Folder f;
    f.inbox(rawHash(1), "BM-me", "BM-a", "caf\xE9", "Sat, 12 Jan 2013  08:21 AM", "x\xFF y",
            "inbox");
    f.inbox(rawHash(2), "BM-me", "BM-a", "s", "", "empty date");
    f.inbox(rawHash(3), "BM-me", "BM-a", "s", "1767470387.5", "a fraction");
    pybm::MessagesFile file(f.messages);
    const auto rows = file.inbox(0, 10);
    CHECK_EQ(rows.size(), size_t(3));
    CHECK_EQ(rows[0].subject,
             std::string("caf\xEF\xBF\xBD")); // U+FFFD, as the rest of the app reads it
    CHECK_EQ(rows[0].body, std::string("x\xEF\xBF\xBD y"));
    CHECK(!rows[0].receivedKnown);
    CHECK_EQ(rows[0].received, int64_t(0));
    CHECK(!rows[1].receivedKnown);
    CHECK(rows[2].receivedKnown);
    CHECK_EQ(rows[2].received, int64_t(1767470387));
}

TEST(sent_rows_carry_their_status_and_which_are_finished) {
    Folder f;
    f.sent(rawHash(1), "BM-a", "BM-me", "done", "b", bmc::Bytes(38, 1), 1767470899, "msgsent");
    f.sent(bmc::Bytes(16, 9), "BM-a", "BM-me", "waiting", "b", bmc::Bytes(38, 2), 1767470900,
           "awaitingpubkey");
    f.sent(rawHash(3), pybm::kBroadcastAddress, "BM-me", "news", "b", bmc::Bytes(38, 3), 1767470901,
           "broadcastsent");
    f.sent(rawHash(4), "BM-a", "BM-me", "gone", "b", bmc::Bytes(38, 4), 1767470902, "ackreceived",
           "trash");
    pybm::MessagesFile file(f.messages);
    const auto rows = file.sent(0, 10);
    CHECK_EQ(rows.size(), size_t(4));
    CHECK_EQ(rows[0].msgid, rawHash(1));
    CHECK_EQ(rows[0].ackdata, bmc::Bytes(38, 1));
    CHECK_EQ(rows[0].to, std::string("BM-a"));
    CHECK_EQ(rows[0].from, std::string("BM-me"));
    CHECK_EQ(rows[0].subject, std::string("done"));
    CHECK_EQ(rows[0].sent, int64_t(1767470899));
    CHECK_EQ(rows[0].status, std::string("msgsent"));
    CHECK_EQ(rows[0].folder, std::string("sent"));
    CHECK_EQ(rows[1].msgid.size(), size_t(16)); // a random id: not sent yet
    CHECK_EQ(rows[2].to, std::string(pybm::kBroadcastAddress));
    CHECK_EQ(rows[3].folder, std::string("trash"));
    CHECK(file.sent(rows.back().rowid, 10).empty());
    for (const char *done : {"msgsent", "msgsentnoackexpected", "broadcastsent", "ackreceived"})
        CHECK(pybm::sentState(done) == pybm::SentState::Done);
    for (const char *failed : {"toodifficult", "badkey"})
        CHECK(pybm::sentState(failed) == pybm::SentState::Failed);
    for (const char *flying :
         {"msgqueued", "broadcastqueued", "awaitingpubkey", "doingpubkeypow", "doingmsgpow",
          "doingbroadcastpow", "forcepow", "a status of tomorrow", ""})
        CHECK(pybm::sentState(flying) == pybm::SentState::InFlight); // unknown: not claimed sent
}

TEST(contacts_and_subscriptions_are_listed) {
    Folder f;
    f.contact("Ann", "BM-ann");
    f.contact("Bob", "BM-bob");
    f.subscription("Off", "BM-off", false);
    pybm::MessagesFile file(f.messages);
    const auto contacts = file.contacts();
    CHECK_EQ(contacts.size(), size_t(2));
    CHECK_EQ(contacts[0].label, std::string("Ann"));
    CHECK_EQ(contacts[0].address, std::string("BM-ann"));
    const auto subs = file.subscriptions();
    CHECK_EQ(subs.size(), size_t(2)); // the default one PyBitmessage makes, and ours
    CHECK_EQ(subs[0].address, std::string("BM-GtovgYdgs7qXPkoYaRgrLFuFKz1SFpsw"));
    CHECK(subs[0].enabled);
    CHECK_EQ(subs[1].label, std::string("Off"));
    CHECK(!subs[1].enabled);
}

TEST(a_messages_file_of_an_older_version_is_read_with_what_it_has) {
    // Before the "read", "sighash" and "folder" columns existed.
    Folder f(false);
    f.exec("CREATE TABLE inbox (msgid blob, toaddress text, fromaddress text, subject text, "
           "received text, message text, folder text, encodingtype int);"
           "CREATE TABLE sent (toaddress text, fromaddress text, subject text, message text, "
           "ackdata blob, senttime integer, status text);"
           "CREATE TABLE subscriptions (label text, address text, enabled bool);"
           "CREATE TABLE addressbook (label text, address text);"
           "CREATE TABLE settings (key blob, value blob);"
           "INSERT INTO settings VALUES('version','5');"
           "INSERT INTO inbox "
           "VALUES(x'0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20',"
           "'BM-me','BM-a','s','1700000000','b','inbox',2);"
           "INSERT INTO sent VALUES('BM-a','BM-me','s','b',x'aabb',1700000001,'msgsent');");
    pybm::MessagesFile file(f.messages);
    CHECK_EQ(file.census().version, 5);
    const auto in = file.inbox(0, 10);
    CHECK_EQ(in.size(), size_t(1));
    CHECK(in[0].read); // before "read" existed, everything was
    CHECK(in[0].sighash.empty());
    CHECK_EQ(in[0].msgid.size(), size_t(32));
    const auto out = file.sent(0, 10);
    CHECK_EQ(out.size(), size_t(1));
    CHECK_EQ(out[0].folder, std::string("sent"));
    CHECK(out[0].msgid.empty());
    CHECK(file.subscriptions().empty());
    CHECK_EQ(file.census().blacklisted, int64_t(0)); // no such table
}

TEST(a_file_that_is_no_pybitmessage_database_is_refused_with_a_sentence) {
    const auto dir = bmc::test::tempDir();
    const auto expectRefused = [&](const std::string &path) {
        try {
            pybm::MessagesFile file(path);
            bmc::test::fail(__FILE__, __LINE__, "opened " + path);
        } catch (const bmc::Error &e) {
            CHECK(std::string(e.what()).size() >
                  10); // a sentence for the dialog, not an empty text
            CHECK(e.code() == bmc::Code::Io);
        }
    };
    expectRefused(dir + "/not-there.dat");
    bmc::test::writeFile(dir + "/text.dat", bmc::view(std::string("this is not a database at all, "
                                                                  "but long enough to be read")));
    expectRefused(dir + "/text.dat");
    bmc::test::writeFile(dir + "/empty.dat", bmc::view(std::string()));
    expectRefused(dir + "/empty.dat");
    Folder other(false); // a database, but not PyBitmessage's
    other.exec("CREATE TABLE t (a)");
    expectRefused(other.messages);
    Folder partial(false); // the table is there, a column PyBitmessage always has is not
    partial.exec("CREATE TABLE inbox (msgid blob, toaddress text);"
                 "CREATE TABLE sent (toaddress text);");
    expectRefused(partial.messages);
}

TEST(reading_never_writes_to_the_data_and_never_holds_it) {
    Folder f;
    for (int i = 0; i < 5; ++i)
        f.inbox(rawHash(i), "BM-me", "BM-a", "s", "1767470387", "b");
    f.sent(rawHash(9), "BM-a", "BM-me", "s", "b", rawHash(10), 1767470400, "msgsent");
    // Reading changes not one byte of the file and leaves no journal or lock file beside it.
    const auto before = fileHash(f.messages);
    {
        pybm::MessagesFile file(f.messages);
        (void)file.census();
        CHECK_EQ(file.inbox(0, 10).size(), size_t(5));
        CHECK_EQ(file.sent(0, 10).size(), size_t(1));
        (void)file.subscriptions();
        (void)file.contacts();
    }
    CHECK_EQ(fileHash(f.messages), before);
    CHECK(!exists(f.dir, "messages.dat-journal"));
    CHECK(!exists(f.dir, "messages.dat-wal"));
    // PyBitmessage writes while we read. Between two calls nothing of ours is held, so its write
    // goes through at once (this connection has no busy timeout, so a held lock would fail it).
    {
        pybm::MessagesFile file(f.messages);
        const auto first = file.inbox(0, 2);
        f.exec("BEGIN IMMEDIATE; INSERT INTO subscriptions VALUES ('x','BM-x',1); COMMIT;");
        (void)file.sent(0, 10);
        (void)file.census();
        f.exec("BEGIN IMMEDIATE; INSERT INTO subscriptions VALUES ('y','BM-y',1); COMMIT;");
        CHECK_EQ(file.inbox(first.back().rowid, 10).size(), size_t(3));
        (void)file.subscriptions();
        f.exec("BEGIN IMMEDIATE; INSERT INTO subscriptions VALUES ('z','BM-z',1); COMMIT;");
    }
    // A file the user cannot write is read all the same.
    sqlite3_close(f.db);
    f.db = nullptr;
    const auto readOnlyHash = fileHash(f.messages);
    std::filesystem::permissions(f.messages, std::filesystem::perms::owner_read);
    {
        pybm::MessagesFile file(f.messages);
        CHECK_EQ(file.inbox(0, 10).size(), size_t(5));
    }
    CHECK_EQ(fileHash(f.messages), readOnlyHash);
    std::filesystem::permissions(f.messages, std::filesystem::perms::owner_all);
}

TEST(a_real_pybitmessage_folder_is_read_whole) {
    const char *dir = std::getenv("YNOTBIT_PYBM_SAMPLE");
    if (!dir || !*dir) {
        std::puts("  skipped: YNOTBIT_PYBM_SAMPLE does not name a PyBitmessage folder");
        return;
    }
    const std::string folder = dir;
    pybm::MessagesFile file(folder + "/messages.dat");
    const auto census = file.census();
    CHECK_EQ(census.version, 11);
    // The census against the database's own counts.
    sqlite3 *db = nullptr;
    CHECK(sqlite3_open_v2((folder + "/messages.dat").c_str(), &db, SQLITE_OPEN_READONLY, nullptr) ==
          SQLITE_OK);
    const auto count = [&](const char *sql) {
        sqlite3_stmt *s = nullptr;
        sqlite3_prepare_v2(db, sql, -1, &s, nullptr);
        sqlite3_step(s);
        const int64_t n = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
        return n;
    };
    int64_t inboxRows = 0;
    for (const auto &g : census.inbox)
        inboxRows += g.count;
    CHECK_EQ(inboxRows, count("SELECT count(*) FROM inbox"));
    int64_t sentRows = 0;
    for (const auto &g : census.sent)
        sentRows += g.count;
    CHECK_EQ(sentRows, count("SELECT count(*) FROM sent"));
    CHECK_EQ(census.subscriptions, count("SELECT count(*) FROM subscriptions"));
    CHECK_EQ(census.contacts, count("SELECT count(*) FROM addressbook"));
    sqlite3_close(db);
    // Every row comes out, once, whole.
    std::set<std::string> seen;
    int64_t after = 0, rows = 0;
    for (;;) {
        const auto chunk = file.inbox(after, 100);
        if (chunk.empty())
            break;
        for (const auto &r : chunk) {
            ++rows;
            CHECK_EQ(r.msgid.size(), size_t(32));
            CHECK(r.receivedKnown && r.received > 1000000000);
            CHECK(!r.to.empty() && !r.from.empty());
            seen.insert(bmc::hex(r.msgid));
        }
        after = chunk.back().rowid;
    }
    CHECK_EQ(rows, inboxRows);
    CHECK_EQ(int64_t(seen.size()), inboxRows); // msgid is unique in the table
    after = 0;
    int64_t sentRead = 0;
    for (;;) {
        const auto chunk = file.sent(after, 100);
        if (chunk.empty())
            break;
        sentRead += int64_t(chunk.size());
        after = chunk.back().rowid;
    }
    CHECK_EQ(sentRead, sentRows);
    // keys.dat: every entry has usable keys and its own address.
    const auto keys = pybm::readKeys(folder + "/keys.dat");
    CHECK(!keys.encrypted);
    CHECK(!keys.entries.empty());
    std::set<std::string> addresses;
    for (const auto &e : keys.entries) {
        CHECK(e.problem == pybm::KeyProblem::None);
        CHECK(e.address.rfind("BM-", 0) == 0);
        addresses.insert(e.address);
    }
    CHECK_EQ(addresses.size(), keys.entries.size());
}
