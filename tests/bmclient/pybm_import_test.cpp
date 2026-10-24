// The import of a PyBitmessage folder into a vault and a mailbox: what a folder holds, what becomes
// of each row, that running it twice changes nothing, and that it goes in slices, can be stopped,
// and waits for a PyBitmessage that has its data locked.
#include "fake_node.h"
#include "pybm_fixtures.h"
#include <algorithm>
#include <bmc/cache.h>
#include <bmc/chat_format.h>
#include <bmc/clock.h>
#include <bmc/delivery.h>
#include <bmc/error.h>
#include <bmc/mailbox.h>
#include <bmc/protocol.h>
#include <bmc/pybm_import.h>
#include <bmc/random.h>
#include <bmc/test.h>
#include <bmc/vault.h>
#include <bmc/wire.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <sqlcipher/sqlite3.h>
#include <utility>

namespace pybm = bmc::pybm;
using bmc::IdentityImport;
using pybmfix::rawHash;
using Phase = pybm::Progress::Phase;

namespace {
bmc::Secret key32() {
    bmc::Secret key(32);
    for (int i = 0; i < 32; ++i)
        key.data()[i] = 0x02;
    return key;
}
std::string idOf(int seed) {
    return bmc::hex(rawHash(seed));
}
std::string ackId(uint8_t fill) {
    return "pybm-" + bmc::hex(bmc::Bytes(38, fill));
}
bool near(double a, double b, double eps = 0.01) {
    return std::fabs(a - b) <= eps;
}

// A PyBitmessage folder with a bit of everything, and the target a user would import it into.
struct Case {
    pybmfix::Folder source;
    bmc::Identity me = bmc::Protocol::identity("me"), chan = bmc::Protocol::identity("chan"),
                  old = bmc::Protocol::identity("old");
    bmc::Vault vault;
    bmc::Mailbox mailbox;
    std::string mailPath;

    explicit Case(bool keys = true) {
        if (keys)
            source.keys(pybmfix::kSettings + pybmfix::section(me, "Me") +
                        pybmfix::section(chan, "[chan] general", true) +
                        pybmfix::section(old, "Old one", false, false));
        fill();
        vault.create(bmc::test::tempDir() + "/v.bmvault", "password");
        vault.addIdentity("Mine");
        mailPath = bmc::test::tempDir() + "/m.bmmail";
        mailbox.create(mailPath, "key-1", key32());
    }
    // 12 rows of the inbox, 5 of sent, 3 subscriptions (one off, one PyBitmessage's own), 2
    // contacts.
    void fill() {
        auto &f = source;
        const auto mine = me.address, room = chan.address;
        const std::string broadcast = pybm::kBroadcastAddress;
        f.inbox(rawHash(1), mine, "BM-aaa", "d1", "1700000100", "body of d1", "inbox", false);
        f.inbox(rawHash(2), mine, "BM-bbb", "d2", "1700000200", "body of d2", "inbox", false);
        f.inbox(rawHash(3), mine, "BM-bbb", "d3", "1700000300", "body of d3", "inbox", true);
        f.inbox(rawHash(4), room, "BM-ccc", "c1", "1700000400", "body of c1", "inbox", false);
        f.inbox(rawHash(5), room, "BM-ccc", "c2", "1700000500", "body of c2", "inbox", true);
        f.inbox(rawHash(6), broadcast, "BM-ddd", "b1", "1700000600", "body of b1", "inbox", false);
        f.inbox(rawHash(7), broadcast, "BM-ddd", "b2", "1700000700", "body of b2", "inbox", true);
        f.inbox(rawHash(8), mine, "BM-aaa", "t1", "1700000800", "body of t1", "trash", true);
        f.inbox(rawHash(9), room, "BM-ccc", "t2", "1700000900", "body of t2", "trash", false);
        f.inbox(rawHash(10), mine, "BM-aaa", "odd", "1700000950", "never listed", "spam", false);
        f.inbox(rawHash(11), mine, "BM-aaa", "nd", "yesterday", "body of nd", "inbox", false);
        f.inbox(bmc::Bytes(5, 7), mine, "BM-aaa", "short", "1700001000", "body of short", "inbox",
                true);
        f.sent(rawHash(21), "BM-aaa", mine, "s1", "body of s1", bmc::Bytes(38, 1), 1700001100,
               "msgsent");
        f.sent(rawHash(22), broadcast, mine, "s2", "body of s2", bmc::Bytes(38, 2), 1700001200,
               "broadcastsent");
        f.sent(bmc::Bytes(16, 3), "BM-aaa", mine, "u1", "body of u1", bmc::Bytes(38, 3), 1700001300,
               "awaitingpubkey");
        f.sent(bmc::Bytes(16, 4), "BM-bbb", mine, "u2", "body of u2", bmc::Bytes(38, 4), 1700001400,
               "toodifficult");
        f.sent(rawHash(25), "BM-aaa", mine, "s3", "body of s3", bmc::Bytes(38, 5), 1700001500,
               "ackreceived", "trash");
        f.subscription("News", "BM-news", true);
        f.subscription("Off", "BM-off", false);
        f.contact("Ann", "BM-ann");
        f.contact("", "BM-nameless");
    }
    pybm::Options options() const {
        pybm::Options o;
        o.folder = source.dir;
        return o;
    }
    pybm::Report run(pybm::Options o) {
        pybm::Importer importer(std::move(o), mailbox, vault);
        int guard = 0;
        while (importer.step(1000) && ++guard < 10000) {
        }
        return importer.report();
    }
    pybm::Report run() {
        return run(options());
    }
};

// Runs SQL on a mailbox file through a connection of its own, to put it in a state the public API
// would never make.
void editMailbox(const std::string &path, const bmc::Secret &key, const std::string &sql) {
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
        bmc::test::fail(__FILE__, __LINE__, "cannot open the mailbox file");
    const bool ran = sqlite3_key(db, key.data(), 32) == SQLITE_OK &&
                     sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
    const std::string why = ran ? std::string() : sqlite3_errmsg(db);
    sqlite3_close(db);
    if (!ran)
        bmc::test::fail(__FILE__, __LINE__, why);
}

// keys.dat sections as PyBitmessage writes them, read and put into a vault as a chan.
void joinAsChan(bmc::Vault &vault, const bmc::Identity &chan) {
    const auto path = bmc::test::tempDir() + "/chan-keys.dat";
    bmc::test::writeFile(path, bmc::view(pybmfix::section(chan, "[chan] general", true)));
    vault.importEntries(pybm::readKeys(path).entries);
}
} // namespace

