// Bringing in the letters, contacts and subscriptions of another client: each letter with its own
// date, read state and folder; and the guarantee that one network object is never stored twice,
// whether the scan or the import saw it first.
#include "fake_node.h"
#include <bmc/cache.h>
#include <bmc/chat_format.h>
#include <bmc/clock.h>
#include <bmc/delivery.h>
#include <bmc/mailbox.h>
#include <bmc/protocol.h>
#include <bmc/random.h>
#include <bmc/test.h>
#include <bmc/vault.h>
#include <bmc/wire.h>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <sqlcipher/sqlite3.h>

using bmc::ImportedChat;
using bmc::ImportedLetter;
using bmc::Mailbox;

namespace {
bmc::Secret key32() {
    bmc::Secret key(32);
    for (int i = 0; i < 32; ++i)
        key.data()[i] = 0x01;
    return key;
}
std::string newPath() {
    return bmc::test::tempDir() + "/m.bmmail";
}
void editMailbox(const std::string &path, const bmc::Secret &key, const char *sql) {
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
        bmc::test::fail(__FILE__, __LINE__, "cannot open the mailbox file");
    const bool ran = sqlite3_key(db, key.data(), 32) == SQLITE_OK &&
                     sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    const std::string why = ran ? std::string() : sqlite3_errmsg(db);
    sqlite3_close(db);
    if (!ran)
        bmc::test::fail(__FILE__, __LINE__, why);
}
int64_t kNow() {
    static const int64_t value = bmc::now();
    return value;
}
ImportedLetter letter(const std::string &id, const std::string &folder, const std::string &sender,
                      const std::string &recipient, int64_t received, bool unread,
                      const std::string &subject = "Subject", const std::string &body = "Body") {
    ImportedLetter l;
    l.id = id;
    l.folder = folder;
    l.sender = sender;
    l.recipient = recipient;
    l.subject = subject;
    l.body = body;
    l.received = received;
    l.unread = unread;
    return l;
}
ImportedChat chat(const std::string &id, const std::string &local, const std::string &peer,
                  int64_t seqno, const std::string &body, int64_t received, bool unread) {
    ImportedChat c;
    c.id = id;
    c.local = local;
    c.peer = peer;
    c.seqno = seqno;
    c.body = body;
    c.received = received;
    c.unread = unread;
    return c;
}
std::vector<std::string> ids(const Mailbox &m) {
    std::vector<std::string> out;
    for (const auto &message : m.messages())
        out.push_back(message.hash);
    return out;
}
const bmc::Identity &someone(int n) {
    static std::deque<bmc::Identity> made;
    while (int(made.size()) <= n)
        made.push_back(bmc::Protocol::identity("x"));
    return made[size_t(n)];
}

// A receiver: a vault with one identity, a mailbox, the node's object store and the delivery
// engine.
struct Receiver {
    std::string root, identity;
    bmc::Vault vault;
    bmc::Mailbox mailbox;
    std::unique_ptr<bmc::Cache> cache;
    std::unique_ptr<bmc::Delivery> delivery;
    Receiver() : root(bmc::test::tempDir() + "/receiver") {
        std::filesystem::create_directories(bmc::path(root));
        vault.create(root + "/v.bmvault", "import-test");
        const auto key = vault.addMailboxKey();
        mailbox.create(root + "/m.bmmail", key, vault.mailboxKey(key));
        identity = vault.addIdentity("Bob");
        cache = std::make_unique<bmc::Cache>(root);
        delivery = std::make_unique<bmc::Delivery>(root);
        mailbox.bindCache(cache->id());
    }
    // A letter from `from` with an acknowledgment, in the node's store: the kind whose id here is
    // not the hash of the object.
    std::string arrives(const bmc::Identity &from, const std::string &subject,
                        const std::string &body) {
        bmc::Bytes token(32);
        bmc::randomBytes(token.data(), token.size());
        const auto object = bmc::Wire::encodeMessage(
            from, bmc::Wire::publicIdentity(vault.identities()[0]), subject, body, kNow() + 3600,
            bmc::Wire::acknowledgment(token, kNow() + 3600));
        fake::cacheObject(root, object);
        return bmc::Protocol::inventoryHash(object);
    }
    // A chat message from `from`, which carries an acknowledgment as the app's own do: it is keyed
    // here by what it says, not by the hash of its object.
    std::string arrivesAsChat(const bmc::Identity &from, int64_t seqno, const std::string &text) {
        bmc::Bytes token(32);
        bmc::randomBytes(token.data(), token.size());
        const auto object =
            bmc::Wire::encodeMessage(from, bmc::Wire::publicIdentity(vault.identities()[0]), "",
                                     bmc::chatMessageBody(seqno, text), kNow() + 3600,
                                     bmc::Wire::acknowledgment(token, kNow() + 3600));
        fake::cacheObject(root, object);
        return bmc::Protocol::inventoryHash(object);
    }
};
} // namespace

