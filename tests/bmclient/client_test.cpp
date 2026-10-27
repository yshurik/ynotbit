// bmc::Client without Qt: the step that binds, scans and ticks, and the chat and letter services
// with the rules Session had (their English text is what src/i18n/error_catalog.cpp lists).
#include "fake_node.h"
#include <bmc/chat_format.h>
#include <bmc/client.h>
#include <bmc/clock.h>
#include <bmc/error.h>
#include <bmc/identity.h>
#include <bmc/mailbox.h>
#include <bmc/path.h>
#include <bmc/protocol.h>
#include <bmc/secret.h>
#include <bmc/test.h>
#include <bmc/vault.h>
#include <bmc/wire.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <sqlcipher/sqlite3.h>
#include <string>
#include <vector>

namespace {
// A client over a fresh vault, mailbox and node store, with one identity, one chan, and the other
// end of the conversation in a second vault.
struct Fixture {
    std::string root, vaultPath, mailPath, key, me, chan, peer;
    std::shared_ptr<bmc::Vault> vault = std::make_shared<bmc::Vault>();
    std::shared_ptr<bmc::Mailbox> mailbox = std::make_shared<bmc::Mailbox>();
    bmc::Vault peerVault;
    std::unique_ptr<bmc::Client> client;

    Fixture() {
        root = bmc::test::tempDir();
        vaultPath = root + "/v.bmvault";
        mailPath = root + "/m.bmmail";
        vault->create(vaultPath, "slice3-client");
        key = vault->addMailboxKey();
        mailbox->create(mailPath, key, vault->mailboxKey(key));
        me = vault->addIdentity("Me");
        chan = vault->addChannel("slice3-chan", "#chan");
        peerVault.create(root + "/peer.bmvault", "slice3-peer");
        peer = peerVault.addIdentity("Peer");
        client = std::make_unique<bmc::Client>(vault, mailbox, root);
        client->bindMailboxToCache();
    }
    // An object from the peer to this vault's identity, as it would arrive from the network.
    bmc::Bytes incoming(const std::string &subject, const std::string &body, int64_t expires) {
        return bmc::Wire::encodeMessage(
            bmc::IdentityRef(peerVault.identities()[0]),
            bmc::Wire::publicIdentity(bmc::IdentityRef(vault->identities()[0])), subject, body,
            expires);
    }
};

std::string threwMessage(const std::function<void()> &call) {
    try {
        call();
    } catch (const std::exception &e) {
        return e.what();
    }
    return "no exception";
}
// Deletes the first rows of the node's store, the way retention does: the scan then starts past
// the checkpoint.
void dropObjectsBefore(const std::string &root, int64_t sequence) {
    sqlite3 *db = nullptr;
    CHECK_EQ(
        sqlite3_open_v2((root + "/objects.sqlite").c_str(), &db, SQLITE_OPEN_READWRITE, nullptr),
        SQLITE_OK);
    const auto sql = "DELETE FROM objects WHERE seq<" + std::to_string(sequence);
    CHECK_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);
}
} // namespace

TEST(a_step_reports_nothing_until_a_mailbox_is_open) {
    Fixture f;
    const auto report = f.client->step(bmc::now(), false);
    CHECK(report.open);
    CHECK_EQ(report.checkpoint, int64_t(0));
    CHECK(!report.working);
    CHECK(!report.pruned);

    f.mailbox->close();
    const auto closed = f.client->step(bmc::now(), false);
    CHECK(!closed.open);
    CHECK_EQ(closed.checkpoint, int64_t(0));
}

TEST(a_step_reads_the_store_and_then_reports_being_up_to_date) {
    Fixture f;
    const auto expires = bmc::now() + 3600;
    fake::cacheObject(f.root, f.incoming("Hello", "From the peer", expires));

    const auto first = f.client->step(bmc::now(), false);
    CHECK(first.open);
    CHECK(first.upToDate);
    CHECK(!first.pruned);
    CHECK_EQ(first.checkpoint, int64_t(1));
    const auto messages = f.mailbox->messages();
    CHECK_EQ(messages.size(), size_t(1));
    CHECK_EQ(messages[0].subject, std::string("Hello"));
    CHECK_EQ(messages[0].body, std::string("From the peer"));
    CHECK_EQ(messages[0].from, f.peer);
    CHECK_EQ(messages[0].folder, std::string("Inbox"));

    // Nothing new: the same state, and no second copy of the letter.
    const auto second = f.client->step(bmc::now(), false);
    CHECK(second.upToDate);
    CHECK_EQ(second.checkpoint, int64_t(1));
    CHECK_EQ(f.mailbox->messages().size(), size_t(1));
}