TEST(a_preview_counts_what_a_folder_holds) {
    Case c;
    const auto p = pybm::preview(c.source.dir);
    CHECK(p.trouble == pybm::Trouble::None);
    CHECK(p.hasKeys && p.hasMessages && !p.keysEncrypted);
    CHECK_EQ(p.version, 11);
    CHECK_EQ(p.identities, int64_t(3));
    CHECK_EQ(p.chans, int64_t(1));
    CHECK_EQ(p.disabled, int64_t(1));
    CHECK_EQ(p.unusable, int64_t(0));
    CHECK_EQ(p.contacts, int64_t(2));
    CHECK_EQ(p.subscriptions, int64_t(3)); // PyBitmessage's own, and two
    CHECK_EQ(p.subscriptionsDisabled, int64_t(1));
    CHECK_EQ(p.inbox, int64_t(5)); // d1 d2 d3 nd short
    CHECK_EQ(p.inboxUnread, int64_t(3));
    CHECK_EQ(p.channelLetters, int64_t(2));
    CHECK_EQ(p.channelUnread, int64_t(1));
    CHECK_EQ(p.broadcastLetters, int64_t(2));
    CHECK_EQ(p.broadcastUnread, int64_t(1));
    CHECK_EQ(p.trashed, int64_t(2));
    CHECK_EQ(p.unlisted, int64_t(1));
    CHECK_EQ(p.sent, int64_t(2));
    CHECK_EQ(p.unsent, int64_t(2));
    CHECK_EQ(p.sentTrashed, int64_t(1));
    CHECK_EQ(p.letters(), int64_t(16));
    CHECK(!p.running);
    // Without keys.dat a chan is known by the vault, or not at all.
    std::filesystem::remove(c.source.dir + "/keys.dat");
    const auto unknown = pybm::preview(c.source.dir, &c.vault);
    CHECK(!unknown.hasKeys);
    CHECK_EQ(unknown.identities, int64_t(0));
    CHECK_EQ(unknown.channelLetters, int64_t(0));
    CHECK_EQ(unknown.inbox, int64_t(7)); // the chan's posts look like letters to an identity
    joinAsChan(c.vault, c.chan);
    const auto known = pybm::preview(c.source.dir, &c.vault);
    CHECK_EQ(known.channelLetters, int64_t(2));
    CHECK_EQ(known.inbox, int64_t(5));
    // A lock file says that PyBitmessage looks to be running.
    bmc::test::writeFile(c.source.dir + "/singleton.lock", bmc::view(std::string("1234")));
    CHECK(pybm::preview(c.source.dir).running);
}

TEST(a_folder_that_cannot_be_imported_from_says_why) {
    const auto empty = bmc::test::tempDir();
    auto p = pybm::preview(empty);
    CHECK(p.trouble == pybm::Trouble::NothingThere);
    CHECK(!p.hasKeys && !p.hasMessages);
    p = pybm::preview(empty + "/not-there");
    CHECK(p.trouble == pybm::Trouble::NothingThere);
    // A messages.dat that is no database.
    bmc::test::writeFile(empty + "/messages.dat",
                         bmc::view(std::string("this is not a database at all, but long enough")));
    p = pybm::preview(empty);
    CHECK(p.trouble == pybm::Trouble::Unreadable);
    CHECK(!p.detail.empty());
    // A database that is locked, and nobody to wait for it in a preview.
    Case locked;
    locked.source.exec("BEGIN EXCLUSIVE");
    p = pybm::preview(locked.source.dir);
    CHECK(p.trouble == pybm::Trouble::Busy);
    locked.source.exec("ROLLBACK");
    // Keys that say they are encrypted: no identity, and the letters are still counted.
    Case c;
    c.source.keys("[bitmessagesettings]\nkeysencrypted = True\n"
                  "[BM-2DAV89w336ovy6BUJnfVRD5B9qipFbRgmr]\nlabel = x\n");
    p = pybm::preview(c.source.dir);
    CHECK(p.keysEncrypted);
    CHECK_EQ(p.identities, int64_t(0));
    CHECK_EQ(p.inbox, int64_t(7));
}

