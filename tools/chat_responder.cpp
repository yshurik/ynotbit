// A chat peer to try ynotbit's chats against: it has an address, answers every chat
// it gets, and says what it does. Runs headless, on a vault of its own and with a
// node of its own, so ynotbit and it chat as two people would.
//
//   cmake --build build --target chat_responder
//   build/chat_responder
//
// It prints its address. In ynotbit: Chats > New chat, paste the address, write to
// it; say /help to it. By default its node links only to ynotbit's own node on this
// machine (127.0.0.1:8444) and leaves the public network alone, so it works offline.
// Start ynotbit first: the responder's node calls its peer once when it starts, and if
// nothing listens yet it tries again only about a minute later.
//
//   --phrase <text>     what its address comes from: one phrase, one address
//                       (default "ynotbit chat responder")
//   --peer <ip:port>    the node to link to (default 127.0.0.1:8444, ynotbit's)
//   --public            also join the public network, not only the peer
//   --delay <seconds>   how long it takes to answer (default 1)
//   --data-dir <dir>    where its vault, mailbox and objects are kept
//   --fresh             start from nothing: forget them first
#include "key_search.h"
#include "publish_broadcast.h"
#include "session.h"
#include "storage.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <csignal>
#include <iostream>
#include <vector>
extern "C" {
int ntb_daemon(int argc, char **argv);
}
namespace {
volatile std::sig_atomic_t interrupted = 0;
// The vault holds only keys made from a phrase anyone can read here: no secret to keep.
const QByteArray kPassword = "ynotbit chat responder";
void say(const QString &message) {
    std::cout << QDateTime::currentDateTime().toString("hh:mm:ss ").toStdString()
              << message.toStdString() << std::endl;
}
// An address short enough for a line.
QString brief(const QString &address) {
    return address.left(12) + "…";
}
QString oneLine(const QString &text) {
    const auto line = text.simplified();
    return line.size() > 80 ? line.left(79) + "…" : line;
}
// What it says to what it is told: a few commands to try a chat with, else an echo.
QStringList answers(const QString &text) {
    const auto words = text.trimmed().split(' ', Qt::SkipEmptyParts);
    const auto command = words.value(0).toLower();
    if (command == "/help")
        return {"I answer chats. Try:\n\n"
                "- `/ping`: pong\n"
                "- `/time`: the time here\n"
                "- `/md`: some Markdown\n"
                "- `/long`: a long answer\n"
                "- `/many [n]`: n short answers in a row\n\n"
                "Anything else I say back."};
    if (command == "/ping")
        return {"pong"};
    if (command == "/time")
        return {QDateTime::currentDateTime().toString("dddd d MMMM yyyy, hh:mm:ss")};
    if (command == "/md")
        return {"# Markdown\n\n**Bold**, *italic*, ~~struck~~ and `code`.\n\n"
                "1. one\n2. two\n3. three\n\n> a quote\n\n```\na block of code\n```\n\n"
                "A [link](https://example.org)."};
    if (command == "/long") {
        QStringList paragraphs;
        for (int i = 1; i <= 6; ++i)
            paragraphs << QString("**Paragraph %1.** The quick brown fox jumps over the lazy "
                                  "dog, and keeps on running through the whole afternoon, "
                                  "until the line must break more than once, and then again.")
                              .arg(i);
        return {paragraphs.join("\n\n")};
    }
    if (command == "/many") {
        const int n = std::clamp(words.value(1).toInt() > 0 ? words.value(1).toInt() : 5, 1, 20);
        QStringList each;
        for (int i = 1; i <= n; ++i)
            each << QString("Message %1 of %2").arg(i).arg(n);
        return each;
    }
    return {"You said: " + text.left(4000)};
}
// Answers the chats that come, after a delay, and reports how its own are getting on.
class Bot {
  public:
    Bot(bm::Session &session, QString me, int delayMs)
        : session_(session), me_(std::move(me)), delayMs_(delayMs) {}
    void poll() {
        for (const auto &entry : session_.chatConversations()) {
            const auto conversation = entry.toMap();
            if (conversation["local"].toString() != me_)
                continue;
            const auto peer = conversation["address"].toString();
            // Its newest messages are all it needs, and a chat is kept for good: not the whole
            // of it at every poll.
            for (const auto &value : session_.chatMessages(me_, peer, 100)) {
                const auto message = value.toMap();
                const auto id = message["id"].toString();
                if (message["outgoing"].toBool()) {
                    const auto state = message["state"].toString();
                    if (states_.value(id) != state) {
                        states_[id] = state;
                        say("   to " + brief(peer) + ": " + oneLine(message["body"].toString()) +
                            "  [" + QString(state).replace('_', ' ') + "]");
                    }
                    continue;
                }
                if (seen_.contains(id))
                    continue;
                seen_.insert(id);
                if (!started_)
                    continue; // what was there before it started is history
                say("<- " + brief(peer) + ": " + oneLine(message["body"].toString()));
                qint64 at = QDateTime::currentMSecsSinceEpoch() + delayMs_;
                for (const auto &reply : answers(message["body"].toString())) {
                    due_.append({at, peer, reply});
                    at += 300; // in a row, in order
                }
            }
            session_.markChatRead(me_, peer);
        }
        started_ = true;
        const auto now = QDateTime::currentMSecsSinceEpoch();
        while (!due_.isEmpty() && due_.first().at <= now) {
            const auto item = due_.takeFirst();
            if (!session_.sendChat(me_, item.peer, item.text))
                say("cannot answer " + brief(item.peer) + ": " + session_.error());
        }
    }