TEST(a_step_reports_objects_retention_removed) {
    Fixture f;
    const auto expires = bmc::now() + 3600;
    fake::cacheObject(f.root, f.incoming("One", "First", expires));
    fake::cacheObject(f.root, f.incoming("Two", "Second", expires));
    fake::cacheObject(f.root, f.incoming("Three", "Third", expires));
    dropObjectsBefore(f.root, 3); // the first two are gone before this mailbox ever read them

    const auto report = f.client->step(bmc::now(), false);
    CHECK(report.pruned);
    CHECK_EQ(report.checkpoint, int64_t(3));
}

TEST(a_step_reports_the_mailbox_changing_under_it) {
    Fixture f;
    const auto now = bmc::now();
    // Planning an outgoing letter writes the outbox: the mailbox's update hook moves the revision
    // the desktop reloads on.
    const auto draft = f.mailbox->saveDraft({}, f.me, f.peer, "Subject", "Body");
    f.mailbox->setSetting("draftkind:" + draft, "direct");
    f.client->sendLetter(draft, now);
    const auto afterQueue = f.mailbox->messageRevision();
    f.client->step(now, false);
    CHECK_EQ(f.mailbox->outgoing(draft).state, std::string("awaiting_pubkey"));
    CHECK(f.mailbox->messageRevision() > afterQueue);
    // A scan that stores an incoming letter writes through the same hook.
    const auto afterPlan = f.mailbox->messageRevision();
    fake::cacheObject(f.root, f.incoming("Hello", "Body", now + 3600));
    f.client->step(now, false);
    CHECK_EQ(f.mailbox->messageCount("Inbox", ""), size_t(1));
    CHECK(f.mailbox->messageRevision() > afterPlan);
}

TEST(a_chat_needs_an_own_sender_and_a_real_recipient) {
    Fixture f;
    const auto now = bmc::now();
    // A chan of this vault is not a chat sender.
    const auto chanMessage = threwMessage([&] { f.client->sendChat(f.chan, f.peer, "hi", now); });
    CHECK_EQ(chanMessage, std::string("Choose a sender from this vault"));
    // A sender outside the vault is refused too.
    const auto strangerMessage =
        threwMessage([&] { f.client->sendChat("BM-stranger", f.peer, "hi", now); });
    CHECK_EQ(strangerMessage, std::string("Choose a sender from this vault"));
    // A recipient that is not an address, or the sender itself.
    const auto badRecipient =
        threwMessage([&] { f.client->sendChat(f.me, "not-an-address", "hi", now); });
    CHECK_EQ(badRecipient, std::string("Enter a valid Bitmessage recipient address"));
    const auto self = threwMessage([&] { f.client->sendChat(f.me, f.me, "hi", now); });
    CHECK_EQ(self, std::string("Enter a valid Bitmessage recipient address"));
    // A body that is only white space, or too long.
    const auto blank = threwMessage([&] { f.client->sendChat(f.me, f.peer, "  \n ", now); });
    CHECK_EQ(blank, std::string("Chat message must be between 1 and 8192 bytes"));
    const auto huge = threwMessage([&] {
        f.client->sendChat(f.me, f.peer, std::string(size_t(bmc::kMaxChatBodyBytes) + 1, 'x'), now);
    });
    CHECK_EQ(huge, std::string("Chat message must be between 1 and 8192 bytes"));

    const auto id = f.client->sendChat(f.me, f.peer, "hello", now);
    CHECK(!id.empty());
    CHECK_EQ(f.client->chatState(id), std::string("queued"));
    CHECK_EQ(f.mailbox->outgoing(id).expires, now + bmc::kChatTtlSeconds); // a chat lives a day
    CHECK_EQ(f.mailbox->outgoing(id).state, std::string("queued"));

    // With no mailbox there is nothing to queue into.
    f.mailbox->close();
    const auto closed = threwMessage([&] { f.client->sendChat(f.me, f.peer, "hi", now); });
    CHECK_EQ(closed, std::string("Open a mailbox first"));
    CHECK_EQ(f.client->chatState(id), std::string());
    CHECK(f.client->unreadChats(10).empty());
    CHECK(!f.client->markChatMessageRead(id));
}