TEST(an_import_puts_every_row_where_it_belongs) {
    Case c;
    c.mailbox.advance(9);
    const auto report = c.run();
    CHECK(report.finished && !report.cancelled && report.trouble == pybm::Trouble::None);
    CHECK_EQ(report.inbox.added, int64_t(5));
    CHECK_EQ(report.channels.added, int64_t(2));
    CHECK_EQ(report.broadcasts.added, int64_t(2));
    CHECK_EQ(report.trash.added, int64_t(3));
    CHECK_EQ(report.sent.added, int64_t(2));
    CHECK_EQ(report.drafts.added, int64_t(2));
    CHECK_EQ(report.lettersAdded(), int64_t(16));
    CHECK_EQ(report.letterOfUnknownFolder, int64_t(1));
    CHECK_EQ(report.withoutDate, int64_t(1));
    CHECK_EQ(report.contacts.added, int64_t(1));
    CHECK_EQ(report.contacts.skipped, int64_t(1)); // a contact needs a name
    CHECK_EQ(report.subscriptions.added, int64_t(3));
    CHECK_EQ(report.identities.size(), size_t(3));
    for (const auto &i : report.identities)
        CHECK(i.result == IdentityImport::Result::Added);
    CHECK(report.identities[2].wasDisabled); // and it came in all the same

    auto &m = c.mailbox;
    // A direct letter: both addresses, the text, its own date, unread as it was.
    const auto d1 = m.message(idOf(1));
    CHECK_EQ(d1.folder, std::string("Inbox"));
    CHECK_EQ(d1.from, std::string("BM-aaa"));
    CHECK_EQ(d1.to, c.me.address);
    CHECK_EQ(d1.subject, std::string("d1"));
    CHECK_EQ(d1.body, std::string("body of d1"));
    CHECK_EQ(d1.received, int64_t(1700000100));
    CHECK(m.unread(idOf(1)) && m.unread(idOf(2)) && !m.unread(idOf(3)));
    // A chan's post goes to Channels under the chan's address, a broadcast to Broadcasts under its
    // sender.
    CHECK_EQ(m.message(idOf(4)).folder, std::string("Channels"));
    CHECK_EQ(m.message(idOf(4)).to, c.chan.address);
    CHECK_EQ(m.message(idOf(6)).folder, std::string("Broadcasts"));
    CHECK_EQ(m.message(idOf(6)).to, std::string("BM-ddd"));
    CHECK_EQ(m.message(idOf(6)).from, std::string("BM-ddd"));
    // What is counted unread is what was unread over there, in the folders that receive mail.
    const auto unread = m.unreadByFolder();
    CHECK_EQ(unread.at("Inbox"), 3); // d1 d2 nd
    CHECK_EQ(unread.at("Channels"), 1);
    CHECK_EQ(unread.at("Broadcasts"), 1);
    // Trash keeps where each came from.
    CHECK_EQ(m.message(idOf(8)).folder, std::string("Trash"));
    m.restoreMessage(idOf(8));
    CHECK_EQ(m.message(idOf(8)).folder, std::string("Inbox"));
    m.restoreMessage(idOf(9));
    CHECK_EQ(m.message(idOf(9)).folder, std::string("Channels"));
    // A letter with no date is dated 1970 and counted; one with a short id gets an id of its own;
    // one in a folder that PyBitmessage itself never lists is not brought in.
    bool shortFound = false, noDate = false;
    for (const auto &message : m.messages()) {
        if (message.subject == "short") {
            shortFound = true;
            CHECK(message.hash.rfind("pybm-", 0) == 0);
        }
        if (message.subject == "nd") {
            noDate = true;
            CHECK_EQ(message.received, int64_t(0));
        }
        CHECK(message.subject != "odd");
    }
    CHECK(shortFound && noDate);
    // Sent letters and drafts: the sender is ours; a broadcast has no recipient and is one.
    const auto s1 = m.message(ackId(1));
    CHECK_EQ(s1.folder, std::string("Sent"));
    CHECK_EQ(s1.from, c.me.address);
    CHECK_EQ(s1.to, std::string("BM-aaa"));
    CHECK_EQ(s1.received, int64_t(1700001100));
    CHECK_EQ(m.message(ackId(2)).to, std::string());
    CHECK_EQ(m.setting("draftkind:" + ackId(2), "direct"), std::string("broadcast"));
    CHECK_EQ(m.message(ackId(3)).folder, std::string("Drafts")); // waiting for a key: not sent
    CHECK_EQ(m.message(ackId(4)).folder, std::string("Drafts")); // too difficult: not sent
    CHECK_EQ(m.message(ackId(5)).folder, std::string("Trash"));
    m.restoreMessage(ackId(5));
    CHECK_EQ(m.message(ackId(5)).folder, std::string("Sent"));
    CHECK(m.outbox().empty()); // nothing is queued to be sent
    // Contacts, subscriptions, identities.
    CHECK_EQ(m.contacts().size(), size_t(1));
    CHECK_EQ(m.contacts()[0].label, std::string("Ann"));
    CHECK_EQ(m.subscriptions().size(), size_t(3));
    CHECK_EQ(c.vault.identities().size(), size_t(4));
    CHECK_EQ(c.vault.identities()[2].label, std::string("#general"));
    CHECK(c.vault.identities()[2].chan);
    // No arrival among them, and the scan starts over for the identities that came in.
    CHECK_EQ(m.incomingLetterCount(), uint64_t(0));
    CHECK_EQ(m.checkpoint(), int64_t(0));
}

TEST(running_an_import_again_adds_nothing) {
    Case c;
    c.run();
    const auto before = c.mailbox.messages().size();
    c.mailbox.advance(9);
    const auto again = c.run();
    CHECK_EQ(c.mailbox.checkpoint(), int64_t(9)); // no identity came in, so the scan stays
    CHECK(again.finished);
    CHECK_EQ(again.lettersAdded(), int64_t(0));
    CHECK_EQ(again.inbox.present, int64_t(5));
    CHECK_EQ(again.channels.present, int64_t(2));
    CHECK_EQ(again.broadcasts.present, int64_t(2));
    CHECK_EQ(again.trash.present, int64_t(3));
    CHECK_EQ(again.sent.present, int64_t(2));
    CHECK_EQ(again.drafts.present, int64_t(2));
    CHECK_EQ(again.contacts.added, int64_t(0));
    CHECK_EQ(again.contacts.present, int64_t(1));
    CHECK_EQ(again.subscriptions.present, int64_t(3));
    for (const auto &i : again.identities)
        CHECK(i.result == IdentityImport::Result::AlreadyThere);
    CHECK_EQ(c.mailbox.messages().size(), before);
    CHECK_EQ(c.vault.identities().size(), size_t(4));
}

TEST(a_letter_this_mailbox_has_already_is_not_brought_in_again) {
    Case c;
    // The scan stored d1 under an id of its own (a letter with an acknowledgment is keyed by what
    // it says), and c1 under the hash of its object.
    c.mailbox.store("keyed-by-what-it-says", "BM-aaa", c.me.address, "d1", "body of d1", -1);
    c.mailbox.store(idOf(4), "BM-ccc", c.chan.address, "c1", "body of c1", -1, "Channels");
    // A letter that merely looks like d2 is another letter.
    c.mailbox.store("another", "BM-bbb", c.me.address, "d2", "a different body", -1);
    // d3 was thrown away here already, and an import does not bring it back.
    c.mailbox.store("thrown-away", "BM-bbb", c.me.address, "d3", "body of d3", -1, "Trash");
    const auto report = c.run();
    CHECK_EQ(report.inbox.present, int64_t(2)); // d1 d3
    CHECK_EQ(report.inbox.added, int64_t(3));   // d2 nd short
    CHECK_EQ(report.channels.present, int64_t(1));
    CHECK_EQ(report.channels.added, int64_t(1));
    bool d1Twice = false;
    int d1 = 0;
    for (const auto &message : c.mailbox.messages())
        d1 += message.subject == "d1";
    d1Twice = d1 > 1;
    CHECK(!d1Twice);
    CHECK_THROWS(c.mailbox.message(idOf(1))); // d1 stayed what it was
}

