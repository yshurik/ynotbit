// Publishes a release announcement: a broadcast from ynotbit's release address
// whose subject is "ynotbit X.Y.Z", which every ynotbit mailbox turns into an
// update notice. Runs headless on a throwaway vault in a temporary directory,
// so the release key never enters a personal vault.
//
//   build/announce_release --keys release_key.dat --version 0.5.1 --notes notes.md
//
// Without --send it stays offline and only prints the letter it would publish.
#include "session.h"
#include "storage.h"
#include "updates.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
extern "C" {
int ntb_daemon(int argc, char **argv);
}
namespace {
QString option(const QStringList &args, const QString &name) {
    const int i = args.indexOf(name);
    return i >= 0 && i + 1 < args.size() ? args[i + 1] : QString();
}
int fail(const QString &message) {
    std::cerr << "announce_release: " << message.toStdString() << '\n';
    return 1;
}
void say(const QString &message) {
    std::cout << QDateTime::currentDateTime().toString("hh:mm:ss ").toStdString()
              << message.toStdString() << std::endl;
}
} // namespace
int main(int argc, char **argv) {
    // Session runs its network node as this same executable with --node.
    if (argc > 1 && std::string(argv[1]) == "--node")
        return ntb_daemon(argc - 1, argv + 1);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setOrganizationName("YnotbitTools");
    app.setApplicationName("announce_release");
    const auto args = app.arguments();
    const auto keys = option(args, "--keys"), version = option(args, "--version"),
               notes = option(args, "--notes");
    if (keys.isEmpty() || version.isEmpty() || notes.isEmpty())
        return fail("usage: announce_release --keys <keys.dat> --version <X.Y.Z> "
                    "--notes <file> [--send] [--linger <minutes>]");
    const auto subject = "ynotbit " + version;
    if (bm::updates::announcedVersion(subject) != version)
        return fail("\"" + subject + "\" is not a subject ynotbit reads as a release");
    QFile notesFile(notes);
    if (!notesFile.open(QIODevice::ReadOnly))
        return fail("cannot read " + notes);
    const auto body = QString::fromUtf8(notesFile.readAll()).trimmed();
    if (body.isEmpty())
        return fail(notes + " is empty");
    const bool send = args.contains("--send");
    int linger = 10;
    if (const auto value = option(args, "--linger"); !value.isEmpty()) {
        bool ok = false;
        linger = value.toInt(&ok);
        if (!ok || linger < 0)
            return fail("--linger takes a number of minutes");
    }

    QTemporaryDir temp;
    if (!temp.isValid())
        return fail("cannot create a temporary directory");
    const auto root = temp.filePath("node"), vaultPath = temp.filePath("release.bmvault"),
               mailPath = temp.filePath("release.bmmail");
    QDir().mkpath(root);
    // Never written anywhere: the vault is discarded with the directory.
    QByteArray password(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(password.data()), 8);
    password = password.toBase64();
    const auto publisher = bm::updates::publisherAddress();
    try {
        bm::Vault vault;
        vault.create(vaultPath, password);
        vault.importKeys(keys);
        bool found = false;
        for (const auto &identity : vault.identities())
            found = found || identity.address == publisher;
        if (!found)
            return fail(keys + " does not hold the release address " + publisher);
        const auto mailKey = vault.addMailboxKey();
        bm::Mailbox mailbox;
        mailbox.create(mailPath, mailKey, vault.mailboxKey(mailKey));
    } catch (const std::exception &e) {
        return fail(e.what());
    }
    {
        QSettings config(root + "/desktop.ini", QSettings::IniFormat);
        config.setValue("vault", vaultPath);
        config.setValue("mailbox", mailPath);
    }

    // A dry run stays offline: no node starts, nothing leaves this machine.
    bm::Session session(root, !send);
    session.choosePendingVault(vaultPath);
    session.submitVaultPassword(QString::fromLatin1(password), {}, false);
    if (!session.mailboxOpen())
        return fail("cannot open the release mailbox: " + session.error());
    const auto id = session.saveLetter({}, publisher, {}, subject, body, "broadcast");
    if (id.isEmpty())
        return fail(session.error());
    say("From:    " + publisher);
    say("Subject: " + subject);
    say(QString("Body:    %1 bytes").arg(body.toUtf8().size()));
    if (!send) {
        std::cout << '\n' << body.toStdString() << "\n\n";
        say("Dry run: nothing sent. Add --send to publish.");
        return 0;
    }
    if (!session.sendLetter(id))
        return fail(session.error());

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
            return fail("not published: " + letter.value("deliveryError").toString());
        if (state == "published" && publishedAt < 0) {
            publishedAt = clock.elapsed();
            say(QString("Offered to peers; keeping the node up %1 more minutes so they "
                        "can fetch it").arg(linger));
        }
        if (clock.elapsed() - lastReport >= 30000) {
            lastReport = clock.elapsed();
            say(session.status());
        }
        if (publishedAt >= 0 && clock.elapsed() - publishedAt >= qint64(linger) * 60000)
            break;
        if (publishedAt < 0 && clock.elapsed() > 60 * 60000)
            return fail("no peer took the object within an hour; nothing was published");
    }
    say("Done: " + subject + " is on the network.");
    session.lock();
    return 0;
}