TEST(a_service_may_chat_from_a_chan_and_for_less_than_a_day) {
    Fixture f;
    const auto now = bmc::now();
    // The rule for everybody else stays: a chan is not a chat sender.
    CHECK_EQ(threwMessage([&] { f.client->sendChat(f.chan, f.peer, "hi", now); }),
             std::string("Choose a sender from this vault"));
    const auto id = f.client->sendServiceChat(f.chan, f.peer, "/address @ben", 3600, now);
    CHECK_EQ(f.client->chatState(id), std::string("queued"));
    CHECK_EQ(f.mailbox->outgoing(id).expires, now + 3600);
    CHECK_EQ(f.mailbox->outgoing(id).message.from, f.chan);
    CHECK_EQ(f.mailbox->outgoing(id).message.to, f.peer);
    // An identity of the vault may use it too; the chat then lives as long as asked.
    const auto second = f.client->sendServiceChat(f.me, f.peer, "/help", bmc::kChatTtlSeconds, now);
    CHECK_EQ(f.mailbox->outgoing(second).expires, now + bmc::kChatTtlSeconds);
    // The same refusals as for a chat, in the same words, and a life that is none or too long.
    const auto refused = [&](const std::string &from, const std::string &to,
                             const std::string &body, int64_t ttl) {
        return threwMessage([&] { f.client->sendServiceChat(from, to, body, ttl, now); });
    };
    CHECK_EQ(refused("BM-stranger", f.peer, "hi", 60),
             std::string("Choose a sender from this vault"));
    CHECK_EQ(refused(f.chan, "not-an-address", "hi", 60),
             std::string("Enter a valid Bitmessage recipient address"));
    CHECK_EQ(refused(f.chan, f.chan, "hi", 60),
             std::string("Enter a valid Bitmessage recipient address"));
    CHECK_EQ(refused(f.chan, f.peer, "  ", 60),
             std::string("Chat message must be between 1 and 8192 bytes"));
    CHECK_EQ(refused(f.chan, f.peer, "hi", 0), std::string("Invalid chat message or expiry"));
    CHECK_EQ(refused(f.chan, f.peer, "hi", -5), std::string("Invalid chat message or expiry"));
    CHECK_EQ(refused(f.chan, f.peer, "hi", bmc::kChatTtlSeconds + 1),
             std::string("Invalid chat message or expiry"));
    f.mailbox->close();
    CHECK_EQ(refused(f.chan, f.peer, "hi", 60), std::string("Open a mailbox first"));
}

TEST(a_chat_from_the_network_is_a_service_a_bot_can_poll) {
    Fixture f;
    const auto expires = bmc::now() + 3600;
    fake::cacheObject(f.root,
                      f.incoming("", bmc::chatMessageBody(7, "hello from the peer"), expires));
    f.client->step(bmc::now() + 60, false);

    const auto unread = f.client->unreadChats(10);
    CHECK_EQ(unread.size(), size_t(1));
    CHECK_EQ(unread[0].localAddress, f.me);
    CHECK_EQ(unread[0].peerAddress, f.peer);
    CHECK_EQ(unread[0].body, std::string("hello from the peer"));
    CHECK_EQ(unread[0].seqno, int64_t(7));
    CHECK(!unread[0].outgoing);

    CHECK(f.client->markChatMessageRead(unread[0].id));
    CHECK(!f.client->markChatMessageRead(unread[0].id));
    CHECK(f.client->unreadChats(10).empty());
    CHECK_EQ(f.client->chatState(unread[0].id), std::string("received"));
}