TEST(options_leave_out_what_was_not_asked_for) {
    Case c;
    c.mailbox.advance(9);
    auto o = c.options();
    o.trash = o.sent = o.identities = o.contacts = o.subscriptions = false;
    const auto report = c.run(o);
    CHECK(report.finished);
    CHECK_EQ(report.inbox.added + report.channels.added + report.broadcasts.added, int64_t(9));
    CHECK_EQ(report.trash.added + report.sent.added + report.drafts.added, int64_t(0));
    CHECK_EQ(report.left, int64_t(7)); // two in Trash, five sent
    CHECK_EQ(report.letterOfUnknownFolder, int64_t(1));
    CHECK(report.identities.empty());
    CHECK_EQ(c.vault.identities().size(), size_t(1));
    CHECK_EQ(c.mailbox.checkpoint(), int64_t(9)); // no identity came in, the scan stays
    CHECK(c.mailbox.contacts().empty());
    CHECK(c.mailbox.subscriptions().empty());
    // Asking for the rest later brings the rest.
    pybm::Options rest = c.options();
    rest.inbox = false;
    const auto later = c.run(rest);
    CHECK_EQ(later.trash.added, int64_t(3));
    CHECK_EQ(later.sent.added, int64_t(2));
    CHECK_EQ(later.drafts.added, int64_t(2));
    CHECK_EQ(c.vault.identities().size(), size_t(4));
}

TEST(an_import_goes_in_slices_and_a_cancelled_one_can_be_finished) {
    Case c;
    auto o = c.options();
    o.chunk = 3;
    pybm::Importer importer(o, c.mailbox, c.vault);
    CHECK(importer.progress().phase == Phase::Starting);
    int steps = 0;
    int64_t last = -1;
    while (importer.step(0)) { // no budget: one piece of work per step
        ++steps;
        CHECK(importer.progress().done >= last);
        last = importer.progress().done;
        CHECK_EQ(importer.progress().total, int64_t(17)); // 12 rows of the inbox and 5 of sent
        if (steps == 6)
            importer.cancel();
    }
    CHECK(steps >= 6 && steps < 12); // it went in pieces, and stopped when told to
    const auto first = importer.report();
    CHECK(first.cancelled && !first.finished);
    CHECK(importer.progress().phase != Phase::Done);
    CHECK(first.identities.empty()); // the identities come last
    CHECK_EQ(c.vault.identities().size(), size_t(1));
    CHECK(first.lettersAdded() > 0 && first.lettersAdded() < 16); // what was stored stays
    CHECK(!importer.step(0));                                     // and it stays stopped

    // Running it again carries on, and ends with exactly what one run gives.
    const auto second = c.run();
    CHECK(second.finished);
    CHECK_EQ(first.lettersAdded() + second.lettersAdded(), int64_t(16));
    CHECK_EQ(c.mailbox.messages().size(), size_t(16));
    CHECK_EQ(c.vault.identities().size(), size_t(4));
}

TEST(a_pybitmessage_that_has_its_data_locked_is_waited_for) {
    Case c;
    c.source.exec("BEGIN EXCLUSIVE");
    pybm::Importer importer(c.options(), c.mailbox, c.vault);
    CHECK(importer.step(0)); // nothing could be read, and it is not over
    CHECK(importer.progress().waiting);
    CHECK_EQ(c.mailbox.messages().size(), size_t(0));
    c.source.exec("ROLLBACK");
    int guard = 0;
    while (importer.step(1000) && ++guard < 1000) {
    }
    CHECK(importer.report().finished);
    CHECK(!importer.progress().waiting);
    CHECK_EQ(importer.report().lettersAdded(), int64_t(16));
    // And patience has an end.
    Case d;
    d.source.exec("BEGIN EXCLUSIVE");
    auto o = d.options();
    o.patienceMs = 100;
    pybm::Importer impatient(o, d.mailbox, d.vault);
    int tries = 0;
    while (impatient.step(0) && ++tries < 100) {
    }
    CHECK(tries < 100);
    CHECK(impatient.report().trouble == pybm::Trouble::Busy);
    CHECK(!impatient.report().finished);
    d.source.exec("ROLLBACK");
}

TEST(a_mailbox_that_cannot_be_written_stops_the_import_and_keeps_what_was_stored) {
    Case c;
    // The database refuses the letter of the sixth seed, as a full disk or a damaged page would.
    editMailbox(c.mailPath, key32(),
                "CREATE TRIGGER refuse BEFORE INSERT ON messages WHEN NEW.hash='" + idOf(6) +
                    "' BEGIN SELECT RAISE(ABORT, 'refused'); END");
    auto o = c.options();
    o.chunk = 3;
    const auto report = c.run(o);
    CHECK(!report.finished && !report.cancelled);
    CHECK(report.trouble == pybm::Trouble::Failed);
    CHECK(!report.detail.empty());
    CHECK_EQ(report.lettersAdded(), int64_t(3)); // the chunk before it is stored, whole
    CHECK_EQ(int64_t(c.mailbox.messages().size()), int64_t(3));
    CHECK(report.identities.empty()); // the identities come last, so none yet
    CHECK_EQ(c.vault.identities().size(), size_t(1));
}

