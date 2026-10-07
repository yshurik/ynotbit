#include "desktop_window.h"
#include "session.h"
#include "ntb-object-db.h"
#include "updates.h"
#include "feed_view.h"
#include "protocol.h"
#include "letter_document.h"
#include "pow.h"
#include "settings_window.h"
#include <QElapsedTimer>
#include <QFileDialog>
#include <QTest>
#include <QtWidgets>
#include <functional>
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
// The filter searches on a worker thread; let it finish before reading the list.
static bool searched(bm::MessageModel *model) {
    QElapsedTimer clock;
    clock.start();
    while (model->searching() && clock.elapsed() < 20000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return !model->searching();
}
// Answers an open file dialog. QFileDialog::selectFile() won't overwrite the
// name box while it has focus, and on a busy machine it has focus by the time
// a test answers: the dialog then stays open, empty, and the test hangs.
static void chooseFile(QFileDialog *d, const QString &path) {
    if (auto focused = d->focusWidget())
        focused->clearFocus();
    d->selectFile(path);
    static_cast<QDialog *>(d)->accept(); // QFileDialog's own accept() is protected
}
int main(int argc, char **argv) {
    // Unbuffered: a run that hangs and is killed by ctest still shows how far it got.
    std::cout << std::unitbuf;
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    // A modal dialog nobody answers blocks until ctest's 480 s timeout, without
    // a clue: before that, name the windows still open, then fail.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [] {
        std::cerr << "HUNG: still running after 450 s; open windows:\n";
        for (auto w : QApplication::topLevelWidgets())
            if (w->isVisible())
                std::cerr << "  " << w->metaObject()->className() << " \""
                          << w->objectName().toStdString() << "\" \""
                          << w->windowTitle().toStdString() << '"'
                          << (w == QApplication::activeModalWidget() ? " (active modal)" : "")
                          << '\n';
        std::_Exit(3);
    });
    watchdog.start(450000);
    app.setOrganizationName("YnotbitTests");
    app.setApplicationName("Widgets");
    QTemporaryDir temp;
    // A long vault path, like a macOS runner's /var/folders/.../T/...: the
    // locked screen must show it whole-height there too.
    const QString vaultFile =
        temp.filePath("var/folders/xz/q1w2e3r4t5y6u7i8o9p0asdfghjk/T/vaults-of-the-test/vault");
    QDir().mkpath(QFileInfo(vaultFile).absolutePath());
    auto require = [](bool ok, const char *message) {
        if (!ok) {
            // Also printed here: a failure inside a dialog callback is thrown
            // through Qt's event loop, which aborts before main() reports it.
            std::cerr << "FAIL: " << message << "\n";
            throw std::runtime_error(message);
        }
    };
    try {
        for (const auto &icon : {bm::appLogo(), bm::windowLogo()}) {
            for (int size : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256, 512, 1024}) {
                require(icon.availableSizes().contains(QSize(size, size)),
                        "brand icon embeds every authored optical size");
                const auto image = icon.pixmap(QSize(size, size), 1.0).toImage();
                require(!image.isNull() && image.pixelColor(0, 0).alpha() == 0,
                        "brand icon decodes with transparent corners");
                bool red = false, blue = false;
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x) {
                        const auto color = image.pixelColor(x, y);
                        if (color.alpha() < 128)
                            continue;
                        red |= color.red() > color.blue() + 60;
                        blue |= color.blue() > color.red() + 60;
                    }
                require(red && blue, "airmail stripes survive at every icon size");
            }
        }
        bm::Vault vault;
        vault.create(vaultFile, "test password");
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
            settings.setValue("vault", vaultFile);
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
        // The built-in subscriptions get a test of their own below; elsewhere
        // the rail holds only what each test adds.
        bm::updates::setDefaultSubscriptionsForTesting({});
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
        require(window.findChild<QPushButton *>("writeButton")->icon().pixmap(24, 24).toImage() !=
                    window.findChild<QAction *>("editAction")->icon().pixmap(24, 24).toImage(),
                "the write button has its own new-letter icon, not the draft edit pencil");
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
        // The <pre> address part only: the tooltip also carries the chan's
        // name, which is renamed later on.
        const auto chipAddress =
            channelChips[0]->toolTip().mid(channelChips[0]->toolTip().indexOf("<pre>"));
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
        {
            // The search box names the chan it searches, as its chip shows it.
            auto searchBox = window.findChild<QLineEdit *>("messageSearch");
            auto placeholderFor = [&](const QString &address) {
                selectChannel(address);
                QString label;
                for (auto chip : window.findChildren<QPushButton *>("channelChip"))
                    if (chip->isChecked())
                        label = chip->text();
                return std::make_pair(searchBox->placeholderText(), label);
            };
            auto [second, secondLabel] = placeholderFor("second-recipient");
            require(second == "Search " + secondLabel,
                    "the chan search box says which chan it searches");
            auto [first, firstLabel] = placeholderFor("recipient");
            require(first == "Search " + firstLabel && first != second,
                    "and follows the selected chan");
        }
        {
            // Collapsing the chan rail leaves a narrow column of identicons.
            auto rail = window.findChild<QWidget *>("channelRail");
            auto toggle = window.findChild<QPushButton *>("channelRailToggle");
            auto heading = window.findChild<QLabel *>("channelRailHeading");
            auto join = window.findChild<QPushButton *>("joinOrCreateChannelButton");
            require(toggle && toggle->text() == "<<<" && heading->text() == "CHANNELS",
                    "the rail starts expanded, with a collapse button");
            const int wide = rail->width();
            toggle->click();
            QCoreApplication::processEvents();
            const auto chips = window.findChildren<QPushButton *>("channelChip");
            bool iconsOnly = !chips.isEmpty();
            for (auto chip : chips)
                iconsOnly = iconsOnly && chip->text().isEmpty() && !chip->icon().isNull();
            require(rail->width() < wide / 3 && heading->text() == "#" && join->text() == "+" &&
                        toggle->text() == ">>>",
                    "collapsed: narrow rail, '#' heading, '+' join and '>>>' expand");
            require(iconsOnly, "collapsed chips show only their identicons");
            bool activeKept = false;
            for (auto chip : chips)
                activeKept = activeKept || chip->isChecked();
            require(activeKept, "the active chan stays selected while collapsed");
            toggle->click();
            QCoreApplication::processEvents();
            require(rail->width() == wide && heading->text() == "CHANNELS" &&
                        !window.findChildren<QPushButton *>("channelChip").first()->text().isEmpty(),
                    "expanding restores the named list");
        }
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
                window.findChild<QAction *>(QString("appearance_") + mode)->trigger();
            };
            themeTo("light");
            {
                auto language = window.findChild<QMenu *>("languageMenu");
                auto appearance =
                    window.findChild<QAction *>("appearance_light")->associatedObjects();
                bool underAppearance = false;
                for (auto owner : appearance)
                    if (auto menu = qobject_cast<QMenu *>(owner))
                        underAppearance |= menu->actions().contains(language->menuAction());
                require(underAppearance &&
                            !window.menuBar()->actions().contains(language->menuAction()),
                        "Language is a submenu of Appearance, not its own top-level menu");
            }
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
            {
                auto openWindow = window.findChild<QAction *>("openWindowAction");
                auto actionsBar = window.findChild<QToolBar *>("actionsToolbar");
                require(openWindow && actionsBar->actions().contains(openWindow),
                        "open-in-new-window is a toolbar button");
                require(!window.findChild<QToolButton *>("moreActionsButton"),
                        "there is no More button any more");
                for (auto name : {"restoreAction", "deletePermanentlyAction", "retryAction",
                                  "cancelDeliveryAction"}) {
                    auto action = window.findChild<QAction *>(name);
                    require(action && actionsBar->actions().contains(action) &&
                                !action->icon().isNull(),
                            "Trash / Outbox actions are toolbar icons");
                }
            }
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
        require(searched(session.messageModel()) && session.messageModel()->rowCount() == 1,
                "search finds old rows");
        session.messageModel()->setSearch("");
        window.selectMessage("0");
        selectChannel("second-recipient");
        require(list->model()->rowCount() == 1, "second channel excludes first channel messages");
        require(list->model()->index(0, 0).data(Qt::UserRole + 1) == "other-channel",
                "channel page has correct recipient");
        require(window.findChild<QTextBrowser *>("readerBody")->toPlainText().isEmpty(),
                "switching channel clears previous body");
        session.messageModel()->setSearch("Subject");
        require(searched(session.messageModel()) && list->model()->rowCount() == 0,
                "search stays within selected channel");
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
            // Kinds as LetterKind numbers them: Personal, ChanPersonal, ChanAnonymous, Broadcast.
            auto stripe = dialog->findChild<QWidget *>("composerKindStripe");
            require(stripe && stripe->property("letterKind").toInt() == 1,
                    "a personal chan post gets the chan-personal border");
            chanModePublic->click();
            require(stripe->property("letterKind").toInt() == 2,
                    "switching to Anonymous changes the border to chan-anonymous");
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
            vault.unlock(vaultFile, "test password");
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
        {
            // A release announcement the scan recorded shows a banner until dismissed.
            bm::updates::setPublisherAddressForTesting("BM-releases");
            QCoreApplication::setApplicationVersion("0.5.1");
            vault.unlock(vaultFile, "test password");
            bm::Mailbox injected;
            injected.open(temp.filePath("mailbox"), vault.mailboxKey(key));
            injected.setSetting(bm::updates::kLatestSetting, "0.6.0");
            injected.close();
            vault.lock();
            session.clearError();
            auto banner = window.findChild<QWidget *>("updateBanner");
            auto toggle = window.findChild<QAction *>("updateNoticesAction");
            require(banner && !banner->isHidden() &&
                        window.findChild<QLabel *>("updateLabel")->text().contains("0.6.0"),
                    "a newer announced version shows the update banner");
            require(toggle && toggle->isEnabled() && toggle->isChecked(),
                    "update notices are on by default");
            toggle->trigger();
            require(banner->isHidden() && !session.updateNotices(),
                    "turning notices off hides the banner");
            toggle->trigger();
            require(!banner->isHidden(), "turning notices on again shows it");
            window.findChild<QPushButton *>("updateDismissButton")->click();
            require(banner->isHidden(), "a dismissed version is not shown again");
            QCoreApplication::setApplicationVersion("0.6.0");
            session.clearError();
            require(banner->isHidden(), "the running version is not announced as new");
            bm::updates::setPublisherAddressForTesting({});
            session.clearError();
            require(!toggle->isEnabled(), "a build without a publisher has nothing to toggle");
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
        const auto stand = [reader](const QString &name) {
            return reader->document()
                ->resource(QTextDocument::ImageResource, QUrl(name))
                .value<QImage>();
        };
        require(stand("file:///etc/passwd") == stand("https://example.com/a.png"),
                "reader blocks local resources, as remote ones");
        {
            // Scoped to the reader's own switch: a pop-out window carries an
            // identical one, and it is a child of this window too.
            auto readerViews = window.findChild<QWidget *>("viewSwitch");
            auto mode = [&](const char *id) { return readerViews->findChild<QToolButton *>(id); };
            auto subjectLabel = window.findChild<QTextEdit *>("subject");
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
            require(subjectLabel->toPlainText() == bm::hexDump(noiseSubject.toUtf8()),
                    "a cryptic letter's subject is dumped in exactly the body's hex format");
            require(subjectLabel->font().weight() == reader->font().weight() &&
                        subjectLabel->font().family() == reader->font().family() &&
                        subjectLabel->font().pixelSize() == reader->font().pixelSize(),
                    "and in the body's font, not bold");
            mode("view_text")->click();
            require(subjectLabel->toPlainText() == noiseSubject,
                    "switching out of hex shows the raw subject again");
            require(subjectLabel->font().weight() > QFont::Normal,
                    "and the subject is bold again outside hex");
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
            // Compared by family, not measured: offscreen Windows has no real
            // font files, so every font measures as fixed-pitch there.
            require(reader->font().family() != bm::addressFont().family(),
                    "switching to text mode drops the fixed-width font");
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
        require(popped->findChild<QTextBrowser *>("windowBody")->verticalScrollBarPolicy() ==
                    Qt::ScrollBarAlwaysOn,
                "separate window body always shows its scrollbar");
        require(window.findChild<QToolBar *>("actionsToolbar")->mapTo(&window, QPoint()).y() <
                    window.findChild<QTextEdit *>("subject")->mapTo(&window, QPoint()).y(),
                "main window toolbar renders above the subject");
        require(popped->findChild<QToolBar *>("windowActionsToolbar")->mapTo(popped, QPoint()).y() <
                    popped->findChild<QTextEdit *>("windowSubject")
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
        auto subjectLabel = window.findChild<QTextEdit *>("subject");
        require(subjectLabel && subjectLabel->toPlainText() == QString(500, 'L'),
                "long subjects show the full text, not truncated");
        {
            const auto lines = subjectLabel->height() / QFontMetrics(subjectLabel->font()).lineSpacing();
            require(lines == 4, "the subject area is four lines tall");
            require(subjectLabel->verticalScrollBar()->maximum() > 0,
                    "and a longer subject scrolls inside it");
            require(subjectLabel->isReadOnly() &&
                        subjectLabel->textInteractionFlags() & Qt::TextSelectableByMouse,
                    "the subject is a read-only, selectable text area");
            subjectLabel->selectAll();
            subjectLabel->copy();
            require(QApplication::clipboard()->text() == QString(500, 'L'),
                    "selecting and copying the subject yields the full text");
        }
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "Open in new window")
                action->trigger();
        auto longPopped = window.findChild<QDialog *>("messageWindow");
        require(longPopped, "opens a separate window for the long-subject letter too");
        auto subjectScroll = longPopped->findChild<QTextEdit *>("windowSubject");
        require(subjectScroll && subjectScroll->height() == subjectLabel->height(),
                "the separate window's subject area is four lines tall too");
        longPopped->close();
        QCoreApplication::processEvents();
        for (auto action : window.findChildren<QAction *>())
            require(action->text() != "Copy subject", "no Copy subject menu item any more");
        {
            const auto binned = session.saveLetter({}, address, address, "Binned", "x", "direct");
            session.moveLetter(binned, "Trash");
            window.selectMessage(binned);
            auto restore = window.findChild<QAction *>("restoreAction");
            require(restore->isVisible() &&
                        window.findChild<QAction *>("deletePermanentlyAction")->isVisible() &&
                        !window.findChild<QAction *>("retryAction")->isVisible(),
                    "a trashed letter shows Restore and Delete permanently on the toolbar");
            restore->trigger();
            QCoreApplication::processEvents();
            window.selectMessage(binned);
            require(!restore->isVisible(), "Restore takes the letter back out of the Trash");
        }
        {
            // --- Address book ---
            const auto alice = bm::Protocol::identity("Alice for the address book").address;
            require(!session.contactProblem("BM-not-an-address").isEmpty(),
                    "an invalid address cannot be a contact");
            require(!session.contactProblem(address).isEmpty(),
                    "your own identity cannot be a contact");
            require(session.contactProblem(alice).isEmpty(), "a foreign address can be one");
            // A draft to Alice: the reader offers to save the unknown recipient.
            const auto toAlice =
                session.saveLetter({}, address, alice, "Hello Alice", "hi", "direct");
            window.selectMessage(toAlice);
            QCoreApplication::processEvents();
            auto addRecipient = window.findChild<QToolButton *>("addRecipientContact");
            auto addSender = window.findChild<QToolButton *>("addSenderContact");
            auto toName = window.findChild<QLabel *>("toName");
            auto fromName = window.findChild<QLabel *>("fromName");
            require(addRecipient->isVisible() && !toName->isVisible(),
                    "an unknown recipient shows an add-to-contacts button and no name");
            require(!addSender->isVisible() && fromName->isVisible() && !fromName->text().isEmpty(),
                    "your own address shows its identity name and no add button");
            QTimer::singleShot(30, &window, [&] {
                auto dialog = window.findChild<QDialog *>("contactDialog");
                require(dialog && dialog->findChild<QLineEdit *>("contactAddressField")->text() == alice,
                        "the add dialog arrives with the address filled in");
                auto save = dialog->findChild<QPushButton *>("contactSaveButton");
                require(save->isEnabled(), "a valid new address can be saved straight away");
                dialog->findChild<QLineEdit *>("contactNameField")->setText("Alice");
                save->click();
            });
            addRecipient->click();
            QCoreApplication::processEvents();
            require(session.nameFor(alice) == "Alice" && toName->isVisible() &&
                        toName->text() == "Alice" && !addRecipient->isVisible(),
                    "once saved, the reader names the recipient and the button goes away");
            require(window.findChild<QLabel *>("toAddress")->text() == alice,
                    "the full address stays visible beside the name");
            {
                const auto row = list->model()->index(session.messageModel()->rowForHash(toAlice), 0);
                require(!row.isValid() || row.data(Qt::UserRole + 13).toString() == "Alice",
                        "list rows carry the correspondent's name");
            }
            // An address already in the book: the dialog says so, and saving renames.
            QTimer::singleShot(30, &window, [&] {
                auto dialog = window.findChild<QDialog *>("contactDialog");
                auto addressField = dialog->findChild<QLineEdit *>("contactAddressField");
                addressField->setText("BM-garbage");
                require(!dialog->findChild<QPushButton *>("contactSaveButton")->isEnabled() &&
                            !dialog->findChild<QLabel *>("contactProblem")->text().isEmpty(),
                        "an invalid address is explained and cannot be saved");
                addressField->setText(alice);
                require(dialog->findChild<QLabel *>("contactProblem")->text().contains("Alice"),
                        "a known address shows its current name");
                dialog->findChild<QPushButton *>("contactCancelButton")->click();
            });
            window.findChild<QPushButton *>("addContactButton")->click();
            // The Contacts page.
            folders->setCurrentRow(9);
            QCoreApplication::processEvents();
            require(window.findChild<QWidget *>("contactsPane")->isVisible() &&
                        !window.findChild<QWidget *>("listColumn")->isVisible() &&
                        !window.findChild<QWidget *>("identitiesPane")->isVisible(),
                    "Contacts is a whole-pane page");
            auto cards = [&] { return window.findChildren<QFrame *>("contactCard"); };
            require(cards().size() == 1 &&
                        cards()[0]->findChild<QLabel *>("contactName")->text() == "Alice" &&
                        cards()[0]->findChild<QLabel *>("contactAddress")->text() == alice,
                    "the contact is listed with its name and address");
            auto filter = window.findChild<QLineEdit *>("contactsFilter");
            filter->setText("nobody-matches");
            require(cards().isEmpty() && window.findChild<QLabel *>("contactsEmpty"),
                    "a filter with no match says so");
            filter->setText(alice.mid(5, 6));
            require(cards().size() == 1, "the filter matches addresses too");
            filter->clear();
            // Write opens the composer to the contact, with the address book wired in.
            QTimer::singleShot(30, &window, [&] {
                auto dialog = window.findChild<QDialog *>("composer");
                auto to = dialog->findChild<QLineEdit *>("recipientField");
                require(to->text() == alice, "Write addresses the letter to the contact");
                require(dialog->findChild<QPushButton *>("contactsPickerButton")->isVisibleTo(dialog),
                        "the composer offers the contacts picker");
                auto completion = to->completer()->model();
                require(completion->rowCount() == 1 &&
                            completion->index(0, 0).data().toString().startsWith("Alice"),
                        "the recipient field completes on contact names");
                to->clear();
                dialog->findChild<QMenu *>("contactsPickerMenu")->actions().first()->trigger();
                require(to->text() == alice, "picking a contact fills in the address");
                dialog->reject();
            });
            cards()[0]->findChild<QPushButton *>("writeToContactButton")->click();
            // Rename, then delete (confirmed).
            QTimer::singleShot(30, &window, [&] {
                auto dialog = window.findChild<QDialog *>("contactDialog");
                require(dialog->findChild<QLineEdit *>("contactAddressField")->isReadOnly(),
                        "renaming keeps the address");
                dialog->findChild<QLineEdit *>("contactNameField")->setText("Alice Liddell");
                dialog->findChild<QPushButton *>("contactSaveButton")->click();
            });
            cards()[0]->findChild<QToolButton *>("renameContactButton")->click();
            QTest::qWait(20);
            require(session.nameFor(alice) == "Alice Liddell" &&
                        cards()[0]->findChild<QLabel *>("contactName")->text() == "Alice Liddell",
                    "rename updates the book and the card");
            // The confirmation is a message box on the window; answer Yes once it shows.
            QTimer confirm;
            QObject::connect(&confirm, &QTimer::timeout, &window, [&] {
                for (auto box : window.findChildren<QMessageBox *>())
                    if (box->isVisible())
                        box->button(QMessageBox::Yes)->click();
            });
            confirm.start(10);
            cards()[0]->findChild<QToolButton *>("deleteContactButton")->click();
            confirm.stop();
            QTest::qWait(20);
            require(cards().isEmpty() && session.contacts().isEmpty() && session.nameFor(alice).isEmpty(),
                    "delete removes the contact");
            folders->setCurrentRow(0);
            session.moveLetter(toAlice, "Trash");
            QCoreApplication::processEvents();
        }
        {
            // --- Letter size: a meter against the network's limit; a letter
            // over it is kept as a draft but can't be sent. ---
            QString bigDraft;
            QTimer::singleShot(30, &window, [&] {
                auto dialog = window.findChild<QDialog *>("composer");
                auto send = dialog->findChild<QPushButton *>("sendButton");
                auto label = dialog->findChild<QLabel *>("sizeLabel");
                auto bar = dialog->findChild<QProgressBar *>("sizeBar");
                require(label && bar && label->text().endsWith(" of 255 kB") && send->isEnabled(),
                        "the composer shows the letter's size against the 255 kB limit");
                dialog->findChild<QLineEdit *>("recipientField")->setText(address);
                dialog->findChild<QLineEdit *>("subjectField")->setText("Too big");
                auto body = dialog->findChild<QTextEdit *>("bodyField");
                body->setPlainText(QString(270000, 'x'));
                QTest::qWait(400);
                require(!send->isEnabled() && bar->property("over").toBool() &&
                            bar->value() == bar->maximum() && !send->toolTip().isEmpty(),
                        "a letter over the limit can't be sent, and the meter says so");
                body->setPlainText("Small again");
                QTest::qWait(400);
                require(send->isEnabled() && send->isDefault() && !bar->property("over").toBool(),
                        "trimming it enables Send again");
                body->setPlainText(QString(270000, 'x'));
                dialog->findChild<QPushButton *>("saveDraftButton")->click();
            });
            window.compose();
            QCoreApplication::processEvents();
            for (auto m : session.messagePage("Drafts", {}, 0, 100))
                if (m.toMap()["subject"].toString() == "Too big")
                    bigDraft = m.toMap()["hash"].toString();
            require(!bigDraft.isEmpty() &&
                        session.message(bigDraft)["body"].toString().size() == 270000,
                    "a letter over the network limit is still kept as a draft");
            // --- Pictures: inserted from a file, carried in the letter, shown
            // by the reader. ---
            const auto picturePath = temp.filePath("holiday.png");
            {
                QImage photo(2400, 1600, QImage::Format_RGB32);
                QPainter p(&photo);
                for (int y = 0; y < 1600; y += 40)
                    for (int x = 0; x < 2400; x += 40)
                        p.fillRect(x, y, 40, 40, QColor::fromHsv((x + y) % 360, 180, 220));
                require(photo.save(picturePath), "write a picture to insert");
            }
            QString pictureDraft;
            QTimer::singleShot(30, &window, [&] {
                auto dialog = window.findChild<QDialog *>("composer");
                dialog->findChild<QLineEdit *>("recipientField")->setText(address);
                dialog->findChild<QLineEdit *>("subjectField")->setText("With a picture");
                auto body = dialog->findChild<QTextEdit *>("bodyField");
                QTest::keyClicks(body, "From the beach:");
                auto pick = new QTimer(dialog);
                QObject::connect(pick, &QTimer::timeout, dialog, [pick, picturePath] {
                    if (auto d = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                        pick->stop();
                        chooseFile(d, picturePath);
                    }
                });
                pick->start(10);
                auto addMenu = dialog->findChild<QPushButton *>("addMenuButton");
                require(addMenu && addMenu->menu(), "the composer has a + menu");
                QAction *insertImage = nullptr;
                for (auto action : addMenu->menu()->actions())
                    if (action->objectName() == "insertImageAction")
                        insertImage = action;
                require(insertImage, "the + menu offers Insert image");
                insertImage->trigger();
                bool shown = false;
                for (auto block = body->document()->begin(); block.isValid(); block = block.next())
                    for (auto it = block.begin(); !it.atEnd(); ++it)
                        if (it.fragment().charFormat().isImageFormat())
                            shown = !body->document()
                                         ->resource(QTextDocument::ImageResource,
                                                    QUrl(it.fragment().charFormat().toImageFormat().name()))
                                         .value<QImage>()
                                         .isNull();
                require(shown, "the inserted picture shows in the composer");
                dialog->findChild<QPushButton *>("saveDraftButton")->click();
            });
            window.compose();
            QCoreApplication::processEvents();
            for (auto m : session.messagePage("Drafts", {}, 0, 100))
                if (m.toMap()["subject"].toString() == "With a picture")
                    pictureDraft = m.toMap()["hash"].toString();
            {
                const auto saved = session.message(pictureDraft)["body"].toString();
                require(saved.startsWith("From the beach:\n\n![holiday][img1]") &&
                            saved.contains("\n[img1]: data:image/") &&
                            bm::letterTextBytes("With a picture", saved) <= bm::kMaxLetterText,
                        "the picture is saved by reference, shrunk to fit one letter");
                folders->setCurrentRow(1);
                QCoreApplication::processEvents();
                window.selectMessage(pictureDraft);
                auto reader = window.findChild<QTextBrowser *>("readerBody");
                const auto url = saved.section("[img1]: ", 1).trimmed();
                require(!reader->toPlainText().contains("data:image") &&
                            !reader->document()
                                 ->resource(QTextDocument::ImageResource, QUrl(url))
                                 .value<QImage>()
                                 .isNull(),
                        "the reader shows the picture, not its base64");
                session.moveLetter(pictureDraft, "Trash");
                folders->setCurrentRow(0);
                QCoreApplication::processEvents();
            }
            // A draft that can't be saved doesn't trap its window.
            bool asked = false;
            QTimer::singleShot(30, &window, [&] {
                auto dialog = window.findChild<QDialog *>("composer");
                QTest::keyClicks(dialog->findChild<QTextEdit *>("bodyField"), "more");
                session.moveLetter(bigDraft, "Trash"); // saving it now fails
                QTimer::singleShot(30, &window, [&] {
                    auto box = window.findChild<QMessageBox *>("unsavedDraftBox");
                    asked = box != nullptr;
                    if (box)
                        box->findChild<QPushButton *>("closeWithoutSavingButton")->click();
                });
                dialog->reject();
            });
            window.compose({{"hash", bigDraft}});
            QCoreApplication::processEvents();
            require(asked && !window.findChild<QDialog *>("composer"),
                    "closing a draft that can't be saved asks, and then closes");
            session.moveLetter(bigDraft, "Trash");
        }
        {
            // --- Quoting: reading ">" and PyBitmessage history, replying ---
            const QString dashes(54, '-');
            const auto threaded = session.saveLetter(
                {}, address, address, "Re: bootstrap",
                "go offline\n\n" + dashes + "\nWhat happened?\n\n" + dashes +
                    "\nWe have zero working bootstrap addresses.",
                "direct");
            window.selectMessage(threaded);
            QCoreApplication::processEvents();
            auto reader = window.findChild<QTextBrowser *>("readerBody");
            auto views = window.findChild<QWidget *>("viewSwitch");
            require(views->findChild<QToolButton *>("view_text")->isChecked(),
                    "a letter with quoted history opens in Text view, with quote bars");
            QCoreApplication::processEvents();
            QMap<QString, int> levels;
            for (auto block = reader->document()->begin(); block.isValid(); block = block.next())
                levels[block.text()] = block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel);
            require(levels.value("go offline") == 0 && levels.value("What happened?") == 1 &&
                        levels.value("We have zero working bootstrap addresses.") == 2,
                    "Text view shows PyBitmessage's dash-separated history as quote levels");
            require(!reader->toPlainText().contains(dashes) && !reader->toPlainText().contains('>'),
                    "separators and markers give way to the quote bars");
            {
                // The level-1 bar is painted in its colour beside the quoted line.
                QTextBlock quoted;
                for (auto block = reader->document()->begin(); block.isValid(); block = block.next())
                    if (block.text() == "What happened?")
                        quoted = block;
                const auto rect = reader->document()->documentLayout()->blockBoundingRect(quoted);
                const auto image = reader->viewport()->grab().toImage();
                const int x = int(reader->document()->documentMargin()) + 3;
                const int y = int(rect.center().y()) - reader->verticalScrollBar()->value();
                const auto pixel = image.pixelColor(x * image.devicePixelRatio(),
                                                    y * image.devicePixelRatio());
                // Exactly the bar colour: a hue check alone also matches the dark
                // theme's bluish background.
                const QColor bar("#4a8fd6");
                require(qAbs(pixel.red() - bar.red()) + qAbs(pixel.green() - bar.green()) +
                                qAbs(pixel.blue() - bar.blue()) < 24,
                        "a coloured bar marks the quoted block");
            }
            views->findChild<QToolButton *>("view_plain")->click();
            QCoreApplication::processEvents();
            require(reader->toPlainText().contains(dashes),
                    "Plain view keeps the raw text, separators included");
            // Replying quotes the letter email-style, one level deeper.
            QTimer::singleShot(30, &window, [&] {
                auto dialog = window.findChild<QDialog *>("composer");
                auto body = dialog->findChild<QTextEdit *>("bodyField");
                require(dialog->findChild<QLineEdit *>("subjectField")->text() == "Re: bootstrap",
                        "the subject is not prefixed twice");
                require(body->toPlainText().replace(QChar(0x2028), '\n').startsWith(
                            "\n\n-- sent by ynotbit"),
                        "the reply starts with room to write, then the signature");
                // The answered letter is quoted in the same editor, one level deeper.
                QMap<QString, int> quotedLevels;
                for (auto block = body->document()->begin(); block.isValid(); block = block.next())
                    quotedLevels[block.text()] =
                        block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel);
                bool attributed = false;
                for (auto it = quotedLevels.cbegin(); it != quotedLevels.cend(); ++it)
                    attributed |= it.key().endsWith(" wrote:") && it.value() == 0;
                require(attributed, "an attribution line introduces the quote");
                require(quotedLevels.value("go offline") == 1 &&
                            quotedLevels.value("What happened?") == 2 &&
                            quotedLevels.value("We have zero working bootstrap addresses.") == 3,
                        "the answered letter is quoted one level deeper, history deeper still");
                require(!dialog->findChild<QTextBrowser *>("quoteView"),
                        "there is no separate quote pane");
                // Quoted text can be edited, but its quote cannot be removed.
                QTextBlock quoted;
                for (auto block = body->document()->begin(); block.isValid(); block = block.next())
                    if (block.text() == "go offline")
                        quoted = block;
                auto c = body->textCursor();
                c.setPosition(quoted.position());
                body->setTextCursor(c);
                QTest::keyClick(body, Qt::Key_Backspace);
                require(body->textCursor().block().text() == "go offline" &&
                            body->textCursor().block().blockFormat().intProperty(
                                QTextFormat::BlockQuoteLevel) == 1,
                        "Backspace at the start of a quoted paragraph keeps it quoted");
                c = body->textCursor();
                c.setPosition(quoted.previous().position() + quoted.previous().length() - 1);
                body->setTextCursor(c);
                QTest::keyClick(body, Qt::Key_Delete);
                require(body->textCursor().block().next().text() == "go offline",
                        "Delete before a quoted paragraph does not pull it out of the quote");
                c.setPosition(quoted.position() + 2);
                body->setTextCursor(c);
                QTest::keyClicks(body, "X");
                require(body->textCursor().block().text() == "goX offline" &&
                            body->textCursor().block().blockFormat().intProperty(
                                QTextFormat::BlockQuoteLevel) == 1,
                        "quoted text itself can be edited");
                QTest::keyClick(body, Qt::Key_Backspace);
                // Enter twice at the end of a quote leaves it: an inline answer.
                c.setPosition(quoted.position() + quoted.length() - 1);
                body->setTextCursor(c);
                QTest::keyClick(body, Qt::Key_Return);
                QTest::keyClick(body, Qt::Key_Return);
                require(body->textCursor().block().blockFormat().intProperty(
                            QTextFormat::BlockQuoteLevel) == 0,
                        "Enter on an empty quoted line steps out of the quote");
                QTest::keyClicks(body, "Inline answer.");
                c.movePosition(QTextCursor::Start);
                body->setTextCursor(c);
                QTest::keyClicks(body, "Agreed.");
                dialog->findChild<QPushButton *>("saveDraftButton")->click();
            });
            window.compose(session.message(threaded), true);
            QCoreApplication::processEvents();
            QString replyBody;
            for (auto m : session.messagePage("Drafts", {}, 0, 100))
                if (m.toMap()["preview"].toString().startsWith("Agreed."))
                    replyBody = session.message(m.toMap()["hash"].toString())["body"].toString();
            require(replyBody.contains("-- sent by y*notbit*") && !replyBody.contains("\\--"),
                    "the saved reply keeps a proper \"-- \" signature delimiter");
            require(replyBody.contains("\n> go offline\n\nInline answer.\n>> What happened?") &&
                        replyBody.contains("\n>>> We have zero working bootstrap addresses."),
                    "the saved reply quotes with \">\", nested per level, answers in between");
            session.moveLetter(threaded, "Trash");
            QCoreApplication::processEvents();
        }
        {
            // --- A Markdown letter, answered in plain words: the quoted part
            // keeps its Markdown (headings, bold, lists) on the quote tint. ---
            const auto notes = session.saveLetter(
                {}, address, address, "Release notes",
                "# Release notes\n\n## What changed\n\nThe **address book** is here.\n\n"
                "- contacts page\n- one-click add\n\nThanks!",
                "direct");
            QTimer::singleShot(30, &window, [&] {
                auto dialog = window.findChild<QDialog *>("composer");
                QTest::keyClicks(dialog->findChild<QTextEdit *>("bodyField"), "Thanks, noted.");
                dialog->findChild<QPushButton *>("saveDraftButton")->click();
            });
            window.compose(session.message(notes), true);
            QCoreApplication::processEvents();
            QString replyId;
            for (auto m : session.messagePage("Drafts", {}, 0, 100))
                if (m.toMap()["preview"].toString().startsWith("Thanks, noted."))
                    replyId = m.toMap()["hash"].toString();
            require(!replyId.isEmpty(), "the reply to the Markdown letter was saved");
            const auto replyBody = session.message(replyId)["body"].toString();
            require(replyBody.contains("> # Release notes") && replyBody.contains("> ## What changed") &&
                        replyBody.contains("> The **address book** is here.") &&
                        replyBody.contains("> - contacts page"),
                    "the reply quotes the Markdown source itself, every line behind \">\"");
            window.selectMessage(replyId);
            QCoreApplication::processEvents();
            auto views = window.findChild<QWidget *>("viewSwitch");
            require(views->findChild<QToolButton *>("view_markdown")->isChecked(),
                    "a plain answer quoting a Markdown letter still opens as Markdown");
            auto reader = window.findChild<QTextBrowser *>("readerBody");
            auto blockFor = [&](const QString &text) {
                for (auto block = reader->document()->begin(); block.isValid(); block = block.next())
                    if (block.text() == text)
                        return block;
                return QTextBlock();
            };
            const auto level = [](const QTextBlock &block) {
                return block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel);
            };
            const auto own = blockFor("Thanks, noted.");
            require(own.isValid() && level(own) == 0 && !own.blockFormat().background().style(),
                    "the reply's own words are unquoted, on the plain background");
            const auto h1 = blockFor("Release notes"), h2 = blockFor("What changed");
            require(h1.isValid() && h1.blockFormat().headingLevel() == 1 && level(h1) == 1,
                    "a quoted # heading is still a level-1 heading, inside the quote");
            require(h2.isValid() && h2.blockFormat().headingLevel() == 2 && level(h2) == 1,
                    "a quoted ## heading is still a level-2 heading, inside the quote");
            require(h1.blockFormat().background().color().alpha() > 0,
                    "quoted blocks sit on the quote tint");
            const auto paragraph = blockFor("The address book is here.");
            bool bold = false;
            for (auto it = paragraph.begin(); !it.atEnd(); ++it)
                if (it.fragment().text() == "address book")
                    bold = it.fragment().charFormat().fontWeight() >= QFont::Bold;
            require(paragraph.isValid() && level(paragraph) == 1 && bold,
                    "quoted **bold** still renders bold");
            const auto item = blockFor("contacts page");
            require(item.isValid() && item.textList() && level(item) == 1,
                    "a quoted list is still a list");
            require(blockFor("Thanks!").isValid() && !blockFor("Thanks!").textList() &&
                        level(blockFor("Thanks!")) == 1,
                    "the quoted paragraph after the list stays a paragraph");
            require(blockFor("-- sent by ynotbit").isValid(),
                    "the signature keeps its \"-- \" in the Markdown view");
            // Reopening the draft: one editor again, quoting and headings intact.
            QTimer::singleShot(30, &window, [&] {
                auto dialog = window.findChild<QDialog *>("composer");
                auto editor = dialog->findChild<QTextEdit *>("bodyField");
                QTextBlock heading;
                for (auto block = editor->document()->begin(); block.isValid(); block = block.next())
                    if (block.text() == "Release notes")
                        heading = block;
                require(heading.isValid() && heading.blockFormat().headingLevel() == 1 &&
                            heading.blockFormat().intProperty(QTextFormat::BlockQuoteLevel) == 1,
                        "a reopened reply draft shows the quoted heading in the editor");
                dialog->findChild<QPushButton *>("saveDraftButton")->click();
            });
            window.compose({{"hash", replyId}});
            QCoreApplication::processEvents();
            require(session.message(replyId)["body"].toString() == replyBody,
                    "saving the reopened draft keeps the body byte for byte");
            {
                const auto rect = reader->document()->documentLayout()->blockBoundingRect(h1);
                const auto image = reader->viewport()->grab().toImage();
                const auto pixel = image.pixelColor(
                    int(reader->document()->documentMargin()) + 3,
                    int(rect.center().y()) - reader->verticalScrollBar()->value());
                const QColor bar("#4a8fd6");
                require(qAbs(pixel.red() - bar.red()) + qAbs(pixel.green() - bar.green()) +
                                qAbs(pixel.blue() - bar.blue()) < 24,
                        "the quote bar runs beside the quoted heading");
            }
            session.moveLetter(notes, "Trash");
            session.moveLetter(replyId, "Trash");
            QCoreApplication::processEvents();
        }
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
        require(dPopped->findChild<QTextEdit *>("windowSubject")->toPlainText() == expectedSubject,
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
            require(sigBody->toPlainText().replace(QChar(0x2028), '\n').startsWith("\n\n-- sent by ynotbit"),
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
            {
                // MarkText-style: the paragraph's mark opens "Turn into".
                body->clear();
                body->setPlainText("Plain words\nsecond paragraph");
                auto c = body->textCursor();
                c.movePosition(QTextCursor::Start);
                body->setTextCursor(c);
                QApplication::processEvents();
                const auto first = body->document()->begin();
                const auto rect = body->document()->documentLayout()->blockBoundingRect(first);
                const QPoint mark(gutter->width() / 2,
                                  int(rect.center().y()) + body->viewport()->y() -
                                      body->verticalScrollBar()->value());
                QTest::mouseClick(gutter, Qt::LeftButton, {}, mark);
                auto menu = gutter->findChild<QMenu *>("paragraphMenu");
                require(menu && menu->isVisible(), "clicking the paragraph mark opens its menu");
                auto toParagraph = menu->findChild<QAction *>("turnInto_0");
                require(toParagraph && toParagraph->isChecked() &&
                            !menu->findChild<QAction *>("turnInto_1")->isChecked(),
                        "the menu marks the paragraph's current type");
                menu->findChild<QAction *>("turnInto_1")->trigger();
                menu->close();
                require(body->document()->begin().blockFormat().headingLevel() == 1,
                        "\"Turn into\" Heading 1 makes the paragraph a heading");
                require(bm::letterMarkdown(body->document()).startsWith("# Plain words"),
                        "the heading is saved as \"# \"");
                c.movePosition(QTextCursor::End);
                body->setTextCursor(c);
                dialog->activateWindow();
                body->setFocus();
                QApplication::processEvents();
                QTest::keyClick(body, Qt::Key_2, Qt::ControlModifier | Qt::ShiftModifier);
                require(body->textCursor().block().blockFormat().headingLevel() == 2,
                        "Ctrl+Shift+2 turns the paragraph into Heading 2, as in MarkText");
                QTest::keyClick(body, Qt::Key_0, Qt::ControlModifier | Qt::ShiftModifier);
                require(body->textCursor().block().blockFormat().headingLevel() == 0 &&
                            body->textCursor().charFormat().fontWeight() < QFont::Bold,
                        "Ctrl+Shift+0 turns it back into a plain paragraph");
                QTest::keyClick(body, Qt::Key_H, Qt::ControlModifier);
                require(body->textCursor().currentList(), "Ctrl+H makes a bullet list item");
                // A quoted paragraph stays quoted whatever its type becomes.
                bm::setQuoteLevel(body->document()->begin(), 1);
                c.movePosition(QTextCursor::Start);
                body->setTextCursor(c);
                QTest::keyClick(body, Qt::Key_3, Qt::ControlModifier | Qt::ShiftModifier);
                require(body->document()->begin().blockFormat().headingLevel() == 3 &&
                            bm::blockQuoteLevel(body->document()->begin()) == 1,
                        "turning a quoted paragraph into a heading keeps its quote");
                require(bm::letterMarkdown(body->document()).startsWith("> ### Plain words"),
                        "a quoted heading is saved as \"> ### \"");
            }
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
            auto modeHint = dialog->findChild<QLabel *>("modeHint");
            require(modePrivate->parentWidget() == modePublic->parentWidget() &&
                        modePrivate->parentWidget()->objectName() == "modeSwitch",
                    "private and public are one segmented switch");
            require(qAbs(modeHint->geometry().center().y() -
                         modePrivate->mapTo(dialog, modePrivate->rect().center()).y() +
                         modeHint->parentWidget()->mapTo(dialog, QPoint()).y()) < 20 &&
                        modeHint->mapTo(dialog, QPoint()).x() >
                            modePublic->mapTo(dialog, QPoint(modePublic->width(), 0)).x(),
                    "the mode's explanation sits to the right of the switch");
            auto toLabel = dialog->findChild<QLabel *>("recipientLabel");
            require(toLabel && toLabel->buddy() == to && toLabel->isVisible(),
                    "the recipient field is labelled To:");
            require(!dialog->findChild<QToolBar *>(), "no static formatting toolbar");
            require(dialog->findChild<QPushButton *>("sendButton")->isDefault(),
                    "Send is the default button");
            auto stripe = dialog->findChild<QWidget *>("composerKindStripe");
            require(stripe && stripe->property("letterKind").toInt() == 0,
                    "private mail gets the personal letter border");
            modePublic->click();
            require(stripe->property("letterKind").toInt() == 3,
                    "public mail gets the broadcast border");
            require(!to->isVisible() && !toLabel->isVisible(),
                    "recipient and its label hidden in public mode");
            modePrivate->click();
            require(to->isVisible(), "recipient visible again after switching back");
            for (const auto &name : {"floatBoldButton", "floatItalicButton", "floatStrikeButton",
                                     "floatCodeButton", "floatLinkButton", "floatClearButton"})
                require(dialog->findChild<QToolButton *>(name),
                        QString("the floating toolbar has %1").arg(name).toUtf8().constData());
            {
                auto body = dialog->findChild<QTextEdit *>("bodyField");
                body->setFocus();
                body->setPlainText("make bold");
                body->selectAll();
                QTest::keyClick(body, Qt::Key_B, Qt::ControlModifier);
                require(body->textCursor().charFormat().fontWeight() == QFont::Bold,
                        "Ctrl+B still bolds without the toolbar");
            }
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
            require(!window.findChild<QTextEdit *>("subject")->isVisible(),
                    "the reader's subject does not float above the identities pane");
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

            // Menu items change identities behind the open Identities page; the
            // page must redraw itself, not wait for the user to navigate away and back.
            auto menuItem = [&](const QString &text) -> QAction * {
                for (auto a : window.findChildren<QAction *>())
                    if (a->text() == text)
                        return a;
                throw std::runtime_error(("no menu item " + text).toStdString());
            };
            // Runs fn on the modal dialog once it is up -- a fixed delay can fire
            // before it opens on a busy machine.
            auto onDialog = [&](auto *type, auto fn) {
                using Dialog = std::remove_pointer_t<decltype(type)>;
                auto poll = new QTimer(&window);
                QObject::connect(poll, &QTimer::timeout, &window, [poll, fn]() mutable {
                    if (auto d = qobject_cast<Dialog *>(QApplication::activeModalWidget())) {
                        poll->stop();
                        poll->deleteLater();
                        fn(d);
                    }
                });
                poll->start(10);
            };
            auto cards = [&] { return window.findChildren<QFrame *>("identityCard").size(); };
            auto cardFor = [&](const QString &addr) {
                for (auto l : window.findChildren<QLabel *>("identityAddress"))
                    if (l->text() == addr)
                        return true;
                return false;
            };
            auto shownCards = cards();
            menuItem("Create identity…")->trigger(); // the responder answers the label prompt
            QTest::qWait(50);
            require(cards() == shownCards + 1,
                    "Identity > Create identity… shows the new card on the open Identities page");
            const QString imported = "BM-2cSsZnHbLbJr5A2RCrCMQo7vwGBi7CKNfz";
            {
                QFile keys(temp.filePath("import-keys.dat"));
                require(keys.open(QIODevice::WriteOnly), "write a keys.dat to import");
                keys.write("[BM-2cSsZnHbLbJr5A2RCrCMQo7vwGBi7CKNfz]\nlabel = Imported key\n"
                           "enabled = true\ndecoy = false\n"
                           "privsigningkey = 5JCLH7eb8Hd3CNMLfP8sBTE8AYLUeLbb6K7dqJdrp5bgzYpnAvX\n"
                           "privencryptionkey = 5JXWzkqmfv6MxZ8Uq18byihHYSQPCN5GV6j9GsC2ZD1BSmRjQn6\n");
            }
            bool picked = false;
            onDialog((QFileDialog *)nullptr, [&](QFileDialog *d) {
                chooseFile(d, temp.filePath("import-keys.dat"));
                picked = true;
            });
            menuItem("Import keys.dat…")->trigger();
            QTest::qWait(50);
            require(picked, "Import keys.dat… asks for the file");
            require(cardFor(imported),
                    "Identity > Import keys.dat… shows the imported key on the open Identities page");
            // A chan takes two prompts (phrase, then the expected address), so the
            // password responder steps aside.
            responder.stop();
            // Answers the menu item's prompts in turn.
            std::function<void(QStringList)> answer = [&](QStringList replies) {
                onDialog((QInputDialog *)nullptr, [&, replies](QInputDialog *d) mutable {
                    d->setTextValue(replies.takeFirst());
                    if (!replies.isEmpty())
                        answer(replies);
                    d->accept();
                });
            };
            answer({"widgets menu chan", ""});
            shownCards = cards();
            menuItem("Join or create chan…")->trigger();
            QTest::qWait(50);
            responder.start(10);
            require(session.error().isEmpty() && cards() == shownCards + 1,
                    "Identity > Join or create chan… shows the chan on the open Identities page");

            // A subscription's label names its letters: subscribing or unsubscribing
            // from the menu redraws the open letter and the list.
            const QString publisher = "BM-2cWFkyuXXFw6d393RGnin2RpSXj8wxtt6F";
            {
                vault.unlock(vaultFile, "test password");
                bm::Mailbox injected;
                injected.open(temp.filePath("mailbox"), vault.mailboxKey(key));
                injected.store("news-1", publisher, publisher, "Weekly news", "Broadcast body",
                               1800, "Inbox");
                injected.close();
                vault.lock();
            }
            window.findChild<QToolButton *>("folderIcon_Inbox")->click();
            session.messageModel()->reload();
            window.selectMessage("news-1");
            require(window.findChild<QLabel *>("toLabel")->isVisible() &&
                        window.findChild<QLabel *>("toAddress")->text() == publisher,
                    "an ordinary letter keeps its To line");
            auto fromNameLabel = window.findChild<QLabel *>("fromName");
            auto listName = [&] {
                const auto row = session.messageModel()->rowForHash("news-1");
                return list->model()->index(row, 0).data(Qt::UserRole + 12).toString();
            };
            require(!fromNameLabel->isVisible() && listName().isEmpty(),
                    "a letter from an unknown address shows no name");
            responder.stop();
            answer({publisher, "Release news"});
            menuItem("Subscribe to broadcasts…")->trigger();
            QTest::qWait(50);
            require(session.error().isEmpty() && fromNameLabel->isVisible() &&
                        fromNameLabel->text() == "Release news",
                    "Identity > Subscribe to broadcasts… names the open letter's sender");
            require(listName() == "Release news", "...and its row in the list");
            answer({"Release news · " + publisher});
            menuItem("Manage subscriptions…")->trigger();
            QTest::qWait(50);
            require(!fromNameLabel->isVisible() && listName().isEmpty(),
                    "unsubscribing from the menu takes the name away again");

            // The Subscriptions page: the rail of senders, and the selected
            // sender's posts as a feed.
            const QString other = "BM-2cX8TF9vuQZEWvT7UrEeq1HN9dgiSUPLEN";
            const QString releases = "BM-2cUzX8f9CKUU7L8NeB8GExZvf54PrcXq1S";
            {
                vault.unlock(vaultFile, "test password");
                bm::Mailbox injected;
                injected.open(temp.filePath("mailbox"), vault.mailboxKey(key));
                injected.store("bc-1", publisher, publisher, "Weekly issue 1", "News", 1801,
                               "Broadcasts");
                injected.store("bc-2", other, other, "Other sender's post", "Hello", 1802,
                               "Broadcasts");
                for (int i = 0; i < 45; ++i) // more than two pages of release notes
                    injected.store("rel-" + QString::number(i), releases, releases,
                                   "ynotbit 0." + QString::number(i), "Notes for " + QString::number(i),
                                   1900 + i, "Broadcasts");
                injected.close();
                vault.lock();
            }
            bm::updates::setPublisherAddressForTesting(releases);
            session.messageModel()->reload();
            folders->setCurrentRow(5);
            QTest::qWait(30);
            auto rail = window.findChild<QWidget *>("channelRail");
            auto addSource = window.findChild<QPushButton *>("joinOrCreateChannelButton");
            require(rail->isVisible() &&
                        window.findChild<QLabel *>("channelRailHeading")->text() == "SUBSCRIPTIONS" &&
                        addSource->text() == "+ Subscribe…",
                    "the Broadcasts page shows a subscriptions rail like the Channels page");
            require(window.findChild<QToolButton *>("folderIcon_Broadcasts")->toolTip() ==
                        "Subscriptions",
                    "the page is named Subscriptions (its letters stay in the Broadcasts folder)");
            auto chipNamed = [&](const QString &text) -> QPushButton * {
                for (auto chip : window.findChildren<QPushButton *>("channelChip"))
                    // Collapsed, a chip's name is only in its tooltip.
                    if (chip->text() == text ||
                        chip->toolTip().contains(">" + text.toHtmlEscaped() + "<"))
                        return chip;
                return nullptr;
            };
            require(chipNamed("ynotbit updates") && chipNamed(other),
                    "the rail lists the built-in release notices and every sender with letters");
            require(chipNamed(other)->font().bold(), "a sender with unread letters is bold");
            answer({publisher, "Weekly"});
            addSource->click();
            QTest::qWait(50);
            require(session.error().isEmpty() && chipNamed("Weekly"),
                    "+ Subscribe… adds the subscription to the rail");
            auto feed = window.findChild<QWidget *>("feedView");
            auto feedScroll = window.findChild<QScrollArea *>("feedScroll");
            auto feedHashes = [&] {
                QStringList hashes;
                auto layout = feedScroll->widget()->layout();
                for (int i = 0; i < layout->count(); ++i)
                    if (auto w = layout->itemAt(i)->widget(); w && w->objectName() == "feedCard")
                        hashes << w->property("hash").toString();
                return hashes;
            };
            auto feedCard = [&](const QString &hash) -> QWidget * {
                for (auto card : feedScroll->findChildren<QWidget *>("feedCard"))
                    if (card->property("hash").toString() == hash)
                        return card;
                return nullptr;
            };
            chipNamed("Weekly")->click();
            QTest::qWait(30);
            require(feed->isVisible() && !window.findChild<QWidget *>("listColumn")->isVisible() &&
                        !window.findChild<QTextEdit *>("subject")->isVisible() &&
                        !window.findChild<QLabel *>("messageAddresses")->isVisible() &&
                        !window.findChild<QLabel *>("toLabel")->isVisible(),
                    "Subscriptions shows a feed: no letter list, no reader, no From/To");
            require(feedHashes() == QStringList{"bc-1"} &&
                        window.findChild<QLabel *>("feedHeaderName")->text() == "Weekly",
                    "a subscription's chip shows only that sender's posts, under their name");
            {
                auto card = feedCard("bc-1");
                require(card->findChild<QLabel *>("feedSubject")->text() == "Weekly issue 1" &&
                            card->findChild<QTextBrowser *>("feedBody")->toPlainText().contains("News"),
                        "a card shows the post's subject and full text");
                auto body = card->findChild<QTextBrowser *>("feedBody");
                require(body->height() >= int(body->document()->size().height()),
                        "a card's text is never cut: the card grows with it");
                // Shorter than the action buttons beside it, the text must still
                // start at the top, not centred in the height the buttons take.
                auto actions = card->findChild<QWidget *>("feedActions");
                require(body->height() < actions->height(), "test sanity: a short post");
                require(qAbs(body->mapTo(card, QPoint()).y() - actions->mapTo(card, QPoint()).y()) <= 2,
                        "a short post's text starts at the top of its card, beside the actions");
                // The buttons flow into columns beside a short post, so the card is
                // no taller than its text needs; a long post keeps one column.
                QList<QToolButton *> buttons;
                for (auto name : {"feedReply", "feedForward", "feedCopy", "feedOpen", "feedArchive",
                                  "feedTrash"})
                    buttons << card->findChild<QToolButton *>(name);
                const auto columns = [&] {
                    QSet<int> xs;
                    for (auto b : buttons)
                        xs << b->x();
                    return xs.size();
                };
                require(columns() > 1, "beside a short post the buttons flow into columns");
                require(actions->height() < 3 * buttons.first()->height(),
                        "...so the card is shorter than a column of buttons");
                auto ordered = buttons;
                std::sort(ordered.begin(), ordered.end(), [](QToolButton *a, QToolButton *b) {
                    return a->x() != b->x() ? a->x() < b->x() : a->y() < b->y();
                });
                require(ordered == buttons, "the buttons keep their order, column by column");
                body->setPlainText(QString("A long post.\n").repeated(20));
                QTest::qWait(30);
                require(columns() == 1, "beside a long post the buttons stay in one column");
            }
            chipNamed("ynotbit updates")->click();
            QTest::qWait(30);
            require(feedHashes().size() == bm::FeedView::kPageSize &&
                        feedHashes().first() == "rel-44" && feedHashes()[1] == "rel-43",
                    "the feed opens with one page of posts, newest on top");
            for (int round = 0; round < 3 && feedHashes().size() < 45; ++round) {
                feedScroll->verticalScrollBar()->setValue(feedScroll->verticalScrollBar()->maximum());
                QTest::qWait(30);
            }
            require(feedHashes().size() == 45 && feedHashes().last() == "rel-0",
                    "scrolling to the bottom loads the older posts, page by page");
            {
                const QDateTime now(QDate(2026, 10, 3), QTime(12, 0));
                require(bm::relativeTime(now.addSecs(-20), now) == "just now" &&
                            bm::relativeTime(now.addSecs(-5 * 60), now) == "5 min" &&
                            bm::relativeTime(now.addSecs(-3 * 3600), now) == "3 h" &&
                            bm::relativeTime(now.addDays(-1).addSecs(-3600), now) == "Yesterday" &&
                            bm::relativeTime(now.addDays(-9), now) ==
                                QLocale().toString(now.addDays(-9).date(), QLocale::ShortFormat),
                        "post times read like a feed's: just now, 5 min, 3 h, Yesterday, a date");
            }
            feedScroll->verticalScrollBar()->setValue(0);
            QTest::qWait(30);
            feedCard("rel-44")->findChild<QToolButton *>("feedArchive")->click();
            QTest::qWait(30);
            require(!feedHashes().contains("rel-44") &&
                        session.message("rel-44")["folder"].toString() == "Archive",
                    "Archive on a card files the post and takes the card away");
            chipNamed(other)->click();
            QTest::qWait(30);
            require(feedCard("bc-2")->property("unread").toBool() &&
                        !feedCard("bc-2")->findChild<QLabel *>("feedUnreadDot")->isHidden(),
                    "a new post starts unread, with a dot by its time");
            require(!feedCard("bc-2")->styleSheet().contains("border-left"),
                    "a card's border is even all round (unread is the dot, not the edge)");
            QTest::qWait(bm::FeedView::kReadAfterMs + 600);
            require(!session.message("bc-2")["unread"].toBool() &&
                        !feedCard("bc-2")->property("unread").toBool(),
                    "a post on screen for a second turns read");
            require(feedCard("bc-2")->findChild<QLabel *>("feedUnreadDot")->isHidden(),
                    "...and its dot goes away");
            QTest::qWait(30);
            require(!chipNamed(other)->font().bold(), "...and its sender's bold in the rail clears");
            // Reply privately: a personal letter to the sender, quoting the post.
            bool replied = false;
            onDialog((QDialog *)nullptr, [&](QDialog *d) {
                if (d->objectName() != "composer")
                    return;
                replied = d->findChild<QLineEdit *>("recipientField")->text() == other &&
                          d->findChild<QPushButton *>("modePrivateButton")->isChecked();
                d->reject();
            });
            feedCard("bc-2")->findChild<QToolButton *>("feedReply")->click();
            QTest::qWait(50);
            if (auto box = window.findChild<QMessageBox *>("unsavedDraftBox"))
                box->findChild<QPushButton *>("closeWithoutSavingButton")->click();
            require(replied, "Reply privately writes a personal letter to the sender");
            require(window.findChild<QPushButton *>("writeButton")->toolTip() == "Write a broadcast",
                    "Write on the Subscriptions page writes a broadcast");
            // Right-click a subscription: Unsubscribe. Its letters stay, under the address.
            bool unsubscribed = false;
            auto popup = new QTimer(&window);
            QObject::connect(popup, &QTimer::timeout, &window, [&, popup] {
                auto menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu)
                    return;
                popup->stop();
                popup->deleteLater();
                for (auto a : menu->actions())
                    if (a->objectName() == "unsubscribeSource") {
                        menu->setActiveAction(a);
                        QTest::keyClick(menu, Qt::Key_Return);
                        unsubscribed = true;
                    }
                if (!unsubscribed)
                    menu->close();
            });
            popup->start(10);
            auto weekly = chipNamed("Weekly");
            emit weekly->customContextMenuRequested(QPoint(5, 5));
            QTest::qWait(50);
            require(unsubscribed && session.subscriptions().isEmpty(),
                    "right-click > Unsubscribe on a chip removes the subscription");
            require(!chipNamed("Weekly") && chipNamed(publisher),
                    "its broadcasts stay, listed under the bare address");
            // Built-in subscriptions: added once per mailbox, so unsubscribing sticks.
            bm::updates::setDefaultSubscriptionsForTesting({{other, "Bitmessage digest"}});
            session.seedSubscriptions();
            {
                const auto subs = session.subscriptions();
                require(subs.size() == 1 && subs[0].toMap()["address"] == other &&
                            subs[0].toMap()["label"] == "Bitmessage digest",
                        "a mailbox starts subscribed to the Bitmessage digest");
            }
            session.unsubscribe(other);
            session.seedSubscriptions();
            require(session.subscriptions().isEmpty(),
                    "after unsubscribing, the digest is not added back");
            bm::updates::setDefaultSubscriptionsForTesting({});
            bm::updates::setPublisherAddressForTesting({});
            responder.start(10);
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
        {
            // A short window must not squeeze the chosen vault's box: its name and
            // path stay whole (macOS windows open shorter than this test's).
            // Windows lets a window get shorter than the lock page needs; here
            // the window won't, so the page's own area is capped instead.
            auto welcome = window.findChild<QWidget *>("welcomeStack");
            welcome->setMaximumHeight(260);
            // A subtitle that wraps onto more lines than Qt's minimum counts on
            // (Windows' font does this to the real one): it must not take the
            // vault box's room.
            auto subtitle = window.findChild<QLabel *>("lockedSubtitle");
            const auto subtitleText = subtitle->text();
            subtitle->setText(subtitleText + " " + subtitleText + " " + subtitleText);
            QTest::qWait(30);
            auto box = window.findChild<QWidget *>("vaultBox");
            auto name = window.findChild<QLabel *>("lockedVaultName");
            auto path = window.findChild<QLabel *>("lockedVaultPath");
            require(box->isVisible() && !name->text().isEmpty(), "the locked screen names the vault");
            const auto inside = [&](QWidget *label) {
                const QRect r(label->mapTo(box, QPoint(0, 0)), label->size());
                return box->contentsRect().contains(r) &&
                       label->height() >= label->heightForWidth(label->width()) &&
                       label->height() >= label->fontMetrics().height();
            };
            if (!inside(name) || !inside(path)) {
                // Measured, so a failure on another platform says which part.
                const auto describe = [&](const char *what, QWidget *w) {
                    auto label = static_cast<QLabel *>(w);
                    std::cerr << what << ": at " << w->mapTo(box, QPoint(0, 0)).y() << " height "
                              << w->height() << ", font " << label->fontMetrics().height()
                              << ", needs " << label->heightForWidth(w->width()) << "/"
                              << label->sizeHint().height() << "\n";
                };
                std::cerr << "vault box: height " << box->height() << ", inside "
                          << box->contentsRect().top() << ".." << box->contentsRect().bottom()
                          << ", hint " << box->sizeHint().height() << "\n";
                describe("name", name);
                describe("path", path);
                // What gave it too little room: each widget up to the window,
                // its height against what it asks for.
                std::cerr << "style " << QApplication::style()->name().toStdString()
                          << ", box margins " << box->contentsMargins().top() << "/"
                          << box->layout()->contentsMargins().top() << ", min "
                          << box->minimumSizeHint().height() << ", policy "
                          << int(box->sizePolicy().verticalPolicy()) << "\n";
                for (QWidget *w = box; w; w = w->parentWidget())
                    std::cerr << "  " << w->metaObject()->className() << " '"
                              << w->objectName().toStdString() << "': height " << w->height()
                              << ", min " << w->minimumSizeHint().height() << "/"
                              << w->minimumHeight() << ", hint " << w->sizeHint().height()
                              << ", max " << w->maximumHeight() << ", layout min "
                              << (w->layout() ? w->layout()->minimumSize().height() : -1)
                              << ", visible " << w->isVisible() << "\n";
            }
            require(inside(name) && inside(path),
                    "a lock page shorter than its content still shows the vault's name and path whole");
            require(path->toolTip() == vaultFile && path->text().contains(QChar(0x2026)) &&
                        path->fontMetrics().horizontalAdvance(path->text()) <= path->width(),
                    "a long vault path is one line, shortened in the middle, whole in the tooltip");
            // The box's spacing is its layout's: stylesheet padding shrinks the
            // inside without growing the box on macOS, and clips both lines.
            require(!box->styleSheet().contains("padding"),
                    "the vault box spaces its text with layout margins, not padding");
            subtitle->setText(subtitleText);
            welcome->setMaximumHeight(QWIDGETSIZE_MAX);
            QTest::qWait(30);
        }
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
            auto subjectLabel = window.findChild<QTextEdit *>("subject");
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
            // Every opening shows Subscriptions with the digest selected.
            const QString digest = "BM-2cX8TF9vuQZEWvT7UrEeq1HN9dgiSUPLEN";
            bm::updates::setDefaultSubscriptionsForTesting({{digest, "Bitmessage digest"}});
            session.openMailboxAt(mailboxPath);
            QCoreApplication::processEvents();
            QTest::qWait(30);
            require(folders->currentRow() == 5 &&
                        window.findChild<QLabel *>("feedHeaderAddress")->text() == digest,
                    "ynotbit opens on Subscriptions with the digest selected");
            bm::updates::setDefaultSubscriptionsForTesting({});
            folders->setCurrentRow(0);
            QCoreApplication::processEvents();
            require(subjectLabel->isVisible(),
                    "full-mailbox state shows the reader's subject label again");
            if (capturingStates)
                window.grab().save(dir + "/state3-full.png");
            folders->setCurrentRow(4);
            QTest::qWait(50);
            bool rechecked = false;
            for (auto chip : window.findChildren<QPushButton *>("channelChip"))
                if (chip->toolTip().endsWith(chipAddress)) {
                    require(chip->icon().pixmap(18, 18).toImage() == chipIdenticon,
                            "an address keeps the same identicon across a lock, unlock and "
                            "mailbox reopen -- nothing per-session salts it");
                    rechecked = true;
                }
            require(rechecked, "found the same channel chip again after reopening the mailbox");
        }
        {
            // A chan full of big spam letters, like [chan] general: typing in the
            // filter must not freeze the window while every body is read.
            vault.unlock(vaultFile, "test password");
            {
                bm::Mailbox injected;
                injected.open(session.mailPath(), vault.mailboxKey(key));
                bm::Mailbox::Batch batch(injected);
                QRandomGenerator noise(7);
                for (int i = 0; i < 300; ++i) {
                    QString body(150 * 1024, Qt::Uninitialized);
                    for (auto &c : body)
                        c = QChar(' ' + noise.bounded(94));
                    if (i == 150)
                        body.insert(body.size() / 2, " Cool amber wine in cups of gold ");
                    injected.store("heavy-" + QString::number(i), "spammer", "heavy-chan",
                                   "Spam " + QString::number(i), body, 1900 + i, "Channels");
                }
                batch.commit();
                injected.close();
            }
            vault.lock();
            session.messageModel()->reload();
            folders->setCurrentRow(0);
            folders->setCurrentRow(4);
            QTest::qWait(50);
            selectChannel("heavy-chan");
            require(list->model()->rowCount() == 300, "the heavy chan lists every letter");
            QTest::qWait(200); // the chan's first paint is not the filter's to answer for
            auto searchBox = window.findChild<QLineEdit *>("messageSearch");
            auto progress = window.findChild<QProgressBar *>("searchProgress");
            auto countLabel = window.findChild<QLabel *>("listCountLabel");
            QElapsedTimer stall;
            stall.start();
            qint64 last = 0, worst = 0;
            bool sawProgress = false, sawSearching = false;
            QTimer meter;
            QObject::connect(&meter, &QTimer::timeout, [&] {
                const auto now = stall.elapsed();
                worst = std::max(worst, now - last);
                last = now;
                sawProgress |= progress->isVisible();
                sawSearching |= countLabel->text().contains("searching");
            });
            meter.start(5);
            last = stall.elapsed();
            QTest::keyClicks(searchBox, "cool am", Qt::NoModifier, 40);
            // The box applies its text 250 ms after the last key, later on a
            // busy machine: wait for the text to arrive, then for the search.
            const bool finished = QTest::qWaitFor(
                [&] {
                    return session.messageModel()->search() == "cool am" &&
                           !session.messageModel()->searching();
                },
                20000);
            QTest::qWait(20);
            meter.stop();
            std::cout << "Filter over 300 x 150 KB letters: worst UI stall " << worst << "ms\n";
            require(finished, "the search finishes");
            require(worst < 250, "typing in the filter never freezes the window");
            require(sawProgress && sawSearching, "a running search shows its progress");
            require(list->model()->rowCount() == 1 &&
                        list->model()->index(0, 0).data(Qt::UserRole + 1) == "heavy-150",
                    "the filter finds the one letter whose body matches");
            require(!progress->isVisible() && countLabel->text() == "1 found",
                    "a finished search hides its progress and says what it found");
            searchBox->clear();
            require(QTest::qWaitFor(
                        [&] {
                            return list->model()->rowCount() == 300 &&
                                   countLabel->text() == "300 total";
                        },
                        5000),
                    "clearing the filter lists every letter again");
        }
        require(session.nodeArguments().join(' ').contains("-R 2048 -A 90"),
                "the node gets the retention limits, 2048 MiB and 90 days by default");
        {
            QDir().mkpath(temp.filePath("retention-node"));
            QSettings config(temp.filePath("retention-node/desktop.ini"), QSettings::IniFormat);
            config.setValue("retentionMB", 1024);
            config.setValue("retentionDays", 30);
        }
        bm::Session configured(temp.filePath("retention-node"), true);
        require(configured.nodeArguments().join(' ').contains("-R 1024 -A 30"),
                "saved retention settings reach the node's arguments");
        {
            // The settings window's node settings: validated, saved, and one
            // node restart for a burst of changes.
            bm::Session s(temp.filePath("settings-node"), true);
            require(!s.setProxy("not an address") && s.proxy().isEmpty(),
                    "an invalid proxy is refused and not saved");
            require(s.setProxy(" 127.0.0.1:9050 ") && s.proxy() == "127.0.0.1:9050",
                    "a valid proxy is saved, trimmed");
            require(s.setPeer("[::1]:8444") && s.peer() == "[::1]:8444", "a valid peer is saved");
            s.setRetention(1024, 30);
            require(s.retentionMB() == 1024 && s.retentionDays() == 30 &&
                        s.nodeArguments().join(' ').contains("-R 1024 -A 30"),
                    "retention set through the session reaches the node arguments");
            const int before = s.nodeRestarts();
            QTest::qWait(1300);
            require(s.nodeRestarts() == before + 1,
                    "a burst of node setting changes restarts the node once");
            s.setRetention(1024, 30);
            QTest::qWait(1300);
            require(s.nodeRestarts() == before + 1, "an unchanged retention does not restart");
            s.setGpuEnabled(false);
            require(!s.gpuEnabled() && !bm::ProofOfWork::gpuEnabled() &&
                        !QSettings().value("gpu", true).toBool(),
                    "the GPU switch is saved and applied");
            s.setGpuEnabled(true);
        }
        {
            auto open = window.findChild<QAction *>("settingsAction");
            require(open, "File has a Settings action");
            open->trigger();
            auto settings = window.findChild<bm::SettingsWindow *>();
            require(settings && settings->isVisible(), "Settings opens a window");
            open->trigger();
            require(window.findChildren<bm::SettingsWindow *>().size() == 1,
                    "opening Settings again raises the same window");
            auto proxy = settings->findChild<QLineEdit *>("proxyEdit");
            auto proxyError = settings->findChild<QLabel *>("proxyError");
            proxy->clear();
            QTest::keyClicks(proxy, "nonsense");
            require(!proxyError->isHidden(), "an invalid proxy shows an error under the field");
            emit proxy->editingFinished();
            require(session.proxy().isEmpty(), "an invalid proxy is not saved");
            proxy->clear();
            QTest::keyClicks(proxy, "127.0.0.1:9050");
            require(proxyError->isHidden(), "a valid proxy clears the error");
            emit proxy->editingFinished();
            require(session.proxy() == "127.0.0.1:9050", "a valid proxy is saved when editing ends");
            session.setProxy("");
            auto mb = settings->findChild<QSpinBox *>("retentionMBSpin");
            mb->setValue(512);
            emit mb->editingFinished();
            require(session.retentionMB() == 512, "the Storage page saves the retention size");
            session.setRetention(2048, 90);
            require(settings->findChild<QLabel *>("storageUsage")->text().contains("objects"),
                    "the Storage page shows the current usage");
            settings->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            require(!window.findChild<bm::SettingsWindow *>(), "closing Settings deletes it");
        }
        {
            // The node's object count and size come from its status.json, and
            // the last values stay shown while it isn't running.
            QFile status(temp.filePath("node/status.json"));
            require(status.open(QIODevice::WriteOnly), "write a node status");
            status.write(R"({"objects":42,"object_bytes":1048576,"time":0})");
            status.close();
            QTest::qWait(900);
            require(session.objectCount() == 42 && session.cacheBytes() == 1048576,
                    "the object count and size come from the node's status.json");
            require(!session.activity().contains("no longer be recoverable"),
                    "no lost-letters warning while nothing unread was pruned");
            // Retention deleted objects this mailbox never read: say so.
            auto db = ntb_object_db_open(QFile::encodeName(temp.filePath("node")).constData());
            require(db, "open the node's object store");
            for (int n = 0; n < 3; ++n) {
                QByteArray object(64, 'j');
                object[0] = char(n);
                const auto hash = QByteArray::fromHex(bm::Protocol::inventoryHash(object).toLatin1());
                ntb_object_db_save(db, reinterpret_cast<const uint8_t *>(hash.constData()),
                                   reinterpret_cast<const uint8_t *>(object.constData()),
                                   size_t(object.size()), QDateTime::currentSecsSinceEpoch());
                if (n == 1)
                    ntb_object_db_prune(db, 0, 0); // the first two go before the mailbox read them
            }
            ntb_object_db_close(db);
            QTest::qWait(1600);
            require(session.activity().contains("no longer be recoverable"),
                    "objects pruned before the mailbox read them raise the lost-letters warning");
        }
        session.lock();
        std::cout << "PASS Widgets mailbox selection, rendering and lock\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
