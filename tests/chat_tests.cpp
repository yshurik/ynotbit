#include "chat_format.h"
#include "storage.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QTemporaryDir>
#include <QThread>
#include <stdexcept>
#include <iostream>
using namespace bm;
static void require(bool ok, const char *why) {
    if (!ok) throw std::runtime_error(why);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        ChatPayload payload;
        const auto encoded = chatMessageBody(1, "Hi **Alice**");
        require(parseChatMessage({}, encoded, &payload) == ChatParseResult::Chat &&
                    payload.seqno == 1 && payload.body == "Hi **Alice**", "chat round trip");
        // Sequence numbers are milliseconds: far past what a small count needs, and
        // exact when they come back out of the JSON.
        require(parseChatMessage({}, chatMessageBody(1760000000123, "clock"), &payload) ==
                        ChatParseResult::Chat &&
                    payload.seqno == 1760000000123,
                "a sequence number in milliseconds survives the wire format");
        require(parseChatMessage("subject", encoded, &payload) == ChatParseResult::Letter,
                "subject makes the message a letter");
        require(parseChatMessage({}, "ordinary letter", &payload) == ChatParseResult::Letter,
                "ordinary letters stay letters");
        require(parseChatMessage({}, "{\"app\":\"ynotbit\",\"type\":\"chat\"}", &payload) ==
                    ChatParseResult::InvalidChat, "malformed chat is not a letter");
        QTemporaryDir temp;
        Vault vault;
        vault.create(temp.filePath("vault"), "test password");
        auto key = vault.addMailboxKey();
        Mailbox mailbox;
        mailbox.create(temp.filePath("mailbox"), key, vault.mailboxKey(key));
        const auto expiry = QDateTime::currentSecsSinceEpoch() + 86400;
        const auto beforeQueue = QDateTime::currentMSecsSinceEpoch();
        const auto id = mailbox.queueChat("BM-local", "BM-peer", "hello", expiry);
        const auto chats = mailbox.chatMessages("BM-local", "BM-peer");
        require(chats.size() == 1 && chats.first().body == "hello" && chats.first().id == id,
                "outgoing chat stored");
        require(chats.first().seqno >= beforeQueue &&
                    chats.first().seqno <= QDateTime::currentMSecsSinceEpoch(),
                "a chat's sequence number is the sender's clock in milliseconds");
        require(mailbox.outbox().size() == 1 && mailbox.outbox().first().chat &&
                    mailbox.outbox().first().expires == expiry &&
                    mailbox.outbox().first().message.body.contains("\"type\":\"chat\""),
                "outgoing delivery uses 24-hour expiry and chat JSON");
        mailbox.storeRouted("hash-routed", "BM-peer", "BM-local", {},
                            chatMessageBody(2, "routed"), 4, "Inbox", expiry);
        require(mailbox.messageCount("Inbox", {}) == 0 &&
                    mailbox.chatMessages("BM-local", "BM-peer").size() == 2,
                "incoming chat routes away from Inbox");
        mailbox.storeRouted("hash-invalid", "BM-peer", "BM-local", {},
                            "{\"app\":\"ynotbit\",\"type\":\"chat\"}",
                            5, "Inbox", expiry);
        require(mailbox.messageCount("Inbox", {}) == 0, "invalid chat is not a letter");
        mailbox.storeChat("hash-a", "BM-peer", "BM-local", 1, "incoming", expiry, false);
        require(mailbox.chatMessages("BM-local", "BM-peer").size() == 3,
                "incoming chat stored beside outgoing");
        mailbox.storeChat("hash-b", "BM-peer", "BM-local", 1, "replay", expiry, false);
        require(mailbox.chatMessages("BM-local", "BM-peer").size() == 3,
                "duplicate sender sequence ignored");
        // Sent in a burst, the numbers still only grow: the clock may not have moved.
        const auto burstFrom = QDateTime::currentMSecsSinceEpoch();
        for (int i = 0; i < 40; ++i)
            mailbox.queueChat("BM-local", "BM-burst", "n" + QString::number(i), expiry);
        const auto burst = mailbox.chatMessages("BM-local", "BM-burst");
        bool growing = burst.size() == 40;
        for (int i = 1; i < burst.size(); ++i)
            growing = growing && burst[i].seqno > burst[i - 1].seqno;
        require(growing && burst.first().seqno >= burstFrom,
                "sequence numbers sent in one millisecond still increase");
        // A long thread is read a window at a time: its newest messages, oldest first, and
        // whether older ones are left.
        bool older = false;
        const auto newest = mailbox.chatMessages("BM-local", "BM-burst", 10, &older);
        require(newest.size() == 10 && older && newest.first().id == burst[30].id &&
                    newest.last().id == burst[39].id,
                "a window is the newest messages of a thread, oldest first, with older ones left");
        const auto whole = mailbox.chatMessages("BM-local", "BM-burst", 40, &older);
        require(whole.size() == 40 && !older && whole.first().id == burst.first().id,
                "a window as long as the thread is all of it, with nothing older");
        require(mailbox.chatMessages("BM-local", "BM-burst", 100, &older).size() == 40 && !older,
                "a window longer than the thread is the thread");
        require(mailbox.chatMessages("BM-local", "BM-nobody", 10, &older).isEmpty() && !older,
                "a thread with no messages has a window of none, and nothing older");
        // What the peer sent and nobody has looked at is unread; our own never is.
        const auto thread = [&](const QString &peer) {
            for (const auto &summary : mailbox.chatSummaries())
                if (summary.local == "BM-local" && summary.peer == peer)
                    return summary;
            return ChatSummary{};
        };
        require(thread("BM-peer").unread == 2 && thread("BM-peer").body == "incoming" &&
                    !thread("BM-peer").outgoing && mailbox.chatUnread() == 2,
                "incoming chats are unread, and the newest is the conversation's summary");
        mailbox.markChatRead("BM-local", "BM-peer");
        require(thread("BM-peer").unread == 0 && mailbox.chatUnread() == 0,
                "reading a conversation clears its unread count");
        mailbox.storeChat("hash-c", "BM-peer", "BM-local", 3, "later", expiry, false);
        require(thread("BM-peer").unread == 1, "a chat after reading is unread again");
        mailbox.queueChat("BM-local", "BM-peer", "reply", expiry);
        require(thread("BM-peer").outgoing && thread("BM-peer").body == "reply" &&
                    thread("BM-peer").unread == 1 && thread("BM-peer").state == "queued",
                "our own reply is the newest, carries its delivery state, and is not unread");
        mailbox.storeChat("hash-d", "BM-friend", "BM-local", 1, "from a friend", expiry, false);
        require(mailbox.chatSummaries().first().peer == "BM-friend" && mailbox.chatUnread() == 2,
                "the conversation with the newest message comes first");
        {
            // What a service has still to answer: the chats nobody has looked at, oldest first
            // across every conversation, and each marked read by itself, by its id.
            mailbox.storeChat("q-1", "BM-friend", "BM-local", 2, "first", expiry, false);
            mailbox.storeChat("q-2", "BM-friend", "BM-local", 3, "second", expiry, false);
            const auto waiting = mailbox.unreadChats(100);
            require(waiting.size() == mailbox.chatUnread() && waiting.size() >= 2 &&
                        waiting[waiting.size() - 2].id == "q-1" && waiting.last().id == "q-2" &&
                        waiting.last().peerAddress == "BM-friend" &&
                        waiting.last().localAddress == "BM-local" &&
                        waiting.last().body == "second" && waiting.last().seqno == 3 &&
                        !waiting.last().outgoing,
                    "the unread chats come oldest first, from every conversation");
            for (const auto &chat : waiting)
                require(!chat.outgoing, "what we sent is never waiting to be answered");
            require(mailbox.unreadChats(1).size() == 1 &&
                        mailbox.unreadChats(1).first().id == waiting.first().id,
                    "no more than asked for");
            const int before = mailbox.chatUnread();
            require(mailbox.markChatMessageRead("q-1") && mailbox.chatUnread() == before - 1 &&
                        mailbox.unreadChats(100).last().id == "q-2",
                    "a chat marked read by its id leaves the others unread");
            require(!mailbox.markChatMessageRead("q-1") && !mailbox.markChatMessageRead("nobody") &&
                        !mailbox.markChatMessageRead(id) && mailbox.chatUnread() == before - 1,
                    "reading what is read, what is ours or what is not there changes nothing");
            require(mailbox.markChatMessageRead("q-2") && mailbox.chatUnread() == before - 2,
                    "and the last one");
        }
        // A chat lives on the network until its expiry, and in its owner's mailbox until its
        // owner deletes it: the expiry ends a chat's delivery, never the chat.
        const auto brief = QDateTime::currentSecsSinceEpoch() + 2;
        const auto briefId = mailbox.queueChat("BM-local", "BM-brief", "sent shortly", brief);
        mailbox.storeChat("hash-e", "BM-brief", "BM-local", 1, "heard shortly", brief, false);
        QThread::msleep(2200);
        require(QDateTime::currentSecsSinceEpoch() >= brief, "the clock passed the chats' expiry");
        require(mailbox.chatMessages("BM-local", "BM-brief").size() == 2,
                "chats past their expiry stay in their thread");
        require(thread("BM-brief").body == "heard shortly" && thread("BM-brief").unread == 1 &&
                    mailbox.chatUnread() == 3,
                "chats past their expiry stay in the list, and unread ones stay unread");
        NetworkJob expiredJob, liveJob;
        expiredJob.owner = briefId;
        liveJob.owner = id;
        for (auto *job : {&expiredJob, &liveJob}) {
            job->kind = "message";
            job->address = "BM-peer";
            job->payload = "object";
            job->expires = brief;
            mailbox.addJob(*job);
        }
        mailbox.endChatDeliveries(QDateTime::currentSecsSinceEpoch());
        require(mailbox.chatMessages("BM-local", "BM-brief").size() == 2 &&
                    mailbox.chatMessages("BM-local", "BM-peer").size() == 5,
                "ending the delivery of expired chats keeps every chat");
        require(mailbox.jobs().size() == 1 && mailbox.jobs().first().owner == id,
                "it ends the network work of the expired chats, and of no other");
        // A finished delivery is not work to plan or to scan for: the outbox lists the
        // deliveries still going, however long the chats go on.
        const auto going = mailbox.outbox().size();
        mailbox.setDelivery(briefId, "expired", "Chat expired after 24 hours.");
        require(mailbox.outbox().size() == going - 1 &&
                    mailbox.chatMessages("BM-local", "BM-brief").first().state == "expired",
                "an expired delivery leaves the outbox, and its chat shows it");
        mailbox.setDelivery(id, "acknowledged", "Recipient acknowledgment received");
        require(mailbox.outbox().size() == going - 2 &&
                    mailbox.chatMessages("BM-local", "BM-peer").first().state == "acknowledged",
                "so does an acknowledged one");
        // A backup is the mailbox, chats and the deliveries still going with it.
        const auto backup = temp.filePath("backup");
        mailbox.backup(backup, vault.mailboxKey(key));
        Mailbox restored;
        restored.open(backup, vault.mailboxKey(key));
        require(restored.chatMessages("BM-local", "BM-peer").size() == 5 &&
                    restored.chatMessages("BM-local", "BM-brief").size() == 2 &&
                    restored.chatSummaries().size() == 4 && restored.outbox().size() == going - 2,
                "a backup holds the chats, the expired ones too, and their deliveries going");
        // What a restored mailbox sends next must not repeat a sequence number a peer
        // already holds.
        qint64 highest = 0;
        for (const auto &message : mailbox.chatMessages("BM-local", "BM-peer"))
            if (message.outgoing)
                highest = std::max(highest, message.seqno);
        QThread::msleep(5);
        restored.queueChat("BM-local", "BM-peer", "after the restore", expiry);
        const auto afterRestore = restored.chatMessages("BM-local", "BM-peer");
        require(afterRestore.size() == 6 && afterRestore.last().seqno > highest,
                "a restored mailbox does not repeat the sequence numbers it sent before");
        restored.close();
        std::cout << "PASS chat format, storage and expiration\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
