#include "publish_broadcast.h"
#include "session.h"
#include "storage.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
extern "C" {
int ntb_daemon(int argc, char **argv);
}
namespace tools {
namespace {
void say(const QString &message) {
    std::cout << QDateTime::currentDateTime().toString("hh:mm:ss ").toStdString()
              << message.toStdString() << std::endl;
}
} // namespace
QString option(const QStringList &args, const QString &name) {
    const int i = args.indexOf(name);
    return i >= 0 && i + 1 < args.size() ? args[i + 1] : QString();
}
QStringList options(const QStringList &args, const QString &name) {
    QStringList values;
    for (int i = 0; i + 1 < args.size(); ++i)
        if (args[i] == name)
            values << args[i + 1];
    return values;
}
bool sendOptions(const QStringList &args, Broadcast &broadcast) {
    broadcast.send = args.contains("--send");
    if (const auto value = option(args, "--linger"); !value.isEmpty()) {
        bool ok = false;
        broadcast.linger = value.toInt(&ok);
        if (!ok || broadcast.linger < 0) {
            fail(broadcast.tool, "--linger takes a number of minutes");
            return false;
        }
    }
    return true;
}
int fail(const QString &tool, const QString &message) {
    std::cerr << tool.toStdString() << ": " << message.toStdString() << '\n';
    return 1;
}
int runNode(int argc, char **argv) {
    return ntb_daemon(argc - 1, argv + 1);
}
int publish(const Broadcast &b) {
    const auto failed = [&](const QString &message) { return fail(b.tool, message); };
    QTemporaryDir temp;
    if (!temp.isValid())
        return failed("cannot create a temporary directory");
    const auto root = temp.filePath("node"), vaultPath = temp.filePath("sender.bmvault"),
               mailPath = temp.filePath("sender.bmmail");
    QDir().mkpath(root);
    // Never written anywhere: the vault is discarded with the directory.
    QByteArray password(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(password.data()), 8);
    password = password.toBase64();
    // Joined in the throwaway vault, so a letter to a chan is encrypted with
    // the chan's own keys, without asking the network for them.
    QStringList chanAddresses;
    try {
        bm::Vault vault;
        vault.create(vaultPath, password);
        vault.importKeys(b.keys);
        bool found = false;
        for (const auto &identity : vault.identities())
            found = found || identity.address == b.from;
        if (!found)
            return failed(b.keys + " does not hold the address " + b.from);
        for (const auto &phrase : b.chans)
            chanAddresses << vault.addChannel(phrase, {});
        const auto mailKey = vault.addMailboxKey();
        bm::Mailbox mailbox;
        mailbox.create(mailPath, mailKey, vault.mailboxKey(mailKey));
    } catch (const std::exception &e) {
        return failed(e.what());
    }
    {
        QSettings config(root + "/desktop.ini", QSettings::IniFormat);
        config.setValue("vault", vaultPath);
        config.setValue("mailbox", mailPath);
    }

    // A dry run stays offline: no node starts, nothing leaves this machine.
    bm::Session session(root, !b.send);
    session.choosePendingVault(vaultPath);
    session.submitVaultPassword(QString::fromLatin1(password), {}, false);
    if (!session.mailboxOpen())
        return failed("cannot open the sender's mailbox: " + session.error());
    // One broadcast, or one letter per chan.
    QStringList ids, recipients;
    if (b.chans.isEmpty()) {
        ids << session.saveLetter({}, b.from, {}, b.subject, b.body, "broadcast");
        recipients << "subscribers (broadcast)";
    } else {
        for (int i = 0; i < b.chans.size(); ++i) {
            ids << session.saveLetter({}, b.from, chanAddresses[i], b.subject, b.body, "direct");
            recipients << "[chan] " + b.chans[i] + "  " + chanAddresses[i];
        }
    }
    if (ids.contains(QString()))
        return failed(session.error());
    say("From:    " + b.from);
    for (const auto &recipient : recipients)
        say("To:      " + recipient);
    say("Subject: " + b.subject);
    say(QString("Body:    %1 bytes").arg(b.body.toUtf8().size()));
    if (!b.send) {
        std::cout << '\n' << b.body.toStdString() << "\n\n";
        say("Dry run: nothing sent. Add --send to publish.");
        return 0;
    }
    for (const auto &id : ids)
        if (!session.sendLetter(id))
            return failed(session.error());

    // A letter to a chan then waits for an acknowledgment that may never come;
    // being offered to peers is what counts here.
    const auto offered = [](const QString &state) {
        return state == "published" || state == "awaiting_ack" || state == "acknowledged";
    };
    QElapsedTimer clock;
    clock.start();
    QStringList lastStates(ids.size());
    qint64 publishedAt = -1, lastReport = 0;
    while (true) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        QThread::msleep(100);
        bool allOffered = true;
        for (int i = 0; i < ids.size(); ++i) {
            const auto letter = session.message(ids[i]);
            const auto state = letter.value("state").toString();
            if (state != lastStates[i]) {
                lastStates[i] = state;
                say((ids.size() > 1 ? recipients[i].section("  ", 0, 0) + ": " : QString()) +
                    "State: " + QString(state).replace('_', ' ') + " · " + session.status());
            }
            if (state == "failed" || state == "expired")
                return failed("not published: " + letter.value("deliveryError").toString());
            allOffered = allOffered && offered(state);
        }
        if (allOffered && publishedAt < 0) {
            publishedAt = clock.elapsed();
            say(QString("Offered to peers; keeping the node up %1 more minutes so they "
                        "can fetch it").arg(b.linger));
        }
        if (clock.elapsed() - lastReport >= 30000) {
            lastReport = clock.elapsed();
            say(session.status());
        }
        if (publishedAt >= 0 && clock.elapsed() - publishedAt >= qint64(b.linger) * 60000)
            break;
        if (publishedAt < 0 && clock.elapsed() > 60 * 60000)
            return failed("no peer took the object within an hour; nothing was published");
    }
    say("Done: " + b.subject + " is on the network.");
    session.lock();
    return 0;
}
} // namespace tools
