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
    const auto news = bm::Protocol::identity("readme Mesh news").address;
    // A picture travels inside the letter, as a data: URL the reader shows.
    QString beach;
    {
        QImage photo(720, 440, QImage::Format_RGB32);
        QPainter p(&photo);
        p.setRenderHint(QPainter::Antialiasing);
        QLinearGradient sky(0, 0, 0, 260);
        sky.setColorAt(0, QColor("#3f7cc4"));
        sky.setColorAt(1, QColor("#f3c98b"));
        p.fillRect(0, 0, 720, 260, sky);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#ffe9a8"));
        p.drawEllipse(QPointF(520, 210), 46, 46);
        QLinearGradient sea(0, 250, 0, 330);
        sea.setColorAt(0, QColor("#2f6f9a"));
        sea.setColorAt(1, QColor("#3d9bb3"));
        p.fillRect(0, 250, 720, 80, sea);
        QLinearGradient sand(0, 330, 0, 440);
        sand.setColorAt(0, QColor("#e8d3a2"));
        sand.setColorAt(1, QColor("#d1b47c"));
        p.fillRect(0, 330, 720, 110, sand);
        p.end();
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        photo.save(&buffer, "JPG", 82);
        beach = "data:image/jpeg;base64," + QString::fromLatin1(jpeg.toBase64());
    }

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
    store(carol, me, "Photos from Saturday",
          "They came out great -- here is the best one:\n\n![Saturday at the beach][img1]\n\n"
          "More next week.\n\n[img1]: " + beach,
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
    // A sender followed on the Subscriptions page; a broadcast's recipient is its sender.
    mailbox.subscribe(news, "Mesh networking notes");
    store(news, news, "A month on a solar-powered relay",
          "The Raspberry Pi node has now run for **31 days** on a 20 W panel.\n\n"
          "- Uptime: 99.2%, two short stops on cloudy mornings\n"
          "- Memory: under 60 MB the whole time\n"
          "- Objects relayed: about 14,000 a day\n\n"
          "Next: a second node at the allotment.",
          "Broadcasts");
    store(news, news, "Bitmessage over Tor, revisited",
          "Routing the node through a local Tor proxy still works well.\n\n"
          "## Two tips\n\n"
          "- Expect slower first contact with peers, then normal traffic\n"
          "- Keep incoming connections off when running behind Tor",
          "Broadcasts");
    store(news, news, "Reading list",
          "## This week\n\n"
          "1. How proof of work keeps the network quiet\n"
          "2. Chans: shared addresses, shared keys\n"
          "3. Why every message reaches every node",
          "Broadcasts");
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

    // Subscriptions: the senders followed, one sender's posts as a feed.
    folders->setCurrentRow(5);
    settle();
    for (auto chip : window.findChildren<QPushButton *>("channelChip"))
        if (chip->text() == "Mesh networking notes")
            chip->click();
    shot("subscriptions.png");

    // A picture carried inside a letter.
    folders->setCurrentRow(0);
    settle();
    for (int row = 0; row < list->model()->rowCount(); ++row) {
        const auto index = list->model()->index(row, 0);
        if (index.data(Qt::UserRole + 4).toString() == "Photos from Saturday") {
            list->setCurrentIndex(index);
            window.selectMessage(index.data(Qt::UserRole + 1).toString());
        }
    }
    shot("pictures.png");

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

    // Replying: the letter is quoted in the same editor, one level deeper.
    QString notes;
    for (const auto &m : session.messagePage("Inbox", {}, 0, 100))
        if (m.toMap()["subject"].toString() == "Notes for Thursday")
            notes = m.toMap()["hash"].toString();
    QTimer::singleShot(200, &window, [&] {
        if (auto dialog = window.findChild<QDialog *>("composer")) {
            if (auto body = dialog->findChild<QTextEdit *>("bodyField")) {
                body->moveCursor(QTextCursor::Start);
                body->insertPlainText("Sounds good. I will bring the release checklist.");
            }
            settle();
            dialog->grab().save(out + "/reply.png");
            std::cout << (out + "/reply.png").toStdString() << "\n";
            dialog->reject();
        }
    });
    window.compose(session.message(notes), true);
    return 0;
}