TEST(an_older_mailbox_gets_the_object_hash_column_when_it_opens) {
    const auto path = newPath();
    const auto key = key32();
    {
        Mailbox m;
        m.create(path, "key-1", key);
        m.store("old-1", "BM-a", "BM-me", "s", "b", -1);
    }
    // The previous release's mailbox: no such column, no such index.
    editMailbox(path, key,
                "DROP INDEX messages_object_hash; ALTER TABLE messages DROP COLUMN object_hash;");
    Mailbox m;
    m.open(path, key);
    CHECK_EQ(m.messages().size(), size_t(1)); // the letter it had
    m.store("a", "BM-a", "BM-me", "s", "b", -1, "Inbox", "feed");
    m.store("b", "BM-a", "BM-me", "s", "b", -1, "Inbox", "feed");
    CHECK_EQ(m.messages().size(), size_t(2)); // and the object once
    // The previous release stores without the column, and what it stores is not held against it.
    editMailbox(path, key,
                "INSERT INTO messages(hash,sender,recipient,subject,body,folder,received) "
                "VALUES('x','BM-a','BM-me','s','b','Inbox',1)");
    CHECK_EQ(m.messages().size(), size_t(3));
}

TEST(a_network_object_is_stored_once_whatever_id_it_comes_with) {
    Mailbox m;
    m.create(newPath(), "key-1", key32());
    m.store("id-1", "BM-a", "BM-me", "s", "b", -1, "Inbox", "aa11");
    m.store("id-2", "BM-a", "BM-me", "s", "b", -1, "Inbox", "aa11"); // the same object again
    CHECK_EQ(m.messages().size(), size_t(1));
    CHECK_EQ(m.messages()[0].hash, std::string("id-1"));
    CHECK_EQ(m.incomingLetterCount(), uint64_t(1)); // and it was one arrival
    m.store("id-3", "BM-a", "BM-me", "s", "b", -1, "Inbox", "bb22");
    CHECK_EQ(m.messages().size(), size_t(2));
    // Without the hash of an object there is nothing to compare: both are kept.
    m.store("id-4", "BM-a", "BM-me", "s", "b", -1);
    m.store("id-5", "BM-a", "BM-me", "s", "b", -1);
    CHECK_EQ(m.messages().size(), size_t(4));
    // The routed form passes it on; a chat has no object hash to keep.
    m.storeRouted("id-6", "BM-a", "BM-me", "s", "b", -1, "Inbox", kNow() + 100, "bb22");
    m.storeRouted("id-7", "BM-a", "BM-me", "s", "b", -1, "Inbox", kNow() + 100, "cc33");
    CHECK_EQ(m.messages().size(), size_t(5));
}