  private:
    struct Due {
        qint64 at;
        QString peer, text;
    };
    bm::Session &session_;
    QString me_;
    int delayMs_;
    bool started_ = false;
    QSet<QString> seen_;
    QHash<QString, QString> states_;
    QList<Due> due_;
};
} // namespace
int main(int argc, char **argv) {
    // Session runs its network node as this same executable with --node. Unless it is to
    // join the public network, the node links to its peer alone: no bootstrap by default
    // peers or DNS, no other peers (-b -B -e).
    if (argc > 1 && std::string(argv[1]) == "--node") {
        std::vector<char *> args(argv + 1, argv + argc);
        static char noDefaults[] = "-b", noDns[] = "-B", onlyExplicit[] = "-e";
        if (qEnvironmentVariableIsEmpty("YNOTBIT_RESPONDER_PUBLIC")) {
            args.push_back(noDefaults);
            args.push_back(noDns);
            args.push_back(onlyExplicit);
        }
        args.push_back(nullptr);
        return ntb_daemon(int(args.size()) - 1, args.data());
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setOrganizationName("YnotbitTools");
    app.setApplicationName("chat_responder");
    const auto args = app.arguments();
    const QString tool = "chat_responder";
    if (args.contains("--help") || args.contains("-h")) {
        std::cout << "usage: chat_responder [--phrase <text>] [--peer <ip:port>] [--public]\n"
                     "                      [--delay <seconds>] [--data-dir <dir>] [--fresh]\n";
        return 0;
    }
    const auto phrase = tools::option(args, "--phrase").isEmpty()
                            ? QString("ynotbit chat responder")
                            : tools::option(args, "--phrase");
    const bool joinPublic = args.contains("--public");
    const auto peer =
        args.contains("--peer") ? tools::option(args, "--peer") : QString("127.0.0.1:8444");
    if (!bm::Session::validEndpoint(peer) || (peer.isEmpty() && !joinPublic))
        return tools::fail(tool, "--peer takes an IP:port, like 127.0.0.1:8444");
    bool delayOk = true;
    const auto delayText = tools::option(args, "--delay");
    const double delay = delayText.isEmpty() ? 1.0 : delayText.toDouble(&delayOk);
    if (!delayOk || delay < 0)
        return tools::fail(tool, "--delay takes a number of seconds");
    // One folder for each phrase: its vault, mailbox and the node's objects.
    auto base = tools::option(args, "--data-dir");
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const auto root = QDir(base).absoluteFilePath(QString::fromLatin1(
        QCryptographicHash::hash(phrase.toUtf8(), QCryptographicHash::Sha256).toHex().left(8)));
    if (args.contains("--fresh"))
        QDir(root).removeRecursively();
    QDir().mkpath(root);
    const auto vaultPath = root + "/responder.bmvault", mailPath = root + "/responder.bmmail";
    const auto identity = bm::phraseIdentity(phrase, "Chat responder", 0);
    try {
        if (!QFile::exists(vaultPath)) {
            bm::Vault vault;
            vault.create(vaultPath, kPassword);
            vault.addIdentity(identity);
            const auto key = vault.addMailboxKey();
            bm::Mailbox mailbox;
            mailbox.create(mailPath, key, vault.mailboxKey(key));
        }
    } catch (const std::exception &e) {
        return tools::fail(tool, e.what());
    }
    {
        QSettings config(root + "/desktop.ini", QSettings::IniFormat);
        config.setValue("vault", vaultPath);
        config.setValue("mailbox", mailPath);
        config.setValue("listen", false); // it calls out; ynotbit's node is the one listening
        config.setValue("peer", peer);
    }
    if (joinPublic)
        qputenv("YNOTBIT_RESPONDER_PUBLIC", "1");
    std::signal(SIGINT, [](int) { interrupted = 1; });
    std::signal(SIGTERM, [](int) { interrupted = 1; });
    try {
        bm::Session session(root, false);
        session.choosePendingVault(vaultPath);
        session.submitVaultPassword(QString::fromLatin1(kPassword));
        if (!session.mailboxOpen())
            return tools::fail(tool, "cannot open its mailbox: " + session.error());
        say("Chat responder");
        say("Address:  " + identity.address);
        say(joinPublic ? "Node:     joins the public network" +
                             (peer.isEmpty() ? QString() : ", and links to " + peer)
                       : "Node:     links to " + peer + " only, the public network left alone");
        say("Data:     " + root);
        say("In ynotbit: Chats > New chat > paste the address. Say /help to it. Ctrl-C quits.");
        Bot bot(session, identity.address, int(delay * 1000));
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, [&] {
            if (interrupted) {
                app.quit();
                return;
            }
            bot.poll();
        });
        poll.start(300);
        QTimer report;
        QString lastError;
        QObject::connect(&report, &QTimer::timeout, [&] {
            say(session.status());
            if (session.error() != lastError && !session.error().isEmpty())
                say("Problem: " + session.error());
            lastError = session.error();
        });
        report.start(30000);
        const int code = app.exec();
        say("Bye.");
        session.lock();
        return code;
    } catch (const std::exception &e) {
        return tools::fail(tool, e.what());
    }
}
