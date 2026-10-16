// A chat peer that stays up: it has an address, answers the chats it gets, and says what it does.
// Runs headless, on a vault of its own and with a node of its own, so ynotbit and it chat as two
// people would. It is a test peer for ynotbit's chats, and made to run for months as a service:
// what came while it was down is answered when it is back, what it remembers is bounded, a peer
// cannot make it answer without limit, and a node that stops doing its work is started again.
//
//   cmake --build build --target chat_responder
//   build/chat_responder
//
// It prints its address. In ynotbit: Chats > New chat, paste the address, write to it; say /help
// to it. By default its node links only to ynotbit's own node on this machine (127.0.0.1:8444)
// and leaves the public network alone, so it works offline; it dials that node again every
// half minute at most, so quitting and starting ynotbit costs it seconds.
//
// For months, on a machine of its own:
//
//   build/chat_responder --public --listen --phrase-file secret.txt --log responder.log \
//                        --status-file status.json
//
//   --phrase <text>       what its address comes from: one phrase, one address (default "ynotbit
//                         chat responder", which is public: anyone can read what is sent to it)
//   --phrase-file <file>  the phrase from a file, or from $YNOTBIT_RESPONDER_PHRASE: not in the
//                         process list, and the vault's password comes from it, so what is on the
//                         disk is of no use without it
//   --peer <ip:port>      the node to link to (default 127.0.0.1:8444, ynotbit's); "" for none
//   --public              also join the public network, not only the peer, with more peers
//   --listen              accept incoming connections (with --public), and ask the router for the
//                         port with UPnP; --port <n> for a port other than 8444
//   --max-outgoing <n>    outgoing peers wanted (8; 12 with --public), at most 64
//   --max-incoming <n>    incoming peers accepted (8; 24 with --public), at most 512
//   --watchdog <minutes>  restart its node when it has had no peer that long (0 never; 15 with
//                         --public); a node whose status stopped is always restarted
//   --delay <seconds>     how long it takes to answer (default 1)
//   --rate <n>            answers a peer gets a minute, after a first burst of --burst (30 and
//                         30); --global-rate and --global-burst for all peers together (300, 200);
//                         0 sets no limit
//   --silent              answer nothing: a peer that only listens and logs what it is sent
//   --hello <address>     say hello to an address once, when it starts: a test of a link, or
//                         the probe of a monitor that waits for the answer
//   --log <file>          also write what it says to a file, rotated at 10 MB, five kept
//   --status-file <file>  a JSON file made again every ten seconds: peers, counts, uptime
//   --data-dir <dir>      where its vault, mailbox and objects are kept
//   --fresh               start from nothing: forget them first
#include "chat_bot.h"
#include "key_search.h"
#include "publish_broadcast.h"
#include "rotating_log.h"
#include "session.h"
#include "storage.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
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
// The vault of the default identity, whose phrase anyone can read here: no secret to keep. With a
// phrase of its own (--phrase-file) the vault's password comes from the phrase instead.
const QByteArray kPassword = "ynotbit chat responder";
// Where it says what it does: standard output and, with --log, a file kept to a size.
class Logger {
  public:
    void open(const QString &path) {
        if (!file_.open(path))
            std::cerr << "cannot write the log " << path.toStdString() << "\n";
    }
    void say(const QString &message) {
        const auto line = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss ") + message;
        std::cout << line.toStdString() << std::endl;
        file_.write(line);
    }