TEST(a_folder_with_only_keys_or_only_letters_is_imported_as_far_as_it_goes) {
    // keys.dat alone.
    Case c;
    c.source.close();
    std::filesystem::remove(c.source.dir + "/messages.dat");
    const auto onlyKeys = c.run();
    CHECK(onlyKeys.finished);
    CHECK_EQ(onlyKeys.identities.size(), size_t(3));
    CHECK_EQ(onlyKeys.lettersAdded(), int64_t(0));
    CHECK_EQ(c.vault.identities().size(), size_t(4));
    // messages.dat alone, the chan already being the vault's.
    Case d(false);
    joinAsChan(d.vault, d.chan);
    const auto onlyLetters = d.run();
    CHECK(onlyLetters.finished);
    CHECK(onlyLetters.identities.empty());
    CHECK_EQ(onlyLetters.channels.added, int64_t(2));
    CHECK_EQ(onlyLetters.inbox.added, int64_t(5));
    // Keys that are encrypted are said to be, and the letters come in.
    Case e;
    e.source.keys("[bitmessagesettings]\nkeysencrypted = True\n");
    const auto encrypted = e.run();
    CHECK(encrypted.finished && encrypted.keysEncrypted);
    CHECK(encrypted.identities.empty());
    CHECK_EQ(encrypted.lettersAdded(), int64_t(16));
    // Nothing there at all is a trouble, not a success.
    Case f;
    f.source.close();
    std::filesystem::remove(f.source.dir + "/messages.dat");
    std::filesystem::remove(f.source.dir + "/keys.dat");
    const auto nothing = f.run();
    CHECK(!nothing.finished);
    CHECK(nothing.trouble == pybm::Trouble::NothingThere);
}

#ifndef _WIN32
TEST(keys_that_cannot_be_read_leave_the_letters_to_come_in) {
    Case c;
    const auto keys = c.source.dir + "/keys.dat";
    std::filesystem::permissions(keys, std::filesystem::perms::none);
    // Root is not bound by the permission bits (a container runs as root): then there is no test.
    if (std::FILE *probe = std::fopen(keys.c_str(), "rb")) {
        std::fclose(probe);
        std::puts("  skipped: the permission bits are not enforced for this user");
        std::filesystem::permissions(keys, std::filesystem::perms::owner_all);
        return;
    }
    const auto p = pybm::preview(c.source.dir);
    CHECK(p.keysUnreadable);
    CHECK_EQ(p.identities, int64_t(0));
    CHECK(p.trouble == pybm::Trouble::None); // the letters can still come
    const auto report = c.run();
    CHECK(report.finished && report.keysUnreadable && report.identities.empty());
    CHECK_EQ(report.inbox.added, int64_t(7)); // the chan's posts look like letters without its keys
    CHECK_EQ(c.vault.identities().size(), size_t(1));
    std::filesystem::permissions(keys, std::filesystem::perms::owner_all);
}
#endif

TEST(the_scan_after_an_import_stores_nothing_the_import_brought) {
    Case c;
    // A letter from the network with an acknowledgment, to an identity of keys.dat, and the row
    // PyBitmessage made of it: its inbox holds the object's hash.
    const auto alice = bmc::Protocol::identity("alice");
    bmc::Bytes token(32);
    bmc::randomBytes(token.data(), token.size());
    const int64_t now = bmc::now();
    const auto object = bmc::Wire::encodeMessage(alice, bmc::Wire::publicIdentity(c.me), "Hello",
                                                 "From the network", now + 3600,
                                                 bmc::Wire::acknowledgment(token, now + 3600));
    const auto root = bmc::test::tempDir();
    fake::cacheObject(root, object);
    c.source.inbox(bmc::unhex(bmc::Protocol::inventoryHash(object)), c.me.address, alice.address,
                   "Hello", "1700000000", "From the network", "inbox", true);
    c.run();
    auto hello = [&] {
        int n = 0;
        for (const auto &m : c.mailbox.messages())
            n += m.subject == "Hello";
        return n;
    };
    CHECK_EQ(hello(), 1);
    // The identities came in, so the scan reads everything the node holds once more.
    bmc::Cache cache(root);
    bmc::Delivery delivery(root);
    c.mailbox.bindCache(cache.id());
    c.mailbox.advance(0);
    delivery.scan(cache, c.mailbox, c.vault, now);
    CHECK_EQ(c.mailbox.checkpoint(), int64_t(1)); // it did read the object
    CHECK_EQ(hello(), 1);                         // and found nothing new in it
    CHECK_EQ(c.mailbox.incomingLetterCount(), uint64_t(0));
    for (const auto &m : c.mailbox.messages())
        if (m.subject == "Hello") {
            CHECK_EQ(m.received, int64_t(1700000000)); // the imported one, dated as it was
            CHECK(!c.mailbox.unread(m.hash));
        }
}

// The chat messages of this app are letters with no subject whose text is a small JSON document.
// Used with another client on the same identity, they pile up in its inbox as that document.
static std::string talk(int64_t seqno, const std::string &text) {
    return bmc::chatMessageBody(seqno, text);
}