TEST(a_letter_is_checked_before_it_is_queued) {
    Fixture f;
    const auto now = bmc::now();

    const auto notMine = f.mailbox->saveDraft({}, "BM-stranger", f.peer, "Subject", "Body");
    CHECK_EQ(threwMessage([&] { f.client->sendLetter(notMine, now); }),
             std::string("Choose a sender from this vault"));
    const auto badRecipient = f.mailbox->saveDraft({}, f.me, "not-an-address", "Subject", "Body");
    CHECK_EQ(threwMessage([&] { f.client->sendLetter(badRecipient, now); }),
             std::string("Enter a valid Bitmessage recipient address"));
    const auto twoLines = f.mailbox->saveDraft({}, f.me, f.peer, "Subject\nand more", "Body");
    CHECK_EQ(threwMessage([&] { f.client->sendLetter(twoLines, now); }),
             std::string("Subject must be one line"));
    const auto blank = f.mailbox->saveDraft({}, f.me, f.peer, "Subject", "   \n");
    CHECK_EQ(threwMessage([&] { f.client->sendLetter(blank, now); }),
             std::string("Write a message before sending"));
    const auto oversized = f.mailbox->saveDraft({}, f.me, f.peer, "Subject",
                                                std::string(size_t(bmc::kMaxLetterText), 'x'));
    CHECK_EQ(threwMessage([&] { f.client->sendLetter(oversized, now); }),
             std::string("Letter is too large"));

    // None of the refusals left a delivery behind.
    CHECK(f.mailbox->outbox().empty());
    CHECK_EQ(f.mailbox->message(twoLines).folder, std::string("Drafts"));

    const auto good = f.mailbox->saveDraft({}, f.me, f.peer, "Subject", "Body");
    f.mailbox->setSetting("draftkind:" + good, "direct");
    f.client->sendLetter(good, now);
    CHECK_EQ(f.mailbox->outgoing(good).state, std::string("queued"));
    CHECK_EQ(f.mailbox->message(good).folder, std::string("Outbox"));

    // A broadcast needs no recipient address.
    const auto news = f.mailbox->saveDraft({}, f.me, {}, "News", "Everyone");
    f.mailbox->setSetting("draftkind:" + news, "broadcast");
    f.client->sendLetter(news, now);
    CHECK_EQ(f.mailbox->outgoing(news).kind, std::string("broadcast"));
}

// A person the user deals with is listened to for broadcasts, with no subscription listed until
// they post (Mailbox::watch): a letter or a chat from them, and one to them, is such dealing.
TEST(a_letter_from_a_person_makes_a_pending_subscription_to_them) {
    Fixture f;
    fake::cacheObject(f.root, f.incoming("Hello", "From the peer", bmc::now() + 3600));
    f.client->step(bmc::now(), false);
    CHECK_EQ(f.mailbox->messages().size(), size_t(1));
    CHECK_EQ(f.mailbox->watched(), std::vector<std::string>{f.peer});
    CHECK(f.mailbox->subscriptions().empty()); // not listed until the person posts
}

TEST(a_chat_from_a_person_makes_one_too) {
    Fixture f;
    fake::cacheObject(
        f.root, f.incoming("", bmc::chatMessageBody(1700000000123, "hello"), bmc::now() + 3600));
    f.client->step(bmc::now(), false);
    CHECK_EQ(f.client->unreadChats(10).size(), size_t(1));
    CHECK(f.mailbox->messages().empty()); // a chat is no letter
    CHECK_EQ(f.mailbox->watched(), std::vector<std::string>{f.peer});
}

TEST(a_post_to_a_chan_and_a_letter_to_oneself_make_no_pending_subscription) {
    Fixture f;
    const auto expires = bmc::now() + 3600;
    const bmc::Identity *chan = nullptr;
    for (const auto &i : f.vault->identities())
        if (i.address == f.chan)
            chan = &i;
    CHECK(chan != nullptr);
    const auto &me = f.vault->identities()[0];
    // The peer posts to the chan, and the user writes a note to themselves.
    fake::cacheObject(f.root,
                      bmc::Wire::encodeMessage(bmc::IdentityRef(f.peerVault.identities()[0]),
                                               bmc::Wire::publicIdentity(bmc::IdentityRef(*chan)),
                                               "To the chan", "Everybody", expires));
    fake::cacheObject(f.root,
                      bmc::Wire::encodeMessage(bmc::IdentityRef(me),
                                               bmc::Wire::publicIdentity(bmc::IdentityRef(me)),
                                               "Note", "To myself", expires));
    f.client->step(bmc::now(), false);
    CHECK_EQ(f.mailbox->messages().size(), size_t(2)); // both were read
    CHECK(f.mailbox->watched().empty());
}