TEST(imported_letters_keep_their_date_their_read_state_and_their_folder) {
    Mailbox m;
    m.create(newPath(), "key-1", key32());
    auto trashed = letter("tr-1", "Trash", "BM-a", "BM-me", 1700000050, true);
    trashed.previousFolder = "Channels";
    const auto counts =
        m.importLetters({letter("in-1", "Inbox", "BM-a", "BM-me", 1700000100, true),
                         letter("in-2", "Inbox", "BM-b", "BM-me", 1700000200, false),
                         letter("ch-1", "Channels", "BM-x", "BM-chan", 1700000300, true),
                         letter("bc-1", "Broadcasts", "BM-c", "BM-c", 1700000400, true), trashed,
                         letter("se-1", "Sent", "BM-me", "BM-a", 1700000500, false),
                         letter("dr-1", "Drafts", "BM-me", "BM-a", 1700000600, false)});
    CHECK_EQ(counts.added, 7);
    CHECK_EQ(counts.present, 0);
    CHECK_EQ(m.messages().size(), size_t(7));
    const auto one = m.message("in-1");
    CHECK_EQ(one.from, std::string("BM-a"));
    CHECK_EQ(one.to, std::string("BM-me"));
    CHECK_EQ(one.subject, std::string("Subject"));
    CHECK_EQ(one.body, std::string("Body"));
    CHECK_EQ(one.folder, std::string("Inbox"));
    CHECK_EQ(one.received, int64_t(1700000100)); // when it was received, not when it was imported
    CHECK(m.unread("in-1"));
    CHECK(!m.unread("in-2"));
    CHECK(!m.unread("se-1") && !m.unread("dr-1"));
    CHECK_EQ(m.message("ch-1").folder, std::string("Channels"));
    CHECK_EQ(m.message("bc-1").folder, std::string("Broadcasts"));
    CHECK_EQ(m.message("tr-1").folder, std::string("Trash"));
    CHECK_EQ(m.message("se-1").folder, std::string("Sent"));
    CHECK_EQ(m.message("dr-1").folder, std::string("Drafts"));
    // The lists order them by their own dates, newest first.
    const auto inbox = m.messageSummaries("Inbox", "", 0, 10);
    CHECK_EQ(inbox.size(), size_t(2));
    CHECK_EQ(inbox[0].hash, std::string("in-2"));
    CHECK_EQ(inbox[1].hash, std::string("in-1"));
    // The unread ones of the folders that receive mail are counted; the trashed one is not.
    CHECK_EQ(m.unreadLetters(), 3);
    const auto byFolder = m.unreadByFolder();
    CHECK_EQ(byFolder.at("Inbox"), 1);
    CHECK_EQ(byFolder.at("Channels"), 1);
    CHECK_EQ(byFolder.at("Broadcasts"), 1);
    // A letter in Trash goes back to where it came from.
    m.restoreMessage("tr-1");
    CHECK_EQ(m.message("tr-1").folder, std::string("Channels"));
}

TEST(an_import_is_no_arrival_and_leaves_the_scan_where_it_is) {
    Mailbox m;
    m.create(newPath(), "key-1", key32());
    m.advance(7);
    const auto revision = m.messageRevision();
    m.importLetters({letter("in-1", "Inbox", "BM-a", "BM-me", 1700000100, true)});
    CHECK_EQ(m.incomingLetterCount(), uint64_t(0)); // no notification for a letter of years ago
    CHECK_EQ(m.checkpoint(), int64_t(7));
    CHECK(m.messageRevision() != revision); // but the lists are told that something changed
    CHECK(m.outbox().empty());              // and nothing is queued for sending
}

TEST(importing_again_adds_only_what_is_missing) {
    Mailbox m;
    m.create(newPath(), "key-1", key32());
    const std::vector<ImportedLetter> all = {letter("in-1", "Inbox", "BM-a", "BM-me", 100, true),
                                             letter("in-2", "Inbox", "BM-a", "BM-me", 200, true)};
    CHECK_EQ(m.importLetters(all).added, 2);
    // What the reader did with a letter since is not undone.
    m.markRead("in-1");
    const auto again = m.importLetters(all);
    CHECK_EQ(again.added, 0);
    CHECK_EQ(again.present, 2);
    CHECK_EQ(m.messages().size(), size_t(2));
    CHECK(!m.unread("in-1"));
    // A letter that is gone for good comes back, as the import adds what is missing.
    m.moveMessage("in-2", "Trash");
    m.deleteMessage("in-2");
    const auto third = m.importLetters(all);
    CHECK_EQ(third.added, 1);
    CHECK_EQ(third.present, 1);
}

TEST(importing_letters_says_which_of_them_were_stored) {
    Mailbox m;
    m.create(newPath(), "key-1", key32());
    CHECK_EQ(m.importLetters({letter("in-1", "Inbox", "BM-a", "BM-me", 100, true)}).added, 1);
    // Of these three, one is here already.
    const std::vector<ImportedLetter> batch = {
        letter("in-0", "Inbox", "BM-b", "BM-me", 50, true),
        letter("in-1", "Inbox", "BM-a", "BM-me", 100, true),
        letter("out-2", "Sent", "BM-me", "BM-c", 200, false)};
    std::vector<size_t> stored;
    CHECK_EQ(m.importLetters(batch, &stored).added, 2);
    CHECK_EQ(stored.size(), size_t(2)); // the positions in the batch, in its order
    CHECK_EQ(stored.front(), size_t(0));
    CHECK_EQ(stored.back(), size_t(2));
    // Not asked, nothing is said; asked about letters that are all here, the answer is empty.
    CHECK_EQ(m.importLetters(batch).present, 3);
    std::vector<size_t> none = {7};
    m.importLetters(batch, &none);
    CHECK(none.empty());
}

