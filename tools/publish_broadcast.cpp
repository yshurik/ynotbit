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
    try {
        bm::Vault vault;
        vault.create(vaultPath, password);
        vault.importKeys(b.keys);
        bool found = false;
        for (const auto &identity : vault.identities())
            found = found || identity.address == b.from;
        if (!found)
            return failed(b.keys + " does not hold the address " + b.from);
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
    const auto id = session.saveLetter({}, b.from, {}, b.subject, b.body, "broadcast");
    if (id.isEmpty())
        return failed(session.error());
    say("From:    " + b.from);
    say("Subject: " + b.subject);
    say(QString("Body:    %1 bytes").arg(b.body.toUtf8().size()));
    if (!b.send) {
        std::cout << '\n' << b.body.toStdString() << "\n\n";
        say("Dry run: nothing sent. Add --send to publish.");
        return 0;
    }
    if (!session.sendLetter(id))
        return failed(session.error());

    QElapsedTimer clock;
    clock.start();
    QString lastState;
    qint64 publishedAt = -1, lastReport = 0;
    while (true) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        QThread::msleep(100);
        const auto letter = session.message(id);
        const auto state = letter.value("state").toString();
        if (state != lastState) {
            lastState = state;
            say("State: " + QString(state).replace('_', ' ') + " · " + session.status());
        }
        if (state == "failed" || state == "expired")
            return failed("not published: " + letter.value("deliveryError").toString());
        if (state == "published" && publishedAt < 0) {
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
