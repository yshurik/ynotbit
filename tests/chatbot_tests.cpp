// The chat bot behind chat_responder: durable, rate limited, bounded. A real Session, offline,
// on a real mailbox; the clock is the test's own, in milliseconds.
#include "chat_bot.h"
#include "protocol.h"
#include "rotating_log.h"
#include "session.h"
#include "storage.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <iostream>
#include <memory>
#include <stdexcept>
using tools::ChatBot;
using tools::ChatBotOptions;
static void require(bool ok, const char *why) {
    if (!ok) {
        std::cerr << "FAIL: " << why << "\n";
        throw std::runtime_error(why);
    }
}
int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setOrganizationName("YnotbitTests");
    app.setApplicationName("ChatBot");
    try {
        QTemporaryDir temp;
        const auto root = temp.filePath("node"), vaultPath = temp.filePath("bot.bmvault"),
                   mailPath = temp.filePath("bot.bmmail");
        QDir().mkpath(root);
        bm::Vault vault;
        vault.create(vaultPath, "test password");
        const auto me = vault.addIdentity("Bot");
        const auto key = vault.addMailboxKey();
        {
            bm::Mailbox mailbox;
            mailbox.create(mailPath, key, vault.mailboxKey(key));
        }
        vault.lock();
        {
            QSettings config(root + "/desktop.ini", QSettings::IniFormat);
            config.setValue("vault", vaultPath);
            config.setValue("mailbox", mailPath);
        }
        bm::Session session(root, true);
        session.choosePendingVault(vaultPath);
        session.submitVaultPassword("test password");
        require(session.mailboxOpen(), "the bot's mailbox opens");

        const auto alice = bm::Protocol::identity("bot test alice").address;
        const auto bob = bm::Protocol::identity("bot test bob").address;
        const auto expiry = QDateTime::currentSecsSinceEpoch() + 3600;
        qint64 serial = 0;
        // A chat comes in, as the node's scan stores it, through a second handle on the mailbox.
        const auto arrive = [&](const QString &peer, const QString &text) {
            const auto id = "in-" + QString::number(++serial);
            vault.unlock(vaultPath, "test password");
            bm::Mailbox injected;
            injected.open(mailPath, vault.mailboxKey(key));
            injected.storeChat(id, peer, me, serial, text, expiry, false);
            injected.close();
            vault.lock();
            return id;
        };
        // Many at once, with the vault opened once: opening it is slow on purpose.
        const auto arriveMany = [&](const QString &peer, const QString &text, int count) {
            vault.unlock(vaultPath, "test password");
            bm::Mailbox injected;
            injected.open(mailPath, vault.mailboxKey(key));
            for (int i = 0; i < count; ++i) {
                const auto n = ++serial;
                injected.storeChat("in-" + QString::number(n), peer, me, n,
                                   text + " " + QString::number(i), expiry, false);
            }
            injected.close();
            vault.lock();
        };
        const auto outgoing = [&](const QString &peer) {
            QStringList bodies;
            for (const auto &m : session.chatMessages(me, peer))
                if (m.toMap()["outgoing"].toBool())
                    bodies << m.toMap()["body"].toString();
            return bodies;
        };
        QStringList log;
        const auto say = [&](const QString &line) { log << line; };
        const auto logged = [&](const QString &part) {
            for (const auto &line : log)
                if (line.contains(part))
                    return true;
            return false;
        };
        const ChatBot::Handler echo = [](const QString &, const QString &, const QString &text) {
            if (text == "/none")
                return QStringList{};
            if (text == "/three")
                return QStringList{"one", "two", "three"};
            if (text == "/long")
                return QStringList{QString(9000, 'x')}; // too long for a chat: it will not queue
            if (text == "/throws")
                throw std::runtime_error("the handler broke");
            return QStringList{"echo " + text};
        };
        ChatBotOptions quick;
        quick.delayMs = 0;
        quick.gapMs = 0;

        {
            // A chat that came is answered once, and read.
            ChatBot bot(session, echo, quick, say);
            bot.poll(1000);
            require(outgoing(alice).isEmpty() && bot.stats().received == 0,
                    "nothing to answer, nothing sent");
            arrive(alice, "hello");
            bot.poll(1000);
            require(outgoing(alice) == QStringList{"echo hello"} && session.chatUnread() == 0 &&
                        bot.stats().received == 1 && bot.stats().answered == 1 &&
                        bot.stats().working == 0,
                    "a chat is answered, then marked read");
            bot.poll(2000);
            require(outgoing(alice).size() == 1, "and not answered again");
            require(logged("hello"), "what came is logged");
        }
        {
            // The answer comes after the delay, and the chat stays unread until it is sent.
            ChatBotOptions slow = quick;
            slow.delayMs = 1000;
            ChatBot bot(session, echo, slow, say);
            arrive(alice, "later");
            bot.poll(5000);
            require(outgoing(alice).size() == 1 && session.chatUnread() == 1 &&
                        bot.stats().working == 1,
                    "a chat taken up waits out the delay, unread");
            bot.poll(5999);
            require(outgoing(alice).size() == 1, "not a moment early");
            bot.poll(6000);
            require(outgoing(alice).size() == 2 && session.chatUnread() == 0 &&
                        bot.stats().working == 0,
                    "and is answered when the delay is over");
        }
        {
            // What waited while the bot was not running is answered when it runs again; what was
            // read already is history and is not.
            const auto history = arrive(bob, "old news");
            session.markChatMessageRead(history);
            ChatBotOptions slow = quick;
            slow.delayMs = 1000;
            {
                ChatBot first(session, echo, slow, say);
                arrive(bob, "while down");
                first.poll(10000); // taken up, not yet answered: the bot stops here
            }
            require(outgoing(bob).isEmpty() && session.chatUnread() == 1,
                    "a chat not answered when the bot stopped is still unread");
            ChatBot second(session, echo, slow, say);
            second.poll(20000);
            second.poll(21000);
            require(outgoing(bob) == QStringList{"echo while down"} && session.chatUnread() == 0,
                    "a new bot answers it, once, and leaves history alone");
        }
        {
            // Several answers go in a row and in order; the chat is read after the last.
            ChatBotOptions spaced = quick;
            spaced.gapMs = 300;
            ChatBot bot(session, echo, spaced, say);
            const auto before = outgoing(alice).size();
            arrive(alice, "/three");
            bot.poll(30000);
            require(outgoing(alice).size() == before + 1 && session.chatUnread() == 1,
                    "the first answer goes at once, the chat stays unread");
            bot.poll(30299);
            require(outgoing(alice).size() == before + 1, "the next after the gap");
            bot.poll(30300);
            require(outgoing(alice).size() == before + 2 && session.chatUnread() == 1,
                    "the second");
            bot.poll(30600);
            const auto all = outgoing(alice);
            require(all.size() == before + 3 &&
                        all.mid(before) == QStringList{"one", "two", "three"} &&
                        session.chatUnread() == 0,
                    "the last in order, and then the chat is read");
        }
        {
            // A handler with nothing to say, or one that throws, costs nothing and stops nothing.
            ChatBot bot(session, echo, quick, say);
            const auto before = outgoing(alice).size();
            arrive(alice, "/none");
            arrive(alice, "/throws");
            arrive(alice, "still here");
            bot.poll(40000);
            require(outgoing(alice).size() == before + 1 && session.chatUnread() == 0 &&
                        bot.stats().failed == 1 && logged("the handler broke"),
                    "silence and a broken handler leave the bot answering the rest");
        }
        {
            // A peer that asks for more than its rate is answered up to it, and the rest is
            // dropped and read, not queued for later: a flood does not become a backlog.
            ChatBotOptions limited = quick;
            limited.rateBurst = 3;
            limited.ratePerMinute = 60;
            ChatBot bot(session, echo, limited, say);
            const auto before = outgoing(bob).size();
            for (int i = 0; i < 5; ++i)
                arrive(bob, "flood " + QString::number(i));
            bot.poll(50000);
            require(outgoing(bob).size() == before + 3 && session.chatUnread() == 0 &&
                        bot.stats().limited == 2 && logged("too many"),
                    "answers stop at the peer's burst, and the rest is dropped and said");
            arrive(alice, "someone else");
            bot.poll(50000);
            require(outgoing(alice).size() > 0 && outgoing(alice).last() == "echo someone else",
                    "another peer is not held back by it");
            arrive(bob, "after two seconds");
            bot.poll(52000);
            require(outgoing(bob).size() == before + 4 &&
                        outgoing(bob).last() == "echo after two seconds",
                    "and the peer gets more as the minutes go");
        }
        {
            // All peers together are limited too.
            ChatBotOptions limited = quick;
            limited.globalBurst = 2;
            limited.globalPerMinute = 60;
            ChatBot bot(session, echo, limited, say);
            const auto before = outgoing(alice).size() + outgoing(bob).size();
            arrive(alice, "one of many");
            arrive(bob, "two of many");
            arrive(alice, "three of many");
            bot.poll(60000);
            require(outgoing(alice).size() + outgoing(bob).size() == before + 2 &&
                        bot.stats().limited >= 1 && session.chatUnread() == 0,
                    "a flood from many peers is held to the total");
        }
        {
            // An answer that will not queue is tried again, a few times, and then given up and
            // read.
            ChatBotOptions patient = quick;
            patient.retryMs = 1000;
            patient.maxAttempts = 3;
            ChatBot bot(session, echo, patient, say);
            arrive(alice, "/long");
            bot.poll(70000);
            require(session.chatUnread() == 1 && bot.stats().working == 1 &&
                        bot.stats().failed == 0,
                    "a failing answer is tried again, the chat still unread");
            bot.poll(70999);
            bot.poll(71000);
            require(session.chatUnread() == 1 && bot.stats().failed == 0, "twice");
            bot.poll(72000);
            require(session.chatUnread() == 0 && bot.stats().failed == 1 &&
                        bot.stats().working == 0 && logged("cannot answer"),
                    "and the third failure gives it up, said, and the chat is read");
        }
        {
            // Memory is bounded: a thousand chats waiting are not a thousand jobs.
            ChatBotOptions slow = quick;
            slow.delayMs = 100000;
            slow.batch = 20;
            ChatBot bot(session, echo, slow, say);
            arriveMany(alice, "backlog", 120);
            bot.poll(80000);
            require(bot.stats().working == 20 && session.chatUnread() == 120,
                    "no more chats are held than the batch; the rest wait in the mailbox");
            const auto before = outgoing(alice).size();
            bot.poll(80000 + 100000);
            require(outgoing(alice).size() == before + 20 && bot.stats().working == 20 &&
                        session.chatUnread() == 100,
                    "a batch is answered and the next one taken up");
            // The rest, quickly, as a bot with no delay would, following ten answers at a time.
            ChatBotOptions fastOptions = quick;
            fastOptions.track = 10;
            fastOptions.ratePerMinute = 0; // one peer sent them all: no limit, to see the rest
            fastOptions.globalPerMinute = 0;
            ChatBot fast(session, echo, fastOptions, say);
            for (int i = 0; i < 10 && session.chatUnread() > 0; ++i)
                fast.poll(300000 + i);
            require(session.chatUnread() == 0 && fast.stats().working == 0 &&
                        fast.stats().answered == 100 && fast.stats().tracked <= 10 + 1,
                    "the backlog is worked off, and what the bot remembers stays small");
        }
        {
            // Where an answer got to is said as it changes, and forgotten when it is final.
            QStringList seen;
            ChatBot bot(session, echo, quick, [&](const QString &line) { seen << line; });
            arrive(bob, "state check");
            bot.poll(400000);
            QString replyId;
            for (const auto &m : session.chatMessages(me, bob))
                if (m.toMap()["outgoing"].toBool() &&
                    m.toMap()["body"].toString() == "echo state check")
                    replyId = m.toMap()["id"].toString();
            require(!replyId.isEmpty() && bot.stats().tracked >= 1,
                    "an answer on its way is tracked");
            vault.unlock(vaultPath, "test password");
            bm::Mailbox injected;
            injected.open(mailPath, vault.mailboxKey(key));
            injected.setDelivery(replyId, "acknowledged", "Recipient acknowledgment received");
            injected.close();
            vault.lock();
            bot.poll(401000);
            bool said = false;
            for (const auto &line : seen)
                said = said || line.contains("acknowledged");
            const auto tracked = bot.stats().tracked;
            bot.poll(402000);
            require(said && bot.stats().tracked < tracked + 1 && bot.stats().tracked <= 2,
                    "an answer's new state is logged, and a final one is forgotten");
        }
        {
            // A log that is kept for months keeps to a size: when the next line would pass it, the
            // log becomes log.1, log.1 becomes log.2, and the oldest of the kept ones goes.
            const auto path = temp.filePath("service.log");
            tools::RotatingLog rotating(200, 3);
            require(rotating.open(path), "a log opens");
            for (int i = 0; i < 40; ++i)
                rotating.write(QString("line %1 of the log, padded to twenty").arg(i, 3));
            QStringList present;
            for (const auto &suffix : {"", ".1", ".2", ".3", ".4"})
                if (QFile::exists(path + suffix))
                    present << suffix;
            require(present == QStringList{"", ".1", ".2", ".3"},
                    "the log and the three kept before it, and no more");
            QFile newest(path);
            require(newest.open(QIODevice::ReadOnly), "the log is there");
            const auto text = QString::fromUtf8(newest.readAll());
            require(text.contains("line  39 ") && !text.contains("line   0 "),
                    "the newest lines are in the log, the oldest are gone");
            for (const auto &suffix : {"", ".1", ".2", ".3"})
                require(QFileInfo(path + suffix).size() <= 200 + 60,
                        "no log grows past its size by more than a line");
            tools::RotatingLog again(200, 3);
            require(again.open(path), "a log opens again");
            again.write("after a restart");
            // It goes on where it was: the new line follows the old ones, in the log or, if that
            // was nearly full, in the log after it was rotated.
            const auto read = [&](const QString &name) {
                QFile file(name);
                return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                                      : QString();
            };
            require(read(path).contains("after a restart") &&
                        (read(path) + read(path + ".1")).contains("line  39 ") &&
                        !QFileInfo(path + ".4").exists(),
                    "and goes on where it was");
        }
        std::cout << "PASS the chat bot answers what waits, durably, within its limits\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