TEST(a_bad_letter_in_a_batch_leaves_the_whole_batch_out) {
    Mailbox m;
    m.create(newPath(), "key-1", key32());
    CHECK_THROWS(m.importLetters({letter("in-1", "Inbox", "BM-a", "BM-me", 100, true),
                                  letter("in-2", "Outbox", "BM-a", "BM-me", 200, true)}));
    CHECK_THROWS(m.importLetters({letter("in-1", "Inbox", "BM-a", "BM-me", 100, true),
                                  letter("", "Inbox", "BM-a", "BM-me", 200, true)}));
    CHECK_EQ(m.messages().size(), size_t(0)); // not even the good one
    CHECK_EQ(m.importLetters({}).added, 0);
}

TEST(a_failure_in_the_middle_of_an_import_leaves_none_of_the_batch) {
    const auto path = newPath();
    const auto key = key32();
    {
        Mailbox m;
        m.create(path, "key-1", key);
    }
    // A letter the database refuses, as a full disk or a damaged page would.
    editMailbox(path, key,
                "CREATE TRIGGER refuse BEFORE INSERT ON messages WHEN NEW.hash='boom' "
                "BEGIN SELECT RAISE(ABORT, 'refused'); END");
    Mailbox m;
    m.open(path, key);
    CHECK_THROWS(m.importLetters({letter("in-1", "Inbox", "BM-a", "BM-me", 100, true),
                                  letter("boom", "Inbox", "BM-a", "BM-me", 200, true),
                                  letter("in-3", "Inbox", "BM-a", "BM-me", 300, true)}));
    CHECK_EQ(m.messages().size(), size_t(0)); // the first was in the batch, and is out with it
    // The mailbox is as usable as before.
    CHECK_EQ(m.importLetters({letter("in-1", "Inbox", "BM-a", "BM-me", 100, true)}).added, 1);
}

TEST(an_imported_letter_stops_the_scan_from_storing_its_object_again) {
    Receiver bob;
    const auto &alice = someone(0);
    const auto objectHash = bob.arrives(alice, "Hello Bob", "Body");
    // The import has the letter first, as PyBitmessage's inbox had it: under the object's hash.
    auto l =
        letter(objectHash, "Inbox", alice.address, bob.identity, 1700000000, false, "Hello Bob");
    l.objectHash = objectHash;
    CHECK_EQ(bob.mailbox.importLetters({l}).added, 1);
    // Then the scan reads the object (the identities were added, so it reads all again).
    bob.delivery->scan(*bob.cache, bob.mailbox, bob.vault, kNow());
    CHECK_EQ(bob.mailbox.checkpoint(), int64_t(1)); // it did read it
    CHECK_EQ(bob.mailbox.messages().size(), size_t(1));
    CHECK_EQ(bob.mailbox.message(objectHash).received, int64_t(1700000000)); // the imported one
    CHECK(!bob.mailbox.unread(objectHash));
    CHECK_EQ(bob.mailbox.incomingLetterCount(), uint64_t(0));
}

TEST(a_letter_the_scan_stored_stops_the_import_from_adding_its_object) {
    Receiver bob;
    const auto &alice = someone(1);
    const auto objectHash = bob.arrives(alice, "Hello Bob", "Body");
    bob.delivery->scan(*bob.cache, bob.mailbox, bob.vault, kNow());
    const auto stored = bob.mailbox.messages();
    CHECK_EQ(stored.size(), size_t(1));
    // A letter with an acknowledgment is keyed by what it says, not by the hash of its object, so
    // only the object hash can tell that the import brings the same letter.
    CHECK(stored[0].hash != objectHash);
    auto l =
        letter(objectHash, "Inbox", alice.address, bob.identity, 1700000000, false, "Hello Bob");
    l.objectHash = objectHash;
    const auto counts = bob.mailbox.importLetters({l});
    CHECK_EQ(counts.added, 0);
    CHECK_EQ(counts.present, 1);
    CHECK_EQ(bob.mailbox.messages().size(), size_t(1));
}

