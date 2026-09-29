// Renders the README screenshots from a throwaway demo vault and mailbox:
//   readme_screenshots <output-dir> [language, e.g. ru or zh_CN]
// Not a test: it only builds realistic sample data and grabs the window.
#include "desktop_window.h"
#include "i18n.h"
#include "protocol.h"
#include "session.h"
#include <QTest>
#include <QtWidgets>
#include <iostream>

int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setOrganizationName("YnotbitScreenshots");
    app.setApplicationName("Readme");
    if (argc < 2) {
        std::cerr << "usage: readme_screenshots <output-dir> [language]\n";
        return 2;
    }
    const QString out = QString::fromLocal8Bit(argv[1]);
    bm::installTranslations(argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString("en"));
    QDir().mkpath(out);
    QTemporaryDir temp;

    // People ynotbit's user writes to. Their addresses are real, generated keys.
    const auto alice = bm::Protocol::identity("readme Alice").address;
    const auto bob = bm::Protocol::identity("readme Bob").address;
    const auto carol = bm::Protocol::identity("readme Carol").address;

    bm::Vault vault;
    vault.create(temp.filePath("vault"), "demo password");
    const auto me = vault.addIdentity("Personal");
    const auto general = vault.addChannel("general", {});
    vault.addChannel("privacy", {});
    auto key = vault.addMailboxKey();
    bm::Mailbox mailbox;
    mailbox.create(temp.filePath("mailbox"), key, vault.mailboxKey(key));
    qint64 n = 0;
    auto store = [&](const QString &from, const QString &to, const QString &subject,
                     const QString &body, const QString &folder) {
        mailbox.store("demo-" + QString::number(++n), from, to, subject, body, n, folder);
    };
    store(carol, me, "Photos from Saturday", "They came out great -- sending the best ones next week.",
          "Inbox");
    store(bob, me, "Re: node on the Raspberry Pi",
          "It has been up for nine days now and relays happily. Memory stays under 60 MB.",
          "Inbox");
    store(alice, me, "Notes for Thursday",
          "Hi,\n\nHere is the plan for **Thursday**:\n\n"
          "## Agenda\n\n"
          "1. Walk through the new address book\n"
          "2. Decide on the release date\n"
          "3. Anything else you bring\n\n"
          "> Keep it short -- we have the room for an hour.\n\n"
          "See you there,\nAlice",
          "Inbox");
    store(bob, general, "Welcome to the general chan",
          "Say hello, share what you are working on, and be kind.", "Channels");
    store(general, general, "Anyone running ynotbit on Windows?",
          "Curious how the new release behaves there.", "Channels");
    store(carol, general, "Re: Anyone running ynotbit on Windows?",
          "Yes -- the network engine is the same as on Linux now. Works well.", "Channels");
    mailbox.saveContact(alice, "Alice Liddell");
    mailbox.saveContact(bob, "Bob");
    mailbox.saveContact(carol, "Carol");
    const auto sent = mailbox.saveDraft({}, me, alice, "Re: Notes for Thursday",
                                        "Sounds good. I will bring the release checklist.");
    mailbox.queueDraft(sent, "direct", QDateTime::currentSecsSinceEpoch() + 86400);
    mailbox.setDelivery(sent, "acknowledged");
    mailbox.close();
    vault.lock();

    QDir().mkpath(temp.filePath("node"));
    {
        QSettings settings(temp.filePath("node/desktop.ini"), QSettings::IniFormat);
        settings.setValue("vault", temp.filePath("vault"));
        settings.setValue("mailbox", temp.filePath("mailbox"));
    }
    QTimer responder;
    QObject::connect(&responder, &QTimer::timeout, [&] {
        for (auto w : QApplication::topLevelWidgets())
            if (auto d = qobject_cast<QInputDialog *>(w)) {
                d->setTextValue("demo password");
                d->accept();
            }
    });
    responder.start(10);
    bm::Session session(temp.filePath("node"), true);
    session.unlockVault();
    if (!session.mailboxOpen()) {
        std::cerr << "could not open the demo mailbox\n";
        return 1;
    }
    bm::DesktopWindow window(session);
    window.show();
    auto folders = window.findChild<QListWidget *>("folders");
    auto list = window.findChild<QListView *>("letters");
    auto settle = [] { QTest::qWait(120); };
    auto shot = [&](const QString &name) {
        settle();
        window.grab().save(out + "/" + name);
        std::cout << (out + "/" + name).toStdString() << "\n";
    };

    // Inbox, reading Alice's letter (markdown, a named contact).
    folders->setCurrentRow(0);
    settle();
    for (int row = 0; row < list->model()->rowCount(); ++row) {
        const auto index = list->model()->index(row, 0);
        if (index.data(Qt::UserRole + 4).toString() == "Notes for Thursday") {
            list->setCurrentIndex(index);
            window.selectMessage(index.data(Qt::UserRole + 1).toString());
        }
    }
    shot("desktop.png");

    // A chan, reading a member's post.
    folders->setCurrentRow(4);
    settle();
    for (auto chip : window.findChildren<QPushButton *>("channelChip"))
        if (chip->text() == "[chan] general")
            chip->click();
    settle();
    if (list->model()->rowCount() > 0) {
        const auto index = list->model()->index(0, 0);
        list->setCurrentIndex(index);
        window.selectMessage(index.data(Qt::UserRole + 1).toString());
    }
    shot("channels.png");

    // The address book.
    folders->setCurrentRow(9);
    shot("contacts.png");

    // Writing to a contact.
    QTimer::singleShot(200, &window, [&] {
        if (auto dialog = window.findChild<QDialog *>("composer")) {
            dialog->findChild<QLineEdit *>("subjectField")->setText("Thursday");
            if (auto body = dialog->findChild<QTextEdit *>("bodyField"))
                body->setPlainText("Looking forward to it -- see you at ten.");
            settle();
            dialog->grab().save(out + "/composer.png");
            std::cout << (out + "/composer.png").toStdString() << "\n";
            dialog->reject();
        }
    });
    window.compose({{"to", alice}});
    return 0;
}