  private:
    tools::RotatingLog file_; // 10 MB, five kept
};
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
// The node's own status: what is on its page in status.json.
QJsonObject nodeStatus(const QString &root) {
    QFile file(root + "/status.json");
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.read(4096)).object()
                                          : QJsonObject();
}
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
        std::cout
            << "usage: chat_responder [--phrase <text> | --phrase-file <file>] [--peer <ip:port>]\n"
               "                      [--public] [--listen] [--port <n>] [--max-outgoing <n>]\n"
               "                      [--max-incoming <n>] [--watchdog <minutes>]\n"
               "                      [--delay <seconds>] [--rate <n>] [--burst <n>]\n"
               "                      [--global-rate <n>] [--global-burst <n>] [--silent]\n"
               "                      [--hello <address>] [--log <file>] [--status-file <file>]\n"
               "                      [--data-dir <dir>] [--fresh]\n";
        return 0;
    }
    // A number option, or the fallback; false when it is not a number within the range.
    bool numbersOk = true;
    const auto number = [&](const char *name, double fallback, double low, double high) {
        const auto text = tools::option(args, name);
        if (text.isEmpty())
            return fallback;
        bool ok = false;
        const double value = text.toDouble(&ok);
        if (!ok || value < low || value > high) {
            std::cerr << tool.toStdString() << ": " << name << " takes a number from " << low
                      << " to " << high << "\n";
            numbersOk = false;
            return fallback;
        }
        return value;
    };
    // The phrase an address comes from: a file or the environment keep it out of the process list.
    QString phrase = "ynotbit chat responder";
    bool secret = false;
    if (const auto file = tools::option(args, "--phrase-file"); !file.isEmpty()) {
        QFile source(file);
        if (!source.open(QIODevice::ReadOnly))
            return tools::fail(tool, "cannot read " + file);
        phrase = QString::fromUtf8(source.readLine()).trimmed();
        secret = true;
    } else if (!qEnvironmentVariableIsEmpty("YNOTBIT_RESPONDER_PHRASE")) {
        phrase = qEnvironmentVariable("YNOTBIT_RESPONDER_PHRASE").trimmed();
        secret = true;
    } else if (!tools::option(args, "--phrase").isEmpty()) {
        phrase = tools::option(args, "--phrase");
    }
    if (phrase.isEmpty())
        return tools::fail(tool, "the phrase is empty");
    const bool joinPublic = args.contains("--public");
    const bool listen = args.contains("--listen");
    const auto peer =
        args.contains("--peer") ? tools::option(args, "--peer") : QString("127.0.0.1:8444");
    if (!bm::Session::validEndpoint(peer) || (peer.isEmpty() && !joinPublic))
        return tools::fail(tool, "--peer takes an IP:port, like 127.0.0.1:8444");
    const double delay = number("--delay", 1.0, 0, 3600);
    const int port = int(number("--port", 0, 1, 65535));
    const int maxOutgoing = int(number("--max-outgoing", joinPublic ? 12 : 0, 1, 64));
    const int maxIncoming = int(number("--max-incoming", joinPublic ? 24 : 0, 1, 512));
    const int watchdog = int(number("--watchdog", joinPublic ? 15 : 0, 0, 24 * 60));
    tools::ChatBotOptions options;
    options.delayMs = int(delay * 1000);
    options.ratePerMinute = number("--rate", options.ratePerMinute, 0, 1e6);
    options.rateBurst = int(number("--burst", options.rateBurst, 1, 1e6));
    options.globalPerMinute = number("--global-rate", options.globalPerMinute, 0, 1e6);
    options.globalBurst = int(number("--global-burst", options.globalBurst, 1, 1e6));
    if (!numbersOk)
        return 1;
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
    // The default identity's password is public, like its phrase; a phrase of one's own makes it
    // secret, and the vault's with it.
    const QByteArray password =
        secret ? QCryptographicHash::hash("ynotbit chat responder vault\n" + phrase.toUtf8(),
                                          QCryptographicHash::Sha256)
                     .toHex()
               : kPassword;
    try {
        if (!QFile::exists(vaultPath)) {
            bm::Vault vault;
            vault.create(vaultPath, password);
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
        // It calls out; ynotbit's node is the one listening, unless it is asked to listen too.
        config.setValue("listen", listen);
        config.setValue("upnp", listen);
        config.setValue("peer", peer);
        for (const auto &[key, value] : {std::pair{"listenPort", port},
                                         {"maxOutgoing", maxOutgoing},
                                         {"maxIncoming", maxIncoming}})
            if (value > 0)
                config.setValue(key, value);
            else
                config.remove(key);
    }
    if (joinPublic)
        qputenv("YNOTBIT_RESPONDER_PUBLIC", "1");
    std::signal(SIGINT, [](int) { interrupted = 1; });
    std::signal(SIGTERM, [](int) { interrupted = 1; });
    Logger log;
    if (const auto path = tools::option(args, "--log"); !path.isEmpty())
        log.open(path);
    try {
        bm::Session session(root, false);
        session.choosePendingVault(vaultPath);
        session.submitVaultPassword(QString::fromLatin1(password));
        if (!session.mailboxOpen())
            return tools::fail(tool, "cannot open its mailbox: " + session.error() +
                                         (secret ? " (is it the phrase this was made with?)" : ""));
        session.setNodeWatchdog(watchdog);
        log.say("Chat responder");
        log.say("Address:  " + identity.address);
        log.say(joinPublic ? "Node:     joins the public network" +
                                 (peer.isEmpty() ? QString() : ", and links to " + peer)
                           : "Node:     links to " + peer + " only, the public network left alone");
        log.say("Data:     " + root);
        log.say("In ynotbit: Chats > New chat > paste the address. Say /help to it. Ctrl-C quits.");
        QElapsedTimer clock;
        clock.start();
        const bool silent = args.contains("--silent");
        tools::ChatBot bot(
            session,
            [&](const QString &, const QString &, const QString &text) {
                return silent ? QStringList() : answers(text);
            },
            options, [&](const QString &line) { log.say(line); });
        if (const auto hello = tools::option(args, "--hello"); !hello.isEmpty()) {
            if (session.sendChat(identity.address, hello, "hello from " + identity.address))
                log.say("Hello:    sent to " + tools::ChatBot::brief(hello));
            else
                log.say("Hello:    cannot be sent: " + session.error());
        }
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, [&] {
            if (interrupted) {
                app.quit();
                return;
            }
            bot.poll(clock.elapsed());
        });
        poll.start(300);
        // What it reports of the network: when it changes, and now and then that it is alive.
        int lastPeers = -1, lastWatchdog = 0;
        QString lastProblem;
        qint64 lastAlive = 0;
        const auto stateLine = [&] {
            const auto node = nodeStatus(root);
            const auto stats = bot.stats();
            return QString(
                       "peers %1 (out %2, in %3) · objects %4 · chats: %5 taken up, %6 answered, "
                       "%7 limited, %8 given up")
                .arg(node.value("peers").toInt())
                .arg(node.value("established_outgoing").toInt())
                .arg(node.value("established_incoming").toInt())
                .arg(node.value("objects").toInteger())
                .arg(stats.received)
                .arg(stats.answered)
                .arg(stats.limited)
                .arg(stats.failed);
        };
        QTimer report;
        QObject::connect(&report, &QTimer::timeout, [&] {
            const auto node = nodeStatus(root);
            const int peers = node.value("peers").toInt();
            if (peers != lastPeers) {
                log.say((lastPeers < 0 ? "Network:  "
                                       : "Network:  peers " + QString::number(lastPeers) + " -> " +
                                             QString::number(peers) + "; ") +
                        session.status());
                lastPeers = peers;
            }
            if (session.error() != lastProblem && !session.error().isEmpty())
                log.say("Problem: " + session.error());
            lastProblem = session.error();
            if (session.watchdogRestarts() != lastWatchdog) {
                lastWatchdog = session.watchdogRestarts();
                log.say("Watchdog: restarted its node, " + session.watchdogNote());
            }
            if (clock.elapsed() - lastAlive >= 10 * 60 * 1000) {
                lastAlive = clock.elapsed();
                log.say("Alive:    " + stateLine());
            }
        });
        report.start(5000);
        // For whoever watches it from outside: one small file, made again every ten seconds.
        const auto statusFile = tools::option(args, "--status-file");
        QTimer health;
        QObject::connect(&health, &QTimer::timeout, [&] {
            const auto node = nodeStatus(root);
            const auto stats = bot.stats();
            QJsonObject out{{"time", QDateTime::currentSecsSinceEpoch()},
                            {"uptime", clock.elapsed() / 1000},
                            {"address", identity.address},
                            {"peers", node.value("peers").toInt()},
                            {"outgoing", node.value("established_outgoing").toInt()},
                            {"incoming", node.value("established_incoming").toInt()},
                            {"objects", node.value("objects").toInteger()},
                            {"network", session.status()},
                            {"received", qint64(stats.received)},
                            {"answered", qint64(stats.answered)},
                            {"limited", qint64(stats.limited)},
                            {"failed", qint64(stats.failed)},
                            {"working", stats.working},
                            {"watchdogRestarts", session.watchdogRestarts()}};
            QSaveFile file(statusFile);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(QJsonDocument(out).toJson(QJsonDocument::Compact));
                file.commit();
            }
        });
        if (!statusFile.isEmpty())
            health.start(10000);
        const int code = app.exec();
        log.say("Bye.     " + stateLine());
        session.lock();
        return code;
    } catch (const std::exception &e) {
        return tools::fail(tool, e.what());
    }
}