TEST(a_chat_in_the_inbox_becomes_a_chat_message) {
    Case c;
    const auto me = c.me.address;
    auto &f = c.source;
    f.inbox(rawHash(40), me, "BM-peer1", "", "1700002000", talk(1000, "Hello **you**"), "inbox",
            false);
    f.inbox(rawHash(41), me, "BM-peer1", "", "1700002100", talk(2000, "Second"), "inbox", true);
    f.inbox(rawHash(42), me, "BM-peer2", "", "1700002200", talk(1000, "Another peer"), "inbox",
            false);
    // What only looks like a chat is a letter: it has a subject, or its text is not ours, or it is
    // a version this one cannot read, or it says nobody sent it.
    f.inbox(rawHash(43), me, "BM-peer1", "Re: hi", "1700002300", talk(3000, "a subject"), "inbox",
            true);
    f.inbox(rawHash(44), me, "BM-peer1", "", "1700002400", R"({"hello":"world"})", "inbox", true);
    f.inbox(rawHash(45), me, "BM-peer1", "", "1700002500",
            R"({"app":"ynotbit","body":"x","format":"markdown","seqno":4000,"type":"chat",)"
            R"("version":2})",
            "inbox", true);
    f.inbox(rawHash(46), me, "", "", "1700002600", talk(5000, "from nobody"), "inbox", true);
    // Only a direct letter can be a chat, as when it arrives: a post to a chan, or a broadcast,
    // with this text stays a post.
    f.inbox(rawHash(47), c.chan.address, "BM-ccc", "", "1700002700", talk(6000, "to the chan"),
            "inbox", true);
    f.inbox(rawHash(48), pybm::kBroadcastAddress, "BM-ddd", "", "1700002800",
            talk(7000, "broadcast"), "inbox", true);
    // A chat deleted over there is not brought back: a chat has no folder to be deleted into.
    f.inbox(rawHash(49), me, "BM-peer1", "", "1700002900", talk(8000, "deleted"), "trash", true);

    const auto report = c.run();
    CHECK(report.finished);
    CHECK_EQ(report.chats.added, int64_t(3));
    CHECK_EQ(report.chats.present, int64_t(0));
    CHECK_EQ(report.chats.skipped, int64_t(1));
    CHECK_EQ(report.inbox.added, int64_t(9));      // the five of the fixture and four lookalikes
    CHECK_EQ(report.channels.added, int64_t(3));   // two, and the chat-looking post
    CHECK_EQ(report.broadcasts.added, int64_t(3)); // two, and the chat-looking broadcast
    CHECK_EQ(report.trash.added, int64_t(3));      // the deleted chat is none of them
    CHECK_EQ(report.lettersAdded(), int64_t(22));

    auto &m = c.mailbox;
    const auto thread = m.chatMessages(me, "BM-peer1");
    CHECK_EQ(thread.size(), size_t(2));
    CHECK_EQ(thread[0].id, idOf(40));                       // the id PyBitmessage's inbox gave it
    CHECK_EQ(thread[0].body, std::string("Hello **you**")); // the text, not the document
    CHECK_EQ(thread[0].seqno, int64_t(1000));
    CHECK_EQ(thread[0].received, int64_t(1700002000)); // when it came, not when it was imported
    CHECK(!thread[0].outgoing);
    CHECK_EQ(thread[1].body, std::string("Second"));
    CHECK_EQ(m.chatMessages(me, "BM-peer2").size(), size_t(1)); // the same number, another peer
    // Unread as it was, over there: the first of one peer and the one of the other.
    CHECK_EQ(m.chatUnread(), 2);
    CHECK_EQ(m.chatSummaries().size(), size_t(2));
    // The lookalikes are letters, where a letter goes, and the chat's own never are.
    for (int seed : {43, 44, 45, 46})
        CHECK_EQ(m.message(idOf(seed)).folder, std::string("Inbox"));
    CHECK_EQ(m.message(idOf(47)).folder, std::string("Channels"));
    CHECK_EQ(m.message(idOf(48)).folder, std::string("Broadcasts"));
    for (int seed : {40, 41, 42, 49})
        CHECK_THROWS(m.message(idOf(seed)));
    CHECK_EQ(m.chatState(idOf(49)), std::string()); // nor is the deleted one a chat
    // The unread letters are the letters, as before.
    CHECK_EQ(m.unreadByFolder().at("Inbox"), 3); // d1 d2 nd
    CHECK_EQ(m.incomingChatCount(), uint64_t(0));
    CHECK_EQ(m.incomingLetterCount(), uint64_t(0));
    CHECK(m.outbox().empty());
}

TEST(a_chat_with_no_date_is_dated_1970_and_counted_like_a_letter) {
    Case c;
    c.source.inbox(rawHash(40), c.me.address, "BM-peer1", "", "yesterday", talk(1000, "Hello"),
                   "inbox", false);
    const auto report = c.run();
    CHECK_EQ(report.chats.added, int64_t(1));
    CHECK_EQ(report.withoutDate, int64_t(2)); // and the letter of the fixture that has none
    CHECK_EQ(c.mailbox.chatMessages(c.me.address, "BM-peer1")[0].received, int64_t(0));
}

TEST(a_chat_whose_text_is_not_clean_utf8_is_told_as_when_it_arrives) {
    Case c;
    // Two stray bytes in the text, and a byte order mark in front of another: the message path
    // replaces the first and takes off the second, and so does the import.
    auto stray = talk(1000, "stray xx bytes");
    stray.replace(stray.find("xx"), 2, "\xFF\xFE");
    c.source.inbox(rawHash(40), c.me.address, "BM-peer1", "", "1700002000", stray, "inbox", false);
    c.source.inbox(rawHash(41), c.me.address, "BM-peer1", "", "1700002100",
                   "\xEF\xBB\xBF" + talk(2000, "marked"), "inbox", false);
    const auto report = c.run();
    CHECK_EQ(report.chats.added, int64_t(2));
    const auto thread = c.mailbox.chatMessages(c.me.address, "BM-peer1");
    CHECK_EQ(thread.size(), size_t(2));
    CHECK_EQ(thread[0].body, std::string("stray \xEF\xBF\xBD\xEF\xBF\xBD bytes"));
    CHECK_EQ(thread[1].body, std::string("marked"));
}

TEST(a_chat_in_the_inbox_is_brought_in_once) {
    Case c;
    c.source.inbox(rawHash(40), c.me.address, "BM-peer1", "", "1700002000", talk(1000, "Hello"),
                   "inbox", false);
    c.source.inbox(rawHash(41), c.me.address, "BM-peer1", "", "1700002100", talk(2000, "Again"),
                   "inbox", false);
    CHECK_EQ(c.run().chats.added, int64_t(2));
    c.mailbox.markChatRead(c.me.address, "BM-peer1");
    const auto again = c.run();
    CHECK_EQ(again.chats.added, int64_t(0));
    CHECK_EQ(again.chats.present, int64_t(2));
    CHECK_EQ(c.mailbox.chatMessages(c.me.address, "BM-peer1").size(), size_t(2));
    CHECK_EQ(c.mailbox.chatUnread(), 0); // and read stays read
}