TEST(letters_are_recognised_by_what_they_say) {
    const auto f = bmc::letterFingerprint("BM-a", "BM-me", "Hi", "Body");
    CHECK_EQ(f.size(), size_t(32));
    CHECK_EQ(f, bmc::letterFingerprint("BM-a", "BM-me", "Hi", "Body"));
    CHECK(f != bmc::letterFingerprint("BM-b", "BM-me", "Hi", "Body"));
    CHECK(f != bmc::letterFingerprint("BM-a", "BM-you", "Hi", "Body"));
    CHECK(f != bmc::letterFingerprint("BM-a", "BM-me", "Ho", "Body"));
    CHECK(f != bmc::letterFingerprint("BM-a", "BM-me", "Hi", "Body "));
    // Where one field ends and the next begins matters.
    CHECK(bmc::letterFingerprint("ab", "c", "d", "e") !=
          bmc::letterFingerprint("a", "bc", "d", "e"));
    CHECK(bmc::letterFingerprint("a", "b", "cd", "e") !=
          bmc::letterFingerprint("a", "b", "c", "de"));

    Mailbox m;
    m.create(newPath(), "key-1", key32());
    m.store("1", "BM-a", "BM-me", "Hi", "Body", -1);
    m.store("2", "BM-b", "BM-me", "Hi", "Body", -1, "Trash");
    m.store("3", "BM-c", "BM-chan", "Hi", "Body", -1, "Channels");
    const auto inbox = m.letterFingerprints({"Inbox"});
    CHECK_EQ(inbox.size(), size_t(1));
    CHECK(inbox.count(f) == 1);
    CHECK(inbox.count(bmc::letterFingerprint("BM-a", "BM-me", "Hi", "Body!")) == 0);
    const auto both = m.letterFingerprints({"Inbox", "Trash"});
    CHECK_EQ(both.size(), size_t(2));
    CHECK(both.count(bmc::letterFingerprint("BM-b", "BM-me", "Hi", "Body")) == 1);
    CHECK(m.letterFingerprints({}).empty());
}

TEST(imported_contacts_and_subscriptions_keep_what_is_there) {
    Mailbox m;
    m.create(newPath(), "key-1", key32());
    m.saveContact("BM-ann", "Ann (mine)");
    m.subscribe("BM-news", "My name for it");
    const auto sub = m.subscriptions().size();
    CHECK_EQ(m.importContacts(
                 {{"BM-ann", "Ann (theirs)", 0}, {"BM-bob", "Bob", 0}, {"BM-nameless", "  ", 0}}),
             1);
    const auto contacts = m.contacts();
    CHECK_EQ(contacts.size(), size_t(2)); // Ann was here, Bob is new, and a contact needs a name
    CHECK_EQ(contacts[0].label, std::string("Ann (mine)"));
    CHECK_EQ(contacts[1].label, std::string("Bob"));
    CHECK(contacts[1].added > 0);
    CHECK_EQ(m.importContacts({{"BM-bob", "Robert", 0}}), 0);
    CHECK_EQ(m.contacts()[1].label, std::string("Bob"));
    CHECK_EQ(m.importSubscriptions({{"BM-news", "Their name"}, {"BM-other", "Other"}}), 1);
    CHECK_EQ(m.subscriptions().size(), sub + 1);
    for (const auto &s : m.subscriptions())
        if (s.address == "BM-news")
            CHECK_EQ(s.label, std::string("My name for it"));
}

TEST(imported_sent_letters_and_drafts_work_as_the_ones_written_here) {
    Mailbox m;
    m.create(newPath(), "key-1", key32());
    auto news = letter("se-news", "Sent", "BM-me", "", 1700000500, false, "News");
    news.broadcast = true;
    // A flag that says unread on something written here is not believed: it is never unread.
    m.importLetters({letter("se-1", "Sent", "BM-me", "BM-a", 1700000400, true), news,
                     letter("dr-1", "Drafts", "BM-me", "BM-a", 1700000600, true)});
    CHECK(!m.unread("se-1") && !m.unread("dr-1"));
    CHECK_EQ(m.unreadLetters(), 0);
    CHECK_EQ(m.setting("draftkind:se-news", "direct"), std::string("broadcast"));
    CHECK_EQ(m.setting("draftkind:se-1", "direct"), std::string("direct"));
    CHECK_EQ(m.messageSummaries("Sent", "", 0, 10).size(), size_t(2));
    // A draft is edited and sent like any other: it has no delivery record.
    CHECK_EQ(m.saveDraft("dr-1", "BM-me", "BM-a", "Edited", "New text"), std::string("dr-1"));
    CHECK_EQ(m.message("dr-1").subject, std::string("Edited"));
    CHECK(m.outbox().empty());
}

