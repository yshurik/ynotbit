#include "desktop_window.h"
#include "session.h"
#include <QElapsedTimer>
#include <QTest>
#include <QtWidgets>
#include <iostream>
#ifdef Q_OS_MACOS
#include <mach/mach.h>
#endif
static void footprint(const char *stage) {
#ifdef Q_OS_MACOS
    task_vm_info_data_t info{};
    mach_msg_type_number_t size = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &size) ==
        KERN_SUCCESS)
        std::cout << stage << " physical footprint: " << info.phys_footprint / 1048576.0
                  << " MiB\n";
#else
    Q_UNUSED(stage);
#endif
}
int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setOrganizationName("YnotbitTests");
    app.setApplicationName("Widgets");
    QTemporaryDir temp;
    auto require = [](bool ok, const char *message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    try {
        bm::Vault vault;
        vault.create(temp.filePath("vault"), "test password");
        const auto personal = vault.addIdentity("Personal");
        const auto emptyChannel =
            vault.addChannel("widgets empty channel regression", "Empty channel");
        auto key = vault.addMailboxKey();
        bm::Mailbox mailbox;
        mailbox.create(temp.filePath("mailbox"), key, vault.mailboxKey(key));
        for (int i = 0; i < 1500; ++i)
            mailbox.store(QString::number(i), "sender", "recipient",
                          "Subject " + QString::number(i), QString(10000, 'x'), i, "Channels");
        mailbox.store("other-channel", "sender", "second-recipient", "Only channel two",
                      "Separate discussion", 1600, "Channels");
        const auto acknowledged = mailbox.saveDraft({}, personal, emptyChannel,
                                                    "Delivery confirmed", "A delivered letter.");
        mailbox.queueDraft(acknowledged, "direct", QDateTime::currentSecsSinceEpoch() + 86400);
        mailbox.setDelivery(acknowledged, "acknowledged");
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
                    d->setTextValue("test password");
                    d->accept();
                }
        });
        responder.start(10);
        bm::Session session(temp.filePath("node"), true);
        QElapsedTimer clock;
        clock.start();
        session.unlockVault();
        require(session.mailboxOpen(), "mailbox unlock");
        std::cout << "Unlock plus mailbox open: " << clock.elapsed() << "ms\n";
        bm::DesktopWindow window(session);
        window.show();
        auto folders = window.findChild<QListWidget *>("folders");
        require(folders, "folders exist");
        folders->setCurrentRow(4);
        QTest::qWait(50);
        footprint("Mailbox open");
        auto list = window.findChild<QListView *>("letters");
        require(list, "list exists");
        auto channels = window.findChild<QComboBox *>("channelSelector");
        const auto mono = [](const QFont &font) {
            QFontMetricsF metrics(font);
            return qAbs(metrics.horizontalAdvance("iiii") - metrics.horizontalAdvance("WWWW")) <
                   0.1;
        };
        require(mono(channels->font()), "channel addresses use equal-width characters");
        require(mono(window.findChild<QLabel *>("messageAddresses")->font()),
                "message address headers use fixed width");
        require(channels && channels->count() == 3, "stored and empty joined channels are listed");
        channels->setCurrentIndex(channels->findData("recipient"));
        require(list->model()->rowCount() == 1500, "all channel rows available");
        clock.restart();
        for (int i = 0; i < 1500; i += 25) {
            list->scrollTo(list->model()->index(i, 0));
            QCoreApplication::processEvents();
            require(session.messageModel()->cachedPages() <= 3, "bounded page cache");
        }
        std::cout << "60 scroll positions across 1500 messages: " << clock.elapsed() << "ms\n";
        session.messageModel()->setSearch("Subject 1499");
        require(session.messageModel()->rowCount() == 1, "search finds old rows");
        session.messageModel()->setSearch("");
        window.selectMessage("0");
        channels->setCurrentIndex(channels->findData("second-recipient"));
        require(list->model()->rowCount() == 1, "second channel excludes first channel messages");
        require(list->model()->index(0, 0).data(Qt::UserRole + 1) == "other-channel",
                "channel page has correct recipient");
        require(window.findChild<QTextBrowser *>("readerBody")->toPlainText().isEmpty(),
                "switching channel clears previous body");
        session.messageModel()->setSearch("Subject");
        require(list->model()->rowCount() == 0, "search stays within selected channel");
        session.messageModel()->setSearch("");
        folders->setCurrentRow(0);
        folders->setCurrentRow(4);
        require(channels->currentData() == "second-recipient",
                "channel selection survives folder navigation");
        channels->setCurrentIndex(channels->findData(emptyChannel));
        require(list->model()->rowCount() == 0,
                "empty joined channel does not show mixed messages");
        QTimer::singleShot(30, &window, [&] {
            auto dialog = window.findChild<QDialog *>("composer");
            require(dialog->findChild<QLineEdit *>("recipientField")->text() == emptyChannel,
                    "channel compose targets selected channel");
            require(mono(dialog->findChild<QLineEdit *>("recipientField")->font()),
                    "recipient field uses fixed width");
            require(dialog->findChild<QComboBox *>("senderSelector")->currentData() == emptyChannel,
                    "channel compose uses channel identity");
            auto chanModePrivate = dialog->findChild<QPushButton *>("modePrivateButton");
            auto chanModePublic = dialog->findChild<QPushButton *>("modePublicButton");
            require(chanModePrivate && chanModePublic, "mode buttons exist for channel compose");
            require(chanModePrivate->text() == "Personal" && chanModePublic->text() == "Anonymous",
                    "channel sender shows Personal/Anonymous labels");
            dialog->reject();
        });
        window.findChild<QPushButton *>("writeButton")->click();
        channels->setCurrentIndex(channels->findData("recipient"));
        clock.restart();
        for (int i = 0; i < 50; ++i) {
            window.selectMessage(QString::number(i));
            QCoreApplication::processEvents();
        }
        std::cout << "50 full message selections: " << clock.elapsed() << "ms\n";
        require(window.findChild<QTextBrowser *>("readerBody")->toPlainText().size() == 10000,
                "body rendered");
        auto restoreAction = window.findChild<QAction *>("restoreAction");
        auto deletePermanentlyAction = window.findChild<QAction *>("deletePermanentlyAction");
        auto retryAction = window.findChild<QAction *>("retryAction");
        auto cancelDeliveryAction = window.findChild<QAction *>("cancelDeliveryAction");
        require(restoreAction && deletePermanentlyAction && retryAction && cancelDeliveryAction,
                "trash-only and delivery-only actions exist");
        require(!restoreAction->isVisible() && !deletePermanentlyAction->isVisible() &&
                    !retryAction->isVisible() && !cancelDeliveryAction->isVisible(),
                "a received channel message hides trash-only and delivery-only actions");
        footprint("After scrolling and selections");
        if (app.arguments().contains("--soak")) {
            for (int pass = 0; pass < 5; ++pass) {
                for (int i = 0; i < 100; ++i) {
                    window.selectMessage(QString::number(i));
                    QCoreApplication::processEvents();
                }
                footprint("After another 100 selections");
            }
        }
        auto address = session.identities().first().toMap()["address"].toString();
        auto formatted = session.saveLetter(
            {}, address, address, "A letter from the Widgets desktop",
            "Hello,\n\nThis is **bold**, this is *italic*, and this is a "
            "[link](https://example.com).\n\n- A readable list\n- With normal body text\n\n> A "
            "quoted passage\n\nNo remote images are loaded.\n\n![blocked](file:///etc/passwd)",
            "direct");
        window.selectMessage(formatted);
        auto reader = window.findChild<QTextBrowser *>("readerBody");
        require(reader->toPlainText().contains("A readable list"), "Markdown renders lists");
        require(reader->document()
                    ->resource(QTextDocument::ImageResource, QUrl("file:///etc/passwd"))
                    .value<QImage>()
                    .isNull(),
                "reader blocks local resources");
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "Open in new window")
                action->trigger();
        auto popped = window.findChild<QDialog *>("messageWindow");
        require(popped, "opens a separate message window");
        require(popped->findChild<QTextBrowser *>("windowBody")->toPlainText().contains(
                    "A readable list"),
                "separate window renders the same markdown body");
        require(reader->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOn,
                "main window body always shows its scrollbar");
        require(window.findChild<QScrollArea *>("subjectScroll")->verticalScrollBarPolicy() ==
                    Qt::ScrollBarAlwaysOn,
                "main window subject always shows its scrollbar");
        require(popped->findChild<QTextBrowser *>("windowBody")->verticalScrollBarPolicy() ==
                    Qt::ScrollBarAlwaysOn,
                "separate window body always shows its scrollbar");
        require(popped->findChild<QScrollArea *>("windowSubjectScroll")->verticalScrollBarPolicy() ==
                    Qt::ScrollBarAlwaysOn,
                "separate window subject always shows its scrollbar");
        require(window.findChild<QToolBar *>("actionsToolbar")->mapTo(&window, QPoint()).y() <
                    window.findChild<QScrollArea *>("subjectScroll")->mapTo(&window, QPoint()).y(),
                "main window toolbar renders above the subject");
        require(popped->findChild<QToolBar *>("windowActionsToolbar")->mapTo(popped, QPoint()).y() <
                    popped->findChild<QScrollArea *>("windowSubjectScroll")
                        ->mapTo(popped, QPoint())
                        .y(),
                "separate window toolbar renders above the subject");
        popped->close();
        if (app.arguments().contains("--capture")) {
            window.selectMessage(acknowledged);
            QCoreApplication::processEvents();
            auto n = app.arguments().indexOf("--capture");
            if (n + 1 < app.arguments().size())
                window.grab().save(app.arguments()[n + 1]);
        }
        window.selectMessage(acknowledged);
        QCoreApplication::processEvents();
        auto fromLabel = window.findChild<QLabel *>("messageAddresses");
        auto toLabel = window.findChild<QLabel *>("toAddress");
        require(fromLabel->mapTo(&window, QPoint()).x() == toLabel->mapTo(&window, QPoint()).x(),
                "From and To address columns align");
        auto badge = window.findChild<QLabel *>("deliveryStatus");
        require(badge->isVisible() && badge->text() == "Acknowledged",
                "acknowledgment has a separate status badge");
        auto timeline = window.findChild<QLabel *>("messageTimeline");
        require(timeline->text().contains("Queued") && timeline->text().contains("Acknowledged"),
                "delivery timeline shows queued and acknowledged stages");
        for (const auto &mode : {QString("light"), QString("dark")}) {
            for (auto action : window.findChildren<QAction *>())
                if (action->text() == mode)
                    action->trigger();
            QCoreApplication::processEvents();
            auto color = badge->palette().color(QPalette::WindowText);
            require(color.green() > color.red() && color.green() > color.blue(),
                    "acknowledged status is green in both themes");
        }
        auto header = window.findChild<QWidget *>("header");
        auto statusBar = window.findChild<QWidget *>("statusBar");
        require(header && statusBar, "header and status bar exist");
        require(statusBar->palette().color(QPalette::Window) ==
                    header->palette().color(QPalette::Window),
                "status bar background matches header background");
        require(statusBar->font().pixelSize() > 0 && statusBar->font().pixelSize() < 13,
                "status bar uses a reduced font size");
        window.selectMessage(formatted);
        require(!badge->isVisible(), "draft has no stale acknowledgment badge");
        auto toolbar = window.findChild<QToolBar *>("actionsToolbar");
        require(toolbar, "message actions render as a toolbar");
        auto editAction = window.findChild<QAction *>("editAction");
        auto replyAction = window.findChild<QAction *>("replyAction");
        require(editAction && replyAction, "edit and reply actions exist");
        require(editAction->isVisible() && !replyAction->isVisible(),
                "a draft shows Edit, not Reply, as a toolbar action");
        require(!editAction->icon().isNull() && !replyAction->icon().isNull(),
                "toolbar actions carry an icon");
        const auto longSubject = session.saveLetter({}, address, address, QString(500, 'L'),
                                                    "Long subject body", "direct");
        window.selectMessage(longSubject);
        auto subjectLabel = window.findChild<QLabel *>("subjectLabel");
        require(subjectLabel && subjectLabel->text() == QString(500, 'L'),
                "long subjects show the full text, not truncated");
        require(window.findChild<QScrollArea *>("subjectScroll")->height() <= 120,
                "long subject stays within a bounded, scrollable area in the reader header");
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "Open in new window")
                action->trigger();
        auto longPopped = window.findChild<QDialog *>("messageWindow");
        require(longPopped, "opens a separate window for the long-subject letter too");
        auto subjectScroll = longPopped->findChild<QScrollArea *>("windowSubjectScroll");
        require(subjectScroll && subjectScroll->height() <= 120,
                "long subject stays within a bounded, scrollable area in the separate window");
        longPopped->close();
        QCoreApplication::processEvents();
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "Copy subject")
                action->trigger();
        require(QApplication::clipboard()->text() == QString(500, 'L'),
                "Copy subject action copies the full untruncated subject");
        const auto longBody =
            session.saveLetter({}, address, address, "Long body letter", QString(3000, 'z'), "direct");
        window.selectMessage(longBody);
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "Open in new window")
                action->trigger();
        QCoreApplication::processEvents();
        auto bodyPopped = window.findChild<QDialog *>("messageWindow");
        require(bodyPopped, "opens a separate window for the long-body letter");
        auto poppedBody = bodyPopped->findChild<QTextBrowser *>("windowBody");
        require(poppedBody->verticalScrollBar()->maximum() > 0,
                "long body is actually scrollable (test sanity)");
        require(poppedBody->verticalScrollBar()->value() == 0,
                "separate window opens scrolled to the beginning of the body, not the end");
        bodyPopped->close();
        QCoreApplication::processEvents();
        folders->setCurrentRow(4);
        channels->setCurrentIndex(channels->findData("recipient"));
        QCoreApplication::processEvents();
        require(list->model()->rowCount() > 0, "channel has at least one letter to double-click");
        auto dIndex = list->model()->index(0, 0);
        auto expectedSubject = dIndex.data(Qt::UserRole + 4).toString();
        emit list->doubleClicked(dIndex);
        QCoreApplication::processEvents();
        auto dPopped = window.findChild<QDialog *>("messageWindow");
        require(dPopped, "double-clicking a letter opens it in a separate window");
        require(dPopped->findChild<QLabel *>("windowSubjectLabel")->text() == expectedSubject,
                "double-click opens the correct letter");
        dPopped->close();
        QCoreApplication::processEvents();
        QTimer::singleShot(50, &window, [&] {
            auto dialog = window.findChild<QDialog *>("composer");
            require(dialog, "composer opens");
            auto body = dialog->findChild<QTextEdit *>("bodyField");
            body->setFocus();
            QTest::keyClicks(body, "#");
            QTest::keyClick(body, Qt::Key_Space);
            require(body->textCursor().blockFormat().headingLevel() == 1,
                    "typing '# ' auto-formats the block as a heading");
            require(!body->toPlainText().contains('#'),
                    "the '#' marker is consumed, not left as literal text");
            QTest::keyClicks(body, "Reply");
            QTest::keyClick(body, Qt::Key_Return);
            require(body->textCursor().blockFormat().headingLevel() == 0,
                    "pressing Enter after a heading reverts to a normal paragraph");
            require(body->textCursor().charFormat().fontWeight() != QFont::Bold,
                    "the paragraph after a heading is not bold");
            body->clear();
            body->setPlainText("Select this word please");
            auto selectCursor = body->textCursor();
            selectCursor.setPosition(0);
            selectCursor.setPosition(6, QTextCursor::KeepAnchor);
            body->setTextCursor(selectCursor);
            auto floatingToolbar = dialog->findChild<QWidget *>("floatingToolbar");
            require(floatingToolbar && floatingToolbar->isVisible(),
                    "selecting text shows the floating formatting toolbar");
            auto clearCursor = body->textCursor();
            clearCursor.clearSelection();
            body->setTextCursor(clearCursor);
            require(!floatingToolbar->isVisible(),
                    "clearing the selection hides the floating toolbar");
            auto gutter = dialog->findChild<QWidget *>("headingGutter");
            require(gutter && gutter->isVisible() && gutter->width() > 0,
                    "heading gutter renders beside the body");
            if (app.arguments().contains("--capture-composer")) {
                body->clear();
                auto c = body->textCursor();
                auto heading = [&](int level, QString text) {
                    auto f = c.blockFormat();
                    f.setHeadingLevel(level);
                    c.insertBlock(f);
                    QTextCharFormat t;
                    t.setFontWeight(QFont::Bold);
                    t.setFontPointSize(level == 1 ? 22 : 18);
                    c.insertText(text, t);
                };
                auto paragraph = [&](QString text) {
                    QTextBlockFormat f;
                    c.insertBlock(f);
                    c.insertText(text, QTextCharFormat());
                };
                heading(1, "Reply");
                paragraph("Some body text explaining the update.");
                heading(2, "Next steps");
                paragraph("More text goes here.");
                QCoreApplication::processEvents();
                auto n = app.arguments().indexOf("--capture-composer");
                if (n + 1 < app.arguments().size())
                    dialog->grab().save(app.arguments()[n + 1]);
            }
            body->clear();
            body->setPlainText("A visual letter");
            auto to = dialog->findChild<QLineEdit *>("recipientField");
            require(to->isVisible(), "recipient visible in private mode by default");
            auto modePrivate = dialog->findChild<QPushButton *>("modePrivateButton");
            auto modePublic = dialog->findChild<QPushButton *>("modePublicButton");
            require(modePrivate->text() == "Private mail" && modePublic->text() == "Public mail",
                    "non-channel sender shows mail privacy labels");
            require(modePrivate->isChecked() && !modePublic->isChecked(),
                    "private mail selected by default");
            modePublic->click();
            require(!to->isVisible(), "recipient hidden in public mode");
            modePrivate->click();
            require(to->isVisible(), "recipient visible again after switching back");
            for (const auto &name :
                {"boldAction", "italicAction", "strikeAction", "codeAction", "linkAction",
                 "imageAction", "clearFormatAction"})
                require(dialog->findChild<QAction *>(name), QString("toolbar has %1").arg(name)
                                                                 .toUtf8()
                                                                 .constData());
            to->setText(session.identities().first().toMap()["address"].toString());
            dialog->findChild<QPushButton *>("sendButton")->click();
        });
        window.compose();
        require(session.messageCount("Outbox", "") == 1,
                "composer sends through persistent outbox");
        const auto draftsBeforeNeverSaved = session.messageCount("Drafts", "");
        QTimer::singleShot(30, &window, [&] {
            window.findChild<QDialog *>("composer")
                ->findChild<QPushButton *>("discardButton")
                ->click();
        });
        window.compose();
        require(session.messageCount("Drafts", "") == draftsBeforeNeverSaved,
                "discarding a never-saved draft creates nothing");
        const auto toDiscard =
            session.saveLetter({}, address, address, "Draft to discard", "Throwaway content", "direct");
        const auto draftsBeforeExisting = session.messageCount("Drafts", "");
        const auto trashBeforeExisting = session.messageCount("Trash", "");
        QTimer::singleShot(30, &window, [&] {
            window.findChild<QDialog *>("composer")
                ->findChild<QPushButton *>("discardButton")
                ->click();
        });
        window.compose({{"hash", toDiscard}});
        require(session.messageCount("Drafts", "") == draftsBeforeExisting - 1,
                "discarding an existing draft removes it from Drafts");
        require(session.messageCount("Trash", "") == trashBeforeExisting + 1,
                "discarding an existing draft moves it to Trash, not a permanent delete");
        for (const auto &mode : {QString("light"), QString("dark"), QString("system")}) {
            bm::Appearance appearance;
            appearance.setMode(mode);
            require(appearance.mode() == mode, "theme mode switches");
        }
        session.lock();
        require(list->model()->rowCount() == 0, "lock clears model");
        require(window.findChild<QTextBrowser *>("readerBody")->toPlainText().isEmpty(),
                "lock clears plaintext");
        QTimer passwordResponder;
        int attempts = 0;
        QObject::connect(&passwordResponder, &QTimer::timeout, [&] {
            auto field = window.findChild<QLineEdit *>("vaultPasswordField");
            auto unlockButton = window.findChild<QPushButton *>("vaultUnlockButton");
            if (!field || !unlockButton || !unlockButton->isVisible() || attempts >= 2)
                return;
            field->setText(attempts++ == 0 ? "wrong password" : "test password");
            unlockButton->click();
            if (attempts == 1)
                require(!session.unlocked(), "inline unlock rejects wrong password");
        });
        passwordResponder.start(30);
        session.beginVaultUnlock();
        // Each attempt runs a real Argon2id unlock (memory-hard, ~100ms+ here), so
        // poll for completion instead of assuming both attempts fit a fixed wait.
        const auto unlockDeadline = QDateTime::currentMSecsSinceEpoch() + 5000;
        while (!(attempts == 2 && session.mailboxOpen()) &&
               QDateTime::currentMSecsSinceEpoch() < unlockDeadline)
            QTest::qWait(20);
        require(attempts == 2 && session.mailboxOpen(),
                "inline unlock retries and restores mailbox");
        passwordResponder.stop();
        require(session.recentVaults().size() >= 1 &&
                    session.recentVaults().first().toMap()["path"] == session.vaultPath(),
                "unlocked vault is remembered as the most recent");
        QTimer::singleShot(30, &window, [&] {
            auto dialog = window.findChild<QDialog *>("composer");
            dialog->findChild<QTextEdit *>("bodyField")->setPlainText("Saved immediately on lock");
            session.lock();
        });
        window.compose({{"hash", formatted}});
        require(!session.mailboxOpen(), "locking closes composer and mailbox");
        session.unlockVault();
        require(session.message(formatted)["body"].toString().contains("Saved immediately on lock"),
                "lock flushes editor before closing database");
        if (app.arguments().contains("--capture-states")) {
            auto n = app.arguments().indexOf("--capture-states");
            QString dir = n + 1 < app.arguments().size() ? app.arguments()[n + 1] : ".";
            const auto mailboxPath = session.mailPath();
            session.lock();
            QTest::qWait(50);
            window.grab().save(dir + "/state1-locked.png");
            QTimer once;
            QObject::connect(&once, &QTimer::timeout, [&] {
                auto field = window.findChild<QLineEdit *>("vaultPasswordField");
                auto btn = window.findChild<QPushButton *>("vaultUnlockButton");
                if (field && btn && btn->isVisible()) {
                    field->setText("test password");
                    btn->click();
                }
            });
            once.start(20);
            const auto deadline = QDateTime::currentMSecsSinceEpoch() + 3000;
            while (!session.unlocked() && QDateTime::currentMSecsSinceEpoch() < deadline)
                QTest::qWait(20);
            once.stop();
            session.closeMailbox();
            QCoreApplication::processEvents();
            window.grab().save(dir + "/state2-nomailbox.png");
            session.openMailboxAt(mailboxPath);
            QCoreApplication::processEvents();
            window.grab().save(dir + "/state3-full.png");
        }
        session.lock();
        std::cout << "PASS Widgets mailbox selection, rendering and lock\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