TEST(a_chat_this_mailbox_received_live_is_not_brought_in_again) {
    Case c;
    c.source.inbox(rawHash(40), c.me.address, "BM-peer1", "", "1700002000", talk(1000, "Hello"),
                   "inbox", false);
    // The scan stored it under an id of its own (a chat with an acknowledgment is keyed by what it
    // says), and the user has read it here.
    c.mailbox.storeChat("keyed-by-what-it-says", "BM-peer1", c.me.address, 1000, "Hello",
                        bmc::now() + 3600);
    c.mailbox.markChatRead(c.me.address, "BM-peer1");
    const auto report = c.run();
    CHECK_EQ(report.chats.added, int64_t(0));
    CHECK_EQ(report.chats.present, int64_t(1));
    const auto thread = c.mailbox.chatMessages(c.me.address, "BM-peer1");
    CHECK_EQ(thread.size(), size_t(1));
    CHECK_EQ(thread[0].id, std::string("keyed-by-what-it-says"));
    CHECK_EQ(c.mailbox.chatUnread(), 0);
}

TEST(the_options_decide_which_chats_are_read) {
    Case c;
    c.source.inbox(rawHash(40), c.me.address, "BM-peer1", "", "1700002000", talk(1000, "Hello"),
                   "inbox", false);
    c.source.inbox(rawHash(49), c.me.address, "BM-peer1", "", "1700002900", talk(8000, "gone"),
                   "trash", true);
    auto o = c.options();
    o.inbox = false; // without the letters of the inbox, its chats stay over there too
    const auto report = c.run(o);
    CHECK_EQ(report.chats.added, int64_t(0));
    CHECK_EQ(report.chats.skipped, int64_t(1)); // the deleted one is still no chat's to take
    CHECK_EQ(c.mailbox.chatSummaries().size(), size_t(0));
    o.inbox = true;
    o.trash = false; // deleted ones asked not to come along are not even read
    const auto next = c.run(o);
    CHECK_EQ(next.chats.added, int64_t(1));
    CHECK_EQ(next.chats.skipped, int64_t(0));
}

TEST(the_scan_after_an_import_stores_no_chat_the_import_brought) {
    Case c;
    // A chat from the network with an acknowledgment, to an identity of keys.dat, and the row
    // PyBitmessage made of it: its inbox holds the object's hash.
    const auto alice = bmc::Protocol::identity("alice");
    bmc::Bytes token(32);
    bmc::randomBytes(token.data(), token.size());
    const int64_t now = bmc::now();
    const auto object = bmc::Wire::encodeMessage(alice, bmc::Wire::publicIdentity(c.me), "",
                                                 talk(1792065603652, "Hello"), now + 3600,
                                                 bmc::Wire::acknowledgment(token, now + 3600));
    const auto root = bmc::test::tempDir();
    fake::cacheObject(root, object);
    c.source.inbox(bmc::unhex(bmc::Protocol::inventoryHash(object)), c.me.address, alice.address,
                   "", "1700000000", talk(1792065603652, "Hello"), "inbox", false);
    c.run();
    CHECK_EQ(c.mailbox.chatMessages(c.me.address, alice.address).size(), size_t(1));
    // The identities came in, so the scan reads everything the node holds once more.
    bmc::Cache cache(root);
    bmc::Delivery delivery(root);
    c.mailbox.bindCache(cache.id());
    c.mailbox.advance(0);
    delivery.scan(cache, c.mailbox, c.vault, now);
    CHECK_EQ(c.mailbox.checkpoint(), int64_t(1)); // it did read the object
    const auto thread = c.mailbox.chatMessages(c.me.address, alice.address);
    CHECK_EQ(thread.size(), size_t(1)); // and found nothing new in it
    CHECK_EQ(thread[0].received, int64_t(1700000000));
    CHECK_EQ(c.mailbox.chatUnread(), 1); // unread as PyBitmessage had it
    CHECK_EQ(c.mailbox.incomingChatCount(), uint64_t(0));
    for (const auto &m : c.mailbox.messages()) // and no letter of its own either
        CHECK(m.from != alice.address);
}

TEST(the_time_left_is_told_once_there_is_something_to_go_by) {
    pybm::Estimator e;
    CHECK(e.secondsLeft() < 0); // no sample
    e.sample(0, 0, 100);
    CHECK(e.secondsLeft() < 0);
    e.sample(1, 10, 100);
    CHECK(e.secondsLeft() < 0); // under two seconds of work is nothing to go by
    e.sample(3, 30, 100);
    CHECK(near(e.secondsLeft(), 7.0)); // ten a second, seventy to go
    e.sample(4, 40, 100);
    CHECK(near(e.secondsLeft(), 6.0));
    // The pace doubles: the answer follows it, though not at once.
    e.sample(5, 60, 100);
    CHECK(e.secondsLeft() > 2.0 && e.secondsLeft() < 4.0);
    e.sample(6, 80, 100);
    e.sample(7, 100, 100);
    CHECK(near(e.secondsLeft(), 0.0));

    // Two seconds of work but hardly any of it done is not enough to go by either.
    pybm::Estimator little;
    little.sample(0, 0, 1000);
    little.sample(3, 5, 1000);
    CHECK(little.secondsLeft() < 0);
    little.sample(4, 30, 1000);
    CHECK(little.secondsLeft() > 0);

    // No progress is no answer, not a division by zero.
    pybm::Estimator stuck;
    stuck.sample(0, 0, 100);
    stuck.sample(5, 0, 100);
    CHECK(stuck.secondsLeft() < 0);
    // A total that is not known.
    pybm::Estimator unknown;
    unknown.sample(0, 0, 0);
    unknown.sample(5, 10, 0);
    CHECK(unknown.secondsLeft() < 0);
    // Finished at once is finished.
    pybm::Estimator quick;
    quick.sample(0, 0, 50);
    quick.sample(0.5, 50, 50);
    CHECK(near(quick.secondsLeft(), 0.0));
    // A clock that does not move forward is passed over.
    pybm::Estimator odd;
    odd.sample(0, 0, 100);
    odd.sample(3, 30, 100);
    odd.sample(2, 90, 100);
    CHECK(near(odd.secondsLeft(), 7.0));
}