TEST(imported_chats_join_their_conversation_with_their_date_and_read_state) {
    Mailbox m;
    m.create(newPath(), "key-1", key32());
    m.advance(7);
    const auto revision = m.messageRevision();
    const auto counts =
        m.importChats({chat("c-1", "BM-me", "BM-a", 1000, "first", 1700000100, true),
                       chat("c-2", "BM-me", "BM-a", 2000, "second", 1700000200, false),
                       chat("c-3", "BM-me", "BM-b", 3000, "from b", 1700000300, true)});
    CHECK_EQ(counts.added, 3);
    CHECK_EQ(counts.present, 0);
    const auto thread = m.chatMessages("BM-me", "BM-a");
    CHECK_EQ(thread.size(), size_t(2));
    CHECK_EQ(thread[0].id, std::string("c-1"));
    CHECK_EQ(thread[0].localAddress, std::string("BM-me"));
    CHECK_EQ(thread[0].peerAddress, std::string("BM-a"));
    CHECK_EQ(thread[0].body, std::string("first"));
    CHECK_EQ(thread[0].seqno, int64_t(1000));
    CHECK_EQ(thread[0].received, int64_t(1700000100)); // when it came, not when it was imported
    CHECK_EQ(thread[0].state, std::string("received"));
    CHECK(!thread[0].outgoing);
    CHECK_EQ(thread[1].id, std::string("c-2"));
    // The list of chats: two conversations, the newest first, with their unread messages counted.
    const auto chats = m.chatSummaries();
    CHECK_EQ(chats.size(), size_t(2));
    CHECK_EQ(chats[0].peer, std::string("BM-b"));
    CHECK_EQ(chats[0].unread, 1);
    CHECK_EQ(chats[1].peer, std::string("BM-a"));
    CHECK_EQ(chats[1].unread, 1); // the first is unread, the second was read over there
    CHECK_EQ(m.chatUnread(), 2);
    CHECK_EQ(m.unreadChats(10).size(), size_t(2));
    // They are chats and no letters; no arrival, nothing queued, the scan stays where it is, and
    // the lists are told that something changed.
    CHECK_EQ(m.messages().size(), size_t(0));
    CHECK_EQ(m.incomingChatCount(), uint64_t(0));
    CHECK(m.outbox().empty());
    CHECK_EQ(m.checkpoint(), int64_t(7));
    CHECK(m.messageRevision() != revision);
    // A chat is told by its date when it was said; its expiry is what its sender would have set.
    CHECK_EQ(thread[0].expires, int64_t(1700000100) + bmc::kChatTtlSeconds);
}

TEST(importing_chats_again_adds_only_what_is_missing) {
    Mailbox m;
    m.create(newPath(), "key-1", key32());
    const std::vector<ImportedChat> all = {
        chat("c-1", "BM-me", "BM-a", 1000, "first", 1700000100, true),
        chat("c-2", "BM-me", "BM-a", 2000, "second", 1700000200, true)};
    CHECK_EQ(m.importChats(all).added, 2);
    // What the reader did with a chat since is not undone.
    m.markChatRead("BM-me", "BM-a");
    const auto again = m.importChats(all);
    CHECK_EQ(again.added, 0);
    CHECK_EQ(again.present, 2);
    CHECK_EQ(m.chatUnread(), 0);
    CHECK_EQ(m.chatMessages("BM-me", "BM-a").size(), size_t(2));
    // The same message under another id: a conversation holds one message per number.
    const auto other =
        m.importChats({chat("other-id", "BM-me", "BM-a", 1000, "first", 1700000100, true)});
    CHECK_EQ(other.added, 0);
    CHECK_EQ(other.present, 1);
    // The same number in another conversation is another message.
    const auto more = m.importChats({chat("c-5", "BM-me", "BM-b", 1000, "x", 1700000100, true),
                                     chat("c-6", "BM-you", "BM-a", 1000, "y", 1700000100, true)});
    CHECK_EQ(more.added, 2);
    CHECK_EQ(m.chatSummaries().size(), size_t(3));
}