TEST(writing_to_a_person_makes_a_pending_subscription_to_them) {
    Fixture f;
    const auto now = bmc::now();
    const auto draft = f.mailbox->saveDraft({}, f.me, f.peer, "Subject", "Body");
    f.mailbox->setSetting("draftkind:" + draft, "direct");
    CHECK(f.mailbox->watched().empty()); // a draft is not a letter sent
    f.client->sendLetter(draft, now);
    CHECK_EQ(f.mailbox->watched(), std::vector<std::string>{f.peer});
    CHECK(f.mailbox->subscriptions().empty());

    const auto second = f.peerVault.addIdentity("Second");
    f.client->sendChat(f.me, second, "hello", now);
    CHECK_EQ(f.mailbox->watched().size(), size_t(2));

    // Not a chan, nor a broadcast, nor a chat that is for a service.
    const auto toChan = f.mailbox->saveDraft({}, f.me, f.chan, "To the chan", "Body");
    f.mailbox->setSetting("draftkind:" + toChan, "direct");
    f.client->sendLetter(toChan, now);
    const auto news = f.mailbox->saveDraft({}, f.me, {}, "News", "Everyone");
    f.mailbox->setSetting("draftkind:" + news, "broadcast");
    f.client->sendLetter(news, now);
    const auto third = f.peerVault.addIdentity("Third");
    f.client->sendServiceChat(f.chan, third, "/address @ben", 3600, now);
    CHECK_EQ(f.mailbox->watched().size(), size_t(2));
}

TEST(the_people_the_user_already_deals_with_are_watched_by_the_migration) {
    Fixture f;
    const auto person = [&](const char *label) { return f.peerVault.addIdentity(label); };
    const auto contact = person("Contact"), writer = person("Writer"),
               archived = person("Archived"), trashed = person("Trashed"),
               poster = person("Poster"), written = person("Written"), chatty = person("Chatty"),
               declined = person("Declined"), formerly = person("Formerly"),
               subscribed = person("Subscribed");
    f.mailbox->saveContact(contact, "Contact");
    f.mailbox->store("in-1", writer, f.me, "S", "B", -1, "Inbox");
    f.mailbox->store("ar-1", archived, f.me, "S", "B", -1, "Archive");
    f.mailbox->store("se-1", f.me, written, "S", "B", -1, "Sent");
    f.mailbox->storeChat("chat-1", chatty, f.me, 1700000000123, "hello", bmc::now() + 3600);
    // Not people the user deals with: a trashed letter, a post in a chan, a post by a subscription,
    // a chan, a broadcast, oneself.
    f.mailbox->store("tr-1", trashed, f.me, "S", "B", -1, "Trash");
    f.mailbox->store("ch-1", poster, f.chan, "S", "B", -1, "Channels");
    f.mailbox->store("se-2", f.me, f.chan, "To the chan", "B", -1, "Sent");
    f.mailbox->store("se-3", f.me, "[Broadcast subscribers]", "News", "B", -1, "Sent");
    f.mailbox->store("self-1", f.me, f.me, "Note", "B", -1, "Inbox");
    // Left out on purpose: the user unsubscribed from one, subscribes to one, and one was followed
    // once (its posts are still here).
    f.mailbox->store("in-2", declined, f.me, "S", "B", -1, "Inbox");
    f.mailbox->subscribe(declined, "Declined");
    f.mailbox->unsubscribe(declined);
    f.mailbox->store("in-3", subscribed, f.me, "S", "B", -1, "Inbox");
    f.mailbox->subscribe(subscribed, "Subscribed");
    f.mailbox->store("in-4", formerly, f.me, "S", "B", -1, "Inbox");
    f.mailbox->store("post-1", formerly, formerly, "Post", "B", -1, "Broadcasts");

    CHECK_EQ(f.client->watchKnownPeople(), 5);
    const auto watched = f.mailbox->watched();
    CHECK(std::set<std::string>(watched.begin(), watched.end()) ==
          (std::set<std::string>{contact, writer, archived, written, chatty, subscribed}));
    CHECK_EQ(f.mailbox->subscriptions().size(), size_t(1)); // only the one on purpose
    // Again: nobody new. Somebody who has come into the book since is caught by the next run.
    CHECK_EQ(f.client->watchKnownPeople(), 0);
    const auto late = person("Late");
    f.mailbox->saveContact(late, "Late");
    CHECK_EQ(f.client->watchKnownPeople(), 1);
}