TEST(a_real_pybitmessage_folder_is_imported_whole_and_only_once) {
    const char *dir = std::getenv("YNOTBIT_PYBM_SAMPLE");
    if (!dir || !*dir) {
        std::puts("  skipped: YNOTBIT_PYBM_SAMPLE does not name a PyBitmessage folder");
        return;
    }
    bmc::Vault vault;
    vault.create(bmc::test::tempDir() + "/real.bmvault", "password");
    vault.addIdentity("Mine");
    bmc::Mailbox mailbox;
    mailbox.create(bmc::test::tempDir() + "/real.bmmail", "key-1", key32());
    const auto p = pybm::preview(dir);
    CHECK(p.trouble == pybm::Trouble::None);
    CHECK(p.identities > 0 && p.inbox + p.channelLetters + p.broadcastLetters > 0);
    pybm::Options o;
    o.folder = dir;
    const auto started = std::chrono::steady_clock::now();
    pybm::Importer importer(o, mailbox, vault);
    while (importer.step(1000)) {
    }
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto &r = importer.report();
    CHECK(r.finished && r.trouble == pybm::Trouble::None);
    std::printf("  imported %lld letters, %lld chat messages, %d identities, %d subscriptions in "
                "%.2f s\n",
                static_cast<long long>(r.lettersAdded()), static_cast<long long>(r.chats.added),
                int(r.identities.size()), int(r.subscriptions.added), seconds);
    // Every row read became a letter or a chat message of this app, except the deleted chats.
    CHECK_EQ(r.lettersAdded() + r.chats.added + r.chats.skipped, p.letters());
    CHECK_EQ(int64_t(mailbox.messages().size()), r.lettersAdded());
    CHECK_EQ(int64_t(r.identities.size()), p.identities);
    CHECK_EQ(int64_t(vault.identities().size()), 1 + p.identities);
    CHECK_EQ(r.subscriptions.added, p.subscriptions);
    const auto unread = mailbox.unreadByFolder();
    const auto count = [&](const char *folder) {
        const auto it = unread.find(folder);
        return it == unread.end() ? 0 : it->second;
    };
    CHECK_EQ(int64_t(count("Inbox")) + mailbox.chatUnread(), p.inboxUnread);
    // No chat message of this app is left among the letters of the inbox.
    for (const auto &summary : mailbox.messageSummaries("Inbox", "", 0, 1000000)) {
        const auto letter = mailbox.message(summary.hash);
        CHECK(letter.from.empty() || bmc::parseChatMessage(letter.subject, letter.body, nullptr) !=
                                         bmc::ChatParseResult::Chat);
    }
    CHECK_EQ(int64_t(count("Channels")), p.channelUnread);
    CHECK_EQ(int64_t(count("Broadcasts")), p.broadcastUnread);
    CHECK_EQ(mailbox.incomingLetterCount(), uint64_t(0));
    // Every chan's post is in Channels, under an address that is a chan of the vault.
    std::set<std::string> chans;
    for (const auto &i : vault.identities())
        if (i.chan)
            chans.insert(i.address);
    for (const auto &address : mailbox.channelAddresses("Channels"))
        CHECK(chans.count(address) == 1);
    for (const auto &message : mailbox.messageSummaries(1000))
        CHECK(!(message.folder == "Inbox" && chans.count(message.to)));
    // Once only.
    pybm::Importer again(o, mailbox, vault);
    while (again.step(1000)) {
    }
    CHECK_EQ(again.report().lettersAdded(), int64_t(0));
    CHECK_EQ(again.report().chats.added, int64_t(0));
    CHECK_EQ(int64_t(mailbox.messages().size()), r.lettersAdded());
    CHECK_EQ(int64_t(vault.identities().size()), 1 + p.identities);
}

TEST(a_big_import_keeps_every_step_short_and_the_estimate_honest) {
    const char *rows = std::getenv("YNOTBIT_PYBM_BIG_ROWS");
    if (!rows || !*rows) {
        std::puts("  skipped: YNOTBIT_PYBM_BIG_ROWS is not set");
        return;
    }
    const int count = std::atoi(rows);
    Case c; // the small fixture is in the way of counting, so this one has its own folder
    pybmfix::Folder big;
    const auto chan = c.chan.address, me = c.me.address;
    big.keys(pybmfix::kSettings + pybmfix::section(c.me, "Me") +
             pybmfix::section(c.chan, "[chan] general", true));
    const std::string body(1000, 'x');
    big.exec("BEGIN");
    for (int i = 0; i < count; ++i) {
        bmc::Bytes id(32, 0);
        for (int b = 0; b < 4; ++b)
            id[size_t(b)] = uint8_t(i >> (8 * b));
        id[31] = 1;
        const char *to =
            i % 3 == 0 ? pybm::kBroadcastAddress : (i % 3 == 1 ? chan.c_str() : me.c_str());
        big.inbox(id, to, "BM-sender" + std::to_string(i % 500), "subject " + std::to_string(i),
                  std::to_string(1700000000 + i), body + std::to_string(i), "inbox", i % 7 != 0);
    }
    big.exec("COMMIT");
    bmc::Vault vault;
    vault.create(bmc::test::tempDir() + "/big.bmvault", "password");
    vault.addIdentity("Mine");
    bmc::Mailbox mailbox;
    mailbox.create(bmc::test::tempDir() + "/big.bmmail", "key-1", key32());
    pybm::Options o;
    o.folder = big.dir;
    pybm::Importer importer(o, mailbox, vault);
    pybm::Estimator estimator;
    const auto started = std::chrono::steady_clock::now();
    const auto since = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    double worst = 0, firstGuess = -1;
    while (true) {
        const double before = since();
        const bool more = importer.step(80); // what a window gives each turn of its event loop
        worst = std::max(worst, since() - before);
        const auto &p = importer.progress();
        estimator.sample(since(), double(p.done), double(p.total));
        if (firstGuess < 0 && estimator.secondsLeft() >= 0)
            firstGuess = since() + estimator.secondsLeft();
        if (!more)
            break;
    }
    const double total = since();
    CHECK(importer.report().finished);
    CHECK_EQ(importer.report().lettersAdded(), int64_t(count));
    std::printf(
        "  %d letters in %.1f s (%.0f letters/s); longest step %.0f ms; first estimate of the "
        "end %.1f s, real end %.1f s\n",
        count, total, count / total, worst * 1000, firstGuess, total);
    CHECK(worst < 1.5);
}
