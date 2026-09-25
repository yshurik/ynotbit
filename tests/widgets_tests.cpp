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
        // Sender == recipient mirrors delivery.cpp's chanBroadcast handling: a chan post
        // signed by the chan's own shared key, not an individually-attributable member.
        mailbox.store("anon-in-recipient", "recipient", "recipient", "Anon post",
                      "Posted anonymously", 1601, "Channels");
        // The shape of the garbage actually seen on the public chan: uniformly
        // random printable ASCII with the odd control byte, not mostly control
        // characters. Deterministic so a failure reproduces.
        const auto noise = [](quint32 seed, int length) {
            QString out;
            for (int i = 0; i < length; ++i) {
                seed = seed * 1103515245u + 12345u;
                const auto r = (seed >> 16) % 100;
                out += r < 2 ? QChar(0x0b) : QChar(0x21 + int((seed >> 8) % 94));
            }
            return out;
        };
        const auto noiseSubject = noise(7, 60), noiseBody = noise(11, 900);
        mailbox.store("noise", "sender", "recipient", noiseSubject, noiseBody, 1603, "Inbox");
        const QString signed_ = "Nigh on.\n\n" + QString(60, '-') + "\nsig line one\n" +
                                QString(60, '-') + "\n" + QString(60, '=');
        mailbox.store("signed", "sender", "recipient", "Re: rule lines", signed_, 1604, "Inbox");
        // Decrypts fine but isn't text -- the reader should fall back to hex.
        mailbox.store("cryptic", "sender", "recipient", "\x01\x02 raw \x03\x04",
                      QString("\x01\x02\x03\x04 raw bytes \x05\x06\x07\x0e\x0f\x10\x11"), 1602,
                      "Inbox");
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
        const auto mono = [](const QFont &font) {
            QFontMetricsF metrics(font);
            return qAbs(metrics.horizontalAdvance("iiii") - metrics.horizontalAdvance("WWWW")) <
                   0.1;
        };
        require(mono(window.findChild<QLabel *>("messageAddresses")->font()),
                "message address headers use fixed width");
        auto selectChannel = [&](const QString &address) {
            for (auto chip : window.findChildren<QPushButton *>("channelChip"))
                if (chip->toolTip().contains(address)) {
                    chip->click();
                    QTest::qWait(20);
                    return;
                }
            require(false, "channel chip not found");
        };
        auto channelChips = window.findChildren<QPushButton *>("channelChip");
        require(channelChips.size() == 3, "stored and empty joined channels are listed");
        for (auto chip : channelChips)
            require(!chip->icon().isNull(), "each channel chip shows its address's identicon");
        require(channelChips[0]->icon().pixmap(18, 18).toImage() !=
                    channelChips[1]->icon().pixmap(18, 18).toImage(),
                "different channels render different identicons");
        // Captured here and compared again after a lock/unlock and a mailbox
        // reopen: an identicon is seeded from the address alone, so nothing
        // about the session, vault or mailbox may leak into it. Salting it
        // per install (the way PyBitmessage's identiconsuffix does) would make
        // the same address unrecognisable between machines.
        const auto chipIdenticon = channelChips[0]->icon().pixmap(18, 18).toImage();
        const auto chipAddress = channelChips[0]->toolTip();
        require(window.findChild<QWidget *>("channelRail")->isVisible(),
                "the channel rail is shown on the Channels folder");
        require(!window.findChild<QLabel *>("listHeading")->isVisible(),
                "the redundant list heading is hidden on the Channels folder");
        bool recipientChipUnread = false;
        for (auto chip : window.findChildren<QPushButton *>("channelChip"))
            if (chip->toolTip().contains("recipient") && !chip->toolTip().contains("second")) {
                recipientChipUnread = chip->font().bold();
            }
        require(recipientChipUnread,
                "a channel with unread mail shows its name in bold, not a text prefix or badge");
        selectChannel("recipient");
        require(window.findChild<QLabel *>("listHeading")->text() == "Channels",
                "the list heading stays generic; the selected chip already shows the active channel");
        for (auto chip : window.findChildren<QPushButton *>("channelChip"))
            if (chip->toolTip().contains("recipient") && !chip->toolTip().contains("second"))
                require(chip->isChecked() && chip->styleSheet().contains(":checked"),
                        "the active channel's chip is checked and visually distinct");
        require(list->model()->rowCount() == 1501, "all channel rows available");
        require(window.findChild<QToolButton *>("density_comfortable") &&
                    window.findChild<QToolButton *>("density_cozy") &&
                    window.findChild<QToolButton *>("density_compact"),
                "all three density buttons exist");
        {
            // Every icon in the list column must follow a theme switch, not
            // keep the colour it was drawn with at startup.
            auto iconOf = [&](const char *id) {
                return window.findChild<QToolButton *>(id)->icon().pixmap(12, 12).toImage();
            };
            auto themeTo = [&](const char *mode) {
                for (auto a : window.findChildren<QAction *>())
                    if (a->text() == mode)
                        a->trigger();
            };
            themeTo("light");
            // A stroke thinner than one pixel at the button's icon size only
            // ever renders as antialiased gray, which reads as a disabled
            // control. Every icon needs at least one fully-inked pixel.
            for (const char *id : {"filter_unread", "filter_anonymous", "density_comfortable",
                                   "density_cozy", "density_compact"}) {
                auto btn = window.findChild<QToolButton *>(id);
                const auto img = btn->icon().pixmap(btn->iconSize()).toImage().convertToFormat(
                    QImage::Format_ARGB32);
                int solid = 0;
                for (int y = 0; y < img.height(); ++y)
                    for (int x = 0; x < img.width(); ++x)
                        if (qAlpha(img.pixel(x, y)) >= 200)
                            ++solid;
                require(solid > 0,
                        "list column switch icons render solid strokes, not disabled-looking gray");
            }
            const auto lightDensity = iconOf("density_compact");
            const auto lightFilter = iconOf("filter_unread");
            themeTo("dark");
            require(iconOf("density_compact") != lightDensity,
                    "density icons recolour on a theme switch");
            require(iconOf("filter_unread") != lightFilter,
                    "filter icons recolour on a theme switch");
            themeTo("system");
        }
        {
            window.selectMessage("0"); // marks a non-anonymous recipient-channel message read
            auto unreadFilter = window.findChild<QToolButton *>("filter_unread");
            auto anonFilter = window.findChild<QToolButton *>("filter_anonymous");
            require(unreadFilter && anonFilter, "both filter buttons exist");
            auto countLabel = window.findChild<QLabel *>("listCountLabel");
            require(countLabel, "list count label exists");
            unreadFilter->click();
            require(list->model()->rowCount() == 1500,
                    "unread-only excludes the one message just read");
            require(countLabel->text() == "1500 of 1501 total", "count label reflects the filter");
            anonFilter->click();
            require(list->model()->rowCount() == 1,
                    "unread and anonymous combine as AND: narrows to the one unread anon post");
            require(countLabel->text() == "1 of 1501 total",
                    "count label reflects both filters combined");
            unreadFilter->click();
            require(list->model()->rowCount() == 1,
                    "anonymous-only alone still isolates the one anon post");
            anonFilter->click();
            require(list->model()->rowCount() == 1501,
                    "clearing both filters restores the full channel");
            require(countLabel->text() == "1501 total",
                    "count label drops the \"of\" qualifier once nothing is filtered");
        }
        {
            auto stripe = window.findChild<QWidget *>("letterKindStripe");
            require(stripe, "letter kind stripe exists");
            require(stripe->layout()->contentsMargins().left() > 0,
                    "the envelope frame always reserves a gap for its border");
            // The border is a striped band: a faint gray ground with coloured
            // stripes over it. Sample along its top edge and look for both --
            // a pixel where the kind's colour dominates, and variation along
            // the row (a solid fill would be the same colour all the way).
            struct Band { bool greenStripe = false, blueStripe = false; int shades = 0; };
            auto band = [&] {
                QCoreApplication::processEvents();
                const auto row = stripe->grab(QRect(0, 1, stripe->width(), 1)).toImage();
                Band b;
                QSet<QRgb> seen;
                for (int x = 0; x < row.width(); ++x) {
                    const auto c = row.pixelColor(x, 0);
                    seen.insert(c.rgb());
                    b.greenStripe |= c.green() > c.red() && c.green() > c.blue();
                    b.blueStripe |= c.blue() > c.red() && c.blue() > c.green();
                }
                b.shades = seen.size();
                return b;
            };
            window.selectMessage("0"); // a plain chan-personal message: from "sender", to "recipient"
            const auto chanPersonal = band();
            require(chanPersonal.blueStripe, "a chan-personal letter's border has blue stripes");
            require(chanPersonal.shades > 1,
                    "a chan-personal border is striped over the gray ground, not a solid fill");
            window.selectMessage(acknowledged); // an ordinary Outbox letter, not in the Channels folder
            const auto personal = band();
            require(personal.greenStripe,
                    "a plain personal letter (Inbox/Outbox/...) gets green stripes, not blue");
            require(personal.shades > 1,
                    "a personal border is striped over the gray ground, not a solid fill");
            window.selectMessage("anon-in-recipient");
            for (auto action : window.findChildren<QAction *>())
                if (action->text() == "Open in new window")
                    action->trigger();
            auto popped = window.findChild<QDialog *>("messageWindow");
            require(popped, "opens a separate message window");
            auto poppedStripe = popped->findChild<QWidget *>("windowKindStripe");
            require(poppedStripe && poppedStripe->layout()->contentsMargins().left() > 0,
                    "a message opened in its own window still shows its envelope border");
            popped->close();
        }
        {
            const bool capturingDensity = app.arguments().contains("--capture-density");
            QString densityDir;
            if (capturingDensity) {
                auto n = app.arguments().indexOf("--capture-density");
                densityDir = n + 1 < app.arguments().size() ? app.arguments()[n + 1] : ".";
                window.selectMessage("0");
                window.grab().save(densityDir + "/density-comfortable.png");
            }
            auto comfortableHeight = list->sizeHintForRow(0);
            window.findChild<QToolButton *>("density_compact")->click();
            auto compactHeight = list->sizeHintForRow(0);
            require(compactHeight < comfortableHeight, "compact density shrinks row height");
            if (capturingDensity)
                window.grab().save(densityDir + "/density-compact.png");
            window.findChild<QToolButton *>("density_cozy")->click();
            auto cozyHeight = list->sizeHintForRow(0);
            require(cozyHeight > compactHeight && cozyHeight < comfortableHeight,
                    "cozy density sits between compact and comfortable");
            if (capturingDensity)
                window.grab().save(densityDir + "/density-cozy.png");
            window.findChild<QToolButton *>("density_comfortable")->click();
            require(list->sizeHintForRow(0) == comfortableHeight,
                    "switching back to comfortable restores the original row height");
        }
        {
            // Comfortable density's icon is 34px, drawn at x=12. Scan the whole
            // icon box rather than one pixel: the identicon is a generated 5x5
            // grid, so any single cell -- the middle one included -- is legitimately
            // empty for some addresses.
            auto rowRect = list->visualRect(list->model()->index(0, 0));
            auto shot = list->viewport()->grab(rowRect).toImage();
            const int top = (rowRect.height() - 34) / 2;
            int painted = 0;
            for (int x = 12; x < 12 + 34; ++x)
                for (int y = top; y < top + 34; ++y)
                    if (shot.pixelColor(x, y) != window.palette().color(QPalette::Base))
                        ++painted;
            require(painted > 34 * 34 / 10,
                    "message rows render a sender identicon, not a blank row");
        }
        if (app.arguments().contains("--capture-channels")) {
            auto n = app.arguments().indexOf("--capture-channels");
            QString dir = n + 1 < app.arguments().size() ? app.arguments()[n + 1] : ".";
            window.selectMessage("0");
            window.grab().save(dir + "/channels.png");
        }
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
        selectChannel("second-recipient");
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
        {
            bool stillOnSecondRecipient = false;
            for (auto chip : window.findChildren<QPushButton *>("channelChip"))
                if (chip->isChecked())
                    stillOnSecondRecipient = chip->toolTip().contains("second-recipient");
            require(stillOnSecondRecipient, "channel selection survives folder navigation");
        }
        selectChannel(emptyChannel);
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
        selectChannel("recipient");
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
        {
            // New mail arriving mid-read forces MessageModel::reload() to do a full
            // beginResetModel()/endResetModel(), which Qt uses to clear the view's
            // selection -- must not also blank the already-open reader pane.
            window.selectMessage("7");
            auto readerBody = window.findChild<QTextBrowser *>("readerBody");
            const auto beforeReset = readerBody->toPlainText();
            require(!beforeReset.isEmpty(), "message body loaded before new mail arrives");
            vault.unlock(temp.filePath("vault"), "test password");
            bm::Mailbox injected;
            injected.open(temp.filePath("mailbox"), vault.mailboxKey(key));
            injected.store("new-arrival", "sender", "recipient", "New arrival",
                           "Freshly delivered while reading.", 1700, "Channels");
            injected.close();
            vault.lock();
            session.messageModel()->reload();
            require(list->model()->rowCount() == 1502,
                    "new mail is counted after the model reload");
            require(readerBody->toPlainText() == beforeReset,
                    "new mail arriving does not blank the reader pane mid-read");
            require(list->currentIndex().isValid() &&
                        list->currentIndex().data(Qt::UserRole + 1).toString() == "7",
                    "the previously selected message stays selected after new mail resets the list");
        }
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
        {
            // Scoped to the reader's own switch: a pop-out window carries an
            // identical one, and it is a child of this window too.
            auto readerViews = window.findChild<QWidget *>("viewSwitch");
            auto mode = [&](const char *id) { return readerViews->findChild<QToolButton *>(id); };
            auto subjectLabel = window.findChild<QLabel *>("subjectLabel");
            require(mode("view_plain") && mode("view_text") && mode("view_markdown") &&
                        mode("view_hex"),
                    "the reader offers all four view modes");
            for (auto btn : readerViews->findChildren<QToolButton *>())
                require(btn->iconSize().width() * 3 >= btn->width() * 2,
                        "view mode icons fill most of their button, not a small glyph in a big box");
            // The letter just selected is the markdown one, and it was detected
            // as such -- that is what rendered the list above.
            require(mode("view_markdown")->isChecked(),
                    "a markdown body opens in markdown mode");
            window.selectMessage("cryptic");
            require(mode("view_hex")->isChecked(), "an unprintable body opens in hex mode");
            require(reader->toPlainText().contains("00000000  01 02 03 04"),
                    "hex mode dumps offsets and bytes");
            require(mono(reader->font()) && mono(subjectLabel->font()),
                    "hex mode puts body and subject in a fixed-width font");
            require(bm::crypticLabel("7687d8a1b2c3") == "<cryptic-7687d8>",
                    "the list labels a cryptic letter by the first six hex digits of its hash");
            require(bm::looksCryptic(noiseSubject, noiseBody),
                    "random printable noise is cryptic even with only ~2% control bytes");
            require(bm::looksCryptic(noiseSubject.left(240), noiseBody.left(240)),
                    "and the 240-char list preview reaches the same verdict");
            require(!bm::looksCryptic("Re: rule lines", signed_) &&
                        !bm::looksCryptic("Re: rule lines", signed_.left(240)),
                    "a short letter with a long rule-line signature is not cryptic");
            window.selectMessage("noise");
            require(mode("view_hex")->isChecked(), "real-shaped noise opens in hex mode");
            require(QRegularExpression("^[0-9a-f]{2}( [0-9a-f]{2}){5}")
                        .match(subjectLabel->text()).hasMatch(),
                    "a cryptic letter's subject is shown as hex too");
            mode("view_text")->click();
            require(subjectLabel->text() == noiseSubject.simplified() ||
                        subjectLabel->text().startsWith(noiseSubject.left(10)),
                    "switching out of hex shows the raw subject again");
            window.selectMessage("signed");
            require(!mode("view_hex")->isChecked(), "the rule-line letter does not open in hex");
            window.selectMessage(session.saveLetter(
                {}, address, address, "Notes",
                "- Structure prevent fund military station wonder report.\n"
                "- Through hot hard industry kind.\n"
                "- Join rather west table political huge grow.\n\n"
                "1. first\n2. second\n",
                "direct"));
            require(mode("view_plain")->isChecked(),
                    "a dash or numbered list alone is not a markdown signal");
            window.selectMessage(acknowledged); // "A delivered letter." -- neither, so plain
            require(mode("view_plain")->isChecked(),
                    "an ordinary body opens in plain mode, not markdown");
            require(reader->toPlainText() == "A delivered letter.",
                    "plain mode shows the body verbatim");
            require(mono(reader->font()) && mono(subjectLabel->font()),
                    "plain mode is the fixed-width one");
            mode("view_text")->click();
            require(!mono(reader->font()), "switching to text mode drops the fixed-width font");
            require(reader->toPlainText() == "A delivered letter.",
                    "text mode still shows the body verbatim");
            mode("view_hex")->click();
            require(reader->toPlainText().startsWith("00000000  41 20 64 65"),
                    "the reader can be switched to hex by hand");
            mode("view_markdown")->click();
            require(reader->toPlainText() == "A delivered letter.",
                    "and back out of hex again");
            // The pop-out window detects and switches on its own.
            window.selectMessage("cryptic");
            for (auto action : window.findChildren<QAction *>())
                if (action->text() == "Open in new window")
                    action->trigger();
            auto popout = window.findChildren<QDialog *>("messageWindow").last();
            auto popViews = popout->findChild<QWidget *>("windowViewSwitch");
            require(popViews, "the pop-out window has its own view switch");
            auto popMode = [&](const char *id) { return popViews->findChild<QToolButton *>(id); };
            auto popBody = popout->findChild<QTextBrowser *>("windowBody");
            require(popMode("view_hex")->isChecked() &&
                        popBody->toPlainText().contains("00000000  01 02 03 04"),
                    "the pop-out opens an unprintable body in hex too");
            popMode("view_text")->click();
            require(popBody->toPlainText().startsWith("\x01\x02\x03\x04 raw bytes"),
                    "the pop-out can be switched by hand");
            require(mode("view_hex")->isChecked(),
                    "switching the pop-out leaves the reader's own mode alone");
            popout->close(); // WA_DeleteOnClose only schedules it; flush so later
                             // findChild("messageWindow") lookups don't find it
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            window.selectMessage(formatted); // leave the markdown letter open for what follows
        }
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
            folders->setCurrentRow(0);
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
        selectChannel("recipient");
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
        folders->setCurrentRow(0);
        QCoreApplication::processEvents();
        QTimer::singleShot(30, &window, [&] {
            auto dialog = window.findChild<QDialog *>("composer");
            auto sigBody = dialog->findChild<QTextEdit *>("bodyField");
            require(sigBody->toPlainText().contains("ynotbit"),
                    "a brand-new letter is pre-filled with a default signature");
            require(sigBody->toPlainText().startsWith("\n\n-- "),
                    "two blank lines separate the cursor position from the signature");
            require(sigBody->textCursor().position() == 0,
                    "the cursor starts on the first blank line, not inside the signature");
            dialog->reject();
        });
        window.findChild<QPushButton *>("writeButton")->click();
        const auto existingDraft = session.saveLetter({}, address, address, "Existing draft",
                                                       "Already written", "direct");
        QTimer::singleShot(30, &window, [&] {
            auto dialog = window.findChild<QDialog *>("composer");
            require(!dialog->findChild<QTextEdit *>("bodyField")->toPlainText().contains("ynotbit"),
                    "reopening an existing draft does not re-inject the signature");
            dialog->reject();
        });
        window.compose({{"hash", existingDraft}});
        folders->setCurrentRow(1);
        QCoreApplication::processEvents();
        const auto draftToOpen =
            session.saveLetter({}, address, address, "Draft to reopen", "Unfinished", "direct");
        // compose() blocks on dialog.exec(), so the dialog must be found and closed from
        // a timer armed before the double-click, not from code after it -- that code
        // would never run until the (never-closed) dialog returns.
        QTimer::singleShot(50, &window, [&] {
            require(!window.findChild<QDialog *>("messageWindow"),
                    "double-clicking a draft does not open the read-only viewer");
            auto draftComposer = window.findChild<QDialog *>("composer");
            require(draftComposer, "double-clicking a draft opens it in the composer instead");
            require(draftComposer->findChild<QLineEdit *>("subjectField")->text() ==
                        "Draft to reopen",
                    "the composer opens with the draft's own content, not a blank letter");
            draftComposer->findChild<QPushButton *>("discardButton")->click();
        });
        for (int row = 0; row < list->model()->rowCount(); ++row) {
            auto index = list->model()->index(row, 0);
            if (index.data(Qt::UserRole + 1).toString() != draftToOpen)
                continue;
            emit list->doubleClicked(index);
            break;
        }
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
            body->clear();
            QApplication::clipboard()->setText(
                "**Bold word** and a heading:\n\n## Section\n\nplain line after");
            QTest::keyClick(body, Qt::Key_V, Qt::ControlModifier);
            require(!body->toPlainText().contains('*'),
                    "pasted markdown source is parsed, not left with literal ** markers");
            bool sawBold = false, sawHeading = false;
            for (auto b = body->document()->begin(); b.isValid(); b = b.next()) {
                if (b.blockFormat().headingLevel() == 2)
                    sawHeading = true;
                for (auto it = b.begin(); !it.atEnd(); ++it)
                    if (it.fragment().isValid() &&
                        it.fragment().charFormat().fontWeight() == QFont::Bold)
                        sawBold = true;
            }
            require(sawBold, "pasted **bold** actually renders bold, not just stripped markers");
            require(sawHeading, "pasted '## Section' becomes a real heading block");
            // A dash-prefixed line alone is common in ordinary prose/notes; only
            // a real multi-line list should be reinterpreted as markdown.
            body->clear();
            QApplication::clipboard()->setText("- just one line, not a list");
            QTest::keyClick(body, Qt::Key_V, Qt::ControlModifier);
            require(body->toPlainText() == "- just one line, not a list",
                    "a single dash-prefixed line pastes as literal text, not a list");
            require(!body->textCursor().currentList(),
                    "a single dash-prefixed line is not turned into a real list");
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
        {
            window.findChild<QToolButton *>("folderIcon_Identities")->click();
            require(!window.findChild<QWidget *>("listColumn")->isVisible(),
                    "identities screen uses the full width, no message list column");
            require(window.findChild<QWidget *>("identitiesPane")->isVisible(),
                    "identities pane shown");
            require(!window.findChild<QLabel *>("subjectLabel")->isVisible(),
                    "the reader's subject label does not float above the identities pane");
            require(!window.findChild<QScrollArea *>("subjectScroll")->isVisible(),
                    "the subject label's empty scroll chrome does not float above the identities "
                    "pane either");
            auto addressLabels = window.findChildren<QLabel *>("identityAddress");
            require(addressLabels.size() == 2, "one card per identity");
            QStringList shown;
            for (auto l : addressLabels)
                shown << l->text();
            require(shown.contains(address) && shown.contains(emptyChannel),
                    "both identities are listed");
            require(window.findChild<QLabel *>("defaultBadge"),
                    "the default identity shows a badge");
            require(window.findChild<QLabel *>("channelBadge"),
                    "the chan identity shows a Channel badge");
            require(window.findChildren<QPushButton *>("setDefaultButton").size() == 1,
                    "only the non-default identity offers Set as default");
            auto identiconFor = [&](const QString &addr) -> QImage {
                for (auto card : window.findChildren<QFrame *>("identityCard")) {
                    auto addrLabel = card->findChild<QLabel *>("identityAddress");
                    if (addrLabel && addrLabel->text() == addr) {
                        auto icon = card->findChild<QLabel *>("identityIdenticon");
                        return icon ? icon->pixmap().toImage() : QImage();
                    }
                }
                return QImage();
            };
            require(window.findChildren<QLabel *>("identityIdenticon").size() == 2,
                    "each identity card shows an identicon");
            auto personalIcon = identiconFor(address);
            auto chanIcon = identiconFor(emptyChannel);
            require(!personalIcon.isNull() && !chanIcon.isNull(), "identicon pixmaps render");
            require(personalIcon.size() == QSize(40, 40),
                    "identicon scaled to the card avatar size");
            require(personalIcon != chanIcon,
                    "different addresses render different identicons (real MD5 seed, not a "
                    "placeholder)");
            bool hasOpaquePixel = false;
            for (int y = 0; y < personalIcon.height() && !hasOpaquePixel; ++y)
                for (int x = 0; x < personalIcon.width(); ++x)
                    if (qAlpha(personalIcon.pixel(x, y)) > 0) {
                        hasOpaquePixel = true;
                        break;
                    }
            require(hasOpaquePixel,
                    "identicon actually paints visible patches, not a blank transparent square");
            if (app.arguments().contains("--capture-identities")) {
                auto n = app.arguments().indexOf("--capture-identities");
                QString dir = n + 1 < app.arguments().size() ? app.arguments()[n + 1] : ".";
                window.grab().save(dir + "/identities.png");
            }

            window.findChild<QToolButton *>("copyAddressButton")->click();
            auto copied = QApplication::clipboard()->text();
            require(copied == address || copied == emptyChannel,
                    "copy button copies that card's address");

            window.findChild<QPushButton *>("setDefaultButton")->click();
            QTest::qWait(20);
            bool channelIsDefault = false;
            for (auto v : session.identities())
                if (v.toMap()["address"].toString() == emptyChannel)
                    channelIsDefault = v.toMap()["default"].toBool();
            require(channelIsDefault, "Set as default promotes the chosen identity");
            require(identiconFor(address) == personalIcon,
                    "the same address renders the identical identicon after the cards rebuild");

            QTimer::singleShot(0, &window, [&] {
                auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (box)
                    box->button(QMessageBox::Yes)->click();
            });
            auto beforeDelete = session.identities().size();
            window.findChild<QToolButton *>("deleteIdentityButton")->click();
            QTest::qWait(20);
            require(session.identities().size() == beforeDelete - 1,
                    "confirming the delete dialog removes the identity");

            bool qrFound = false;
            QTimer::singleShot(0, &window, [&] {
                auto qr = window.findChild<QDialog *>("qrDialog");
                qrFound = qr && qr->isVisible();
                if (qr)
                    qr->close();
            });
            window.findChild<QToolButton *>("showQrButton")->click();
            require(qrFound, "the QR button opens a visible QR dialog");

            auto beforeAdd = session.identities().size();
            window.findChild<QPushButton *>("newIdentityButton")->click();
            QTest::qWait(50);
            require(session.identities().size() == beforeAdd + 1,
                    "New identity adds an identity via the label prompt");

            window.findChild<QToolButton *>("renameIdentityButton")->click();
            QTest::qWait(50);
            bool renamed = false;
            for (auto l : window.findChildren<QLabel *>("identityName"))
                renamed = renamed || l->text() == "test password";
            require(renamed, "Rename updates the identity's label via the same prompt");
            window.findChild<QToolButton *>("folderIcon_Inbox")->click();
        }
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
        {
            const bool capturingStates = app.arguments().contains("--capture-states");
            QString dir;
            if (capturingStates) {
                auto n = app.arguments().indexOf("--capture-states");
                dir = n + 1 < app.arguments().size() ? app.arguments()[n + 1] : ".";
            }
            auto subjectLabel = window.findChild<QLabel *>("subjectLabel");
            const auto mailboxPath = session.mailPath();
            session.lock();
            QTest::qWait(50);
            require(subjectLabel && !subjectLabel->isVisible(),
                    "locked state hides the reader's subject label");
            require(window.findChild<QWidget *>("letterKindStripe") &&
                        !window.findChild<QWidget *>("letterKindStripe")->isVisible(),
                    "locked state hides the reader's envelope frame too, so the "
                    "vault password screen isn't split with an empty stretch");
            // Focus itself isn't checked here: QT_QPA_PLATFORM=offscreen (this
            // test's environment) never marks a window active, and
            // QApplication::focusWidget() depends on that -- confirmed by
            // spiking the assertion and finding it always false regardless of
            // whether setFocus() was actually called. The fix is in
            // DesktopWindow::updateState(): setFocus() is called on
            // vaultPasswordField_ only after welcomeStack_->setCurrentWidget()
            // makes the locked page current, not before (a hidden widget can't
            // hold focus), verified visually via --capture-states.
            if (capturingStates)
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
            require(!subjectLabel->isVisible(),
                    "no-mailbox state hides the reader's subject label");
            if (capturingStates)
                window.grab().save(dir + "/state2-nomailbox.png");
            session.openMailboxAt(mailboxPath);
            QCoreApplication::processEvents();
            require(subjectLabel->isVisible(),
                    "full-mailbox state shows the reader's subject label again");
            if (capturingStates)
                window.grab().save(dir + "/state3-full.png");
            folders->setCurrentRow(4);
            QTest::qWait(50);
            bool rechecked = false;
            for (auto chip : window.findChildren<QPushButton *>("channelChip"))
                if (chip->toolTip() == chipAddress) {
                    require(chip->icon().pixmap(18, 18).toImage() == chipIdenticon,
                            "an address keeps the same identicon across a lock, unlock and "
                            "mailbox reopen -- nothing per-session salts it");
                    rechecked = true;
                }
            require(rechecked, "found the same channel chip again after reopening the mailbox");
        }
        session.lock();
        std::cout << "PASS Widgets mailbox selection, rendering and lock\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