TEST(a_bad_chat_in_a_batch_leaves_the_whole_batch_out) {
    Mailbox m;
    m.create(newPath(), "key-1", key32());
    const auto good = chat("c-1", "BM-me", "BM-a", 1000, "first", 1700000100, true);
    const auto spoiled = [&](const std::function<void(ImportedChat &)> &spoil) {
        auto bad = good;
        bad.id = "c-2";
        bad.seqno = 2000;
        spoil(bad);
        return std::vector<ImportedChat>{good, bad};
    };
    CHECK_THROWS(m.importChats(spoiled([](ImportedChat &c) { c.id.clear(); })));
    CHECK_THROWS(m.importChats(spoiled([](ImportedChat &c) { c.local.clear(); })));
    CHECK_THROWS(m.importChats(spoiled([](ImportedChat &c) { c.peer.clear(); })));
    CHECK_THROWS(m.importChats(spoiled([](ImportedChat &c) { c.seqno = 0; })));
    CHECK_THROWS(m.importChats(spoiled([](ImportedChat &c) { c.body = "  \n"; })));
    CHECK_THROWS(m.importChats(spoiled(
        [](ImportedChat &c) { c.body = std::string(size_t(bmc::kMaxChatBodyBytes) + 1, 'x'); })));
    CHECK_EQ(m.chatSummaries().size(), size_t(0)); // not even the good one
    CHECK_EQ(m.importChats({}).added, 0);
    // The largest body a chat may have is one.
    CHECK_EQ(
        m.importChats({chat("c-3", "BM-me", "BM-a", 3000,
                            std::string(size_t(bmc::kMaxChatBodyBytes), 'x'), 1700000100, true)})
            .added,
        1);
}

TEST(a_chat_the_scan_stored_stops_the_import_from_adding_it_again) {
    Receiver bob;
    const auto &alice = someone(2);
    const auto objectHash = bob.arrivesAsChat(alice, 1792065603652, "hello");
    bob.delivery->scan(*bob.cache, bob.mailbox, bob.vault, kNow());
    const auto stored = bob.mailbox.chatMessages(bob.identity, alice.address);
    CHECK_EQ(stored.size(), size_t(1));
    // Keyed by what it says, as a letter with an acknowledgment is, so the id cannot tell.
    CHECK(stored[0].id != objectHash);
    const auto counts = bob.mailbox.importChats(
        {chat(objectHash, bob.identity, alice.address, 1792065603652, "hello", 1700000000, false)});
    CHECK_EQ(counts.added, 0);
    CHECK_EQ(counts.present, 1);
    CHECK_EQ(bob.mailbox.chatMessages(bob.identity, alice.address).size(), size_t(1));
    CHECK_EQ(bob.mailbox.chatUnread(), 1); // as the scan left it
}

TEST(an_imported_chat_stops_the_scan_from_storing_it_again) {
    Receiver bob;
    const auto &alice = someone(3);
    const auto objectHash = bob.arrivesAsChat(alice, 1792065603652, "hello");
    // The import has it first, as PyBitmessage's inbox had it: under the object's hash.
    CHECK_EQ(bob.mailbox
                 .importChats({chat(objectHash, bob.identity, alice.address, 1792065603652, "hello",
                                    1700000000, false)})
                 .added,
             1);
    bob.delivery->scan(*bob.cache, bob.mailbox, bob.vault, kNow());
    CHECK_EQ(bob.mailbox.checkpoint(), int64_t(1)); // it did read the object
    const auto thread = bob.mailbox.chatMessages(bob.identity, alice.address);
    CHECK_EQ(thread.size(), size_t(1));
    CHECK_EQ(thread[0].id, objectHash);
    CHECK_EQ(thread[0].received, int64_t(1700000000)); // the imported one
    CHECK_EQ(bob.mailbox.chatUnread(), 0);
    CHECK_EQ(bob.mailbox.incomingChatCount(), uint64_t(0));
    CHECK_EQ(bob.mailbox.messages().size(), size_t(0)); // and no letter either
}
