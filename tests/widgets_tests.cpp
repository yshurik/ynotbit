#include "address_label.h"
#include "ellipsis_label.h"
#include "app_badge.h"
#include "desktop_window.h"
#include "chats_pane.h"
#include "coolname.h"
#include "session.h"
#include "ntb-object-db.h"
extern "C" {
#include "ntb-address.h"
}
#include "updates.h"
#include "feed_view.h"
#include "find_people_dialog.h"
#include "protocol.h"
#include "pybm_import_dialog.h"
#include "rail_badge.h"
#include "letter_document.h"
#include "pow.h"
#include "i18n.h"
#include "key_search.h"
#include "new_chat_dialog.h"
#include "join_channel_dialog.h"
#include "nick_client.h"
#include "nickbots_page.h"
#include "nickname_dialog.h"
#include "new_vault_dialog.h"
#include "sound_theme.h"
#include "tray_icon.h"
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QSignalSpy>
#include <QSoundEffect>
#include <QTest>
#include <QtWidgets>
#include "bmclient/pybm_fixtures.h"
#include <bmc/chat_format.h>
#include <bmc/wire.h>
#include <bmc/clock.h>
#include <filesystem>
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
// Stands in for the system when a link is opened: keeps the URL, launches nothing.
class UrlRecorder : public QObject {
    Q_OBJECT
  public:
    QStringList opened;
  public slots:
    void record(const QUrl &url) {
        opened << url.toString();
    }
};
// Whether a default button shows all of its text: bold by its own font, and as
// wide as that text with the window's style sheet's 9 px padding a side.
// compose() no longer blocks, so a test that scripts the composer from a timer waits for
// the window to go before looking at what it did.
static void waitComposerGone(bm::DesktopWindow &window) {
    while (window.findChild<QDialog *>("composer"))
        QTest::qWait(10);
}
static void composeAndWait(bm::DesktopWindow &window, QVariantMap letter = {}, bool reply = false,
                           bool forward = false) {
    window.compose(std::move(letter), reply, forward);
    waitComposerGone(window);
}
static bool boldAndWhole(QPushButton *button) {
    return button->font().weight() >= QFont::DemiBold &&
           QFontMetrics(button->font()).horizontalAdvance(button->text()) + 18 <= button->width();
}
static void require(bool ok, const char *message) {
    if (!ok) {
        // Also printed here: a failure inside a dialog callback is thrown
        // through Qt's event loop, which aborts before main() reports it.
        std::cerr << "FAIL: " << message << "\n";
        throw std::runtime_error(message);
    }
}
// The File menu: the vault's items, the mailbox's, then the copy and the app's own. What has
// nothing to work on is grey, and the two recent lists are submenus that follow the session's.
static void testFileMenu(const QTemporaryDir &temp) {
    const auto vaultOne = temp.filePath("menu-one.bmvault");
    const auto vaultTwo = temp.filePath("menu-two.bmvault");
    const auto boxOne = temp.filePath("menu-one.bmmail");
    const auto boxTwo = temp.filePath("menu-two.bmmail");
    {
        bm::Vault made;
        made.create(vaultOne, "test password");
        made.addIdentity("Mine");
        for (const auto &file : {boxOne, boxTwo}) {
            const auto key = made.addMailboxKey();
            bm::Mailbox box;
            box.create(file, key, made.mailboxKey(key));
        }
        made.lock();
        bm::Vault other;
        other.create(vaultTwo, "test password");
        other.addIdentity("Other");
        other.lock();
    }
    QDir().mkpath(temp.filePath("menu-node"));
    {
        // The session starts as an earlier run left it: locked, with the lists it kept.
        QSettings config(temp.filePath("menu-node/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultOne);
        config.setValue("recentVaults", QStringList{vaultOne, vaultTwo});
        config.setValue("recentMailboxes", QStringList{boxTwo, boxOne});
    }
    bm::Session own(temp.filePath("menu-node"), true);
    bm::DesktopWindow window(own, temp.filePath("sounds-menu"));
    window.show();
    auto file = window.menuBar()->actions().first()->menu();
    require(file && file->title() == "File", "the menu bar starts with File");
    const auto texts = [](QMenu *menu) {
        QStringList result;
        for (auto action : menu->actions())
            result << (action->isSeparator() ? QString("-") : action->text());
        return result;
    };
    // The object names of the items that are grey, in menu order.
    const auto grey = [&] {
        QStringList names;
        for (auto action : file->actions())
            if (!action->isSeparator() && !action->isEnabled())
                names << (action->menu() ? action->menu()->objectName() : action->objectName());
        return names;
    };
    // A recent list's entries: each one's name and the path it carries.
    const auto recent = [&](const char *menu) {
        QStringList entries;
        for (auto action : window.findChild<QMenu *>(menu)->actions())
            entries << action->text() + "|" + action->toolTip();
        return entries;
    };
    // Chooses an entry of a recent list; the lock screen's prompt for a vault is queued.
    const auto pick = [&](const char *menu, const QString &name) {
        for (auto action : window.findChild<QMenu *>(menu)->actions())
            if (action->text() == name && action->isEnabled()) {
                action->trigger();
                QCoreApplication::processEvents();
                return true;
            }
        return false;
    };
    const auto trigger = [&](const char *name) {
        window.findChild<QAction *>(name)->trigger();
        QCoreApplication::processEvents();
    };
    // Runs an action that asks for a file and answers the dialog: with path, or Cancel.
    const auto chooseIn = [&](const char *action, const QString &path) {
        auto answer = new QTimer(&window);
        QObject::connect(answer, &QTimer::timeout, &window, [&, answer] {
            if (auto d = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                answer->stop();
                if (path.isEmpty())
                    d->reject();
                else
                    chooseFile(d, path);
            }
        });
        answer->start(10);
        trigger(action);
        answer->deleteLater();
    };
    const auto lockedName = [&] { return window.findChild<QLabel *>("lockedVaultName")->text(); };
    // The items by their words, "-" a separator.
    QStringList layout = QString("New vault…|Open vault…|Open recent vault|Close vault|-|"
                                 "New mailbox…|Open mailbox…|Open recent mailbox|Close mailbox|-|"
                                 "Import PyBitmessage data…|Back up mailbox and vault…|Settings…")
                             .split('|');
#ifndef Q_OS_MACOS
    layout << "Quit"; // on macOS the application menu has it
#endif
    require(texts(file) == layout,
            "File lists the vault's items, the mailbox's, then the import, the copy, Settings and "
            "Quit");
    require(window.findChild<QAction *>("closeVaultAction")->shortcut() == QKeySequence("Ctrl+L") &&
                window.findChild<QAction *>("settingsAction")->menuRole() ==
                    QAction::PreferencesRole,
            "Close vault keeps Ctrl+L, and Settings is the application menu's Preferences");
    // Locked: what opens or makes a vault is on, the rest waits.
    require(!own.unlocked(), "test sanity: the session starts locked");
    require(grey() == QStringList({"closeVaultAction", "createMailboxAction", "openMailboxAction",
                                   "recentMailboxesMenu", "closeMailboxAction",
                                   "importPyBitmessageAction", "backupAction"}),
            "locked, only the vault items, Settings and Quit are on");
    require(recent("recentVaultsMenu") ==
                QStringList({"menu-one.bmvault|" + vaultOne, "menu-two.bmvault|" + vaultTwo}),
            "Open recent vault lists the vaults last used, newest first, each with its path");
    require(pick("recentVaultsMenu", "menu-two.bmvault") && !own.unlocked() &&
                lockedName() == "menu-two.bmvault",
            "a recent vault chosen from the menu asks for its password on the lock screen");
    own.submitVaultPassword("test password");
    require(own.unlocked() && own.vaultPath() == vaultTwo, "and opens with it");
    require(recent("recentVaultsMenu") ==
                QStringList({"menu-two.bmvault|" + vaultTwo, "menu-one.bmvault|" + vaultOne}),
            "the list follows: the vault just opened comes first");
    // A vault open: choosing another closes it first, as a new vault or a mailbox takes the open
    // one's place. Choosing the open one changes nothing.
    require(pick("recentVaultsMenu", "menu-two.bmvault") && own.unlocked(),
            "choosing the open vault changes nothing");
    require(pick("recentVaultsMenu", "menu-one.bmvault") && !own.unlocked() &&
                lockedName() == "menu-one.bmvault",
            "with a vault open, choosing another closes it and asks for the other's password");
    own.submitVaultPassword("test password");
    require(own.unlocked() && own.vaultPath() == vaultOne, "and opens with it");
    require(grey() ==
                QStringList({"closeMailboxAction", "importPyBitmessageAction", "backupAction"}),
            "with a vault open, Close mailbox, Import and Back up wait for a mailbox");
    require(recent("recentMailboxesMenu") ==
                QStringList({"menu-two.bmmail|" + boxTwo, "menu-one.bmmail|" + boxOne}),
            "Open recent mailbox lists the mailboxes last used, newest first, each with its path");
    require(pick("recentMailboxesMenu", "menu-one.bmmail") && own.mailboxOpen() &&
                own.mailPath() == boxOne,
            "a recent mailbox chosen from the menu opens");
    require(grey().isEmpty(), "with a mailbox open, nothing in File is grey");
    require(recent("recentMailboxesMenu") ==
                QStringList({"menu-one.bmmail|" + boxOne, "menu-two.bmmail|" + boxTwo}),
            "the list follows: the mailbox just opened comes first");
    trigger("closeMailboxAction");
    require(!own.mailboxOpen() && own.unlocked() &&
                grey() ==
                    QStringList({"closeMailboxAction", "importPyBitmessageAction", "backupAction"}),
            "Close mailbox closes the mailbox and keeps the vault");
    require(pick("recentMailboxesMenu", "menu-two.bmmail") && own.mailPath() == boxTwo,
            "the mailbox is opened again from the list");
    // Open vault… asks for a file. Cancel keeps what is open; a file closes the open vault first.
    chooseIn("openVaultAction", {});
    require(own.unlocked() && own.mailboxOpen() && own.mailPath() == boxTwo,
            "Cancel in Open vault… keeps the open vault and its mailbox");
    chooseIn("openVaultAction", vaultTwo);
    require(!own.unlocked() && lockedName() == "menu-two.bmvault",
            "Open vault… with a vault open closes it and asks for the chosen vault's password");
    own.submitVaultPassword("test password");
    require(own.unlocked() && own.vaultPath() == vaultTwo && !own.mailboxOpen(),
            "and opens it, with no mailbox of the other vault's");
    trigger("closeVaultAction");
    require(!own.unlocked() && grey().size() == 7 && grey().first() == "closeVaultAction",
            "Close vault locks it, and the mailbox items wait again");
    // Nothing used yet: each list says so, in grey.
    bm::Session fresh(temp.filePath("menu-fresh-node"), true);
    bm::DesktopWindow bare(fresh, temp.filePath("sounds-menu-fresh"));
    for (const auto &[menu, none] :
         {std::pair<const char *, QString>{"recentVaultsMenu", "No recent vaults"},
          {"recentMailboxesMenu", "No recent mailboxes"}}) {
        const auto entries = bare.findChild<QMenu *>(menu)->actions();
        require(entries.size() == 1 && entries[0]->text() == none && !entries[0]->isEnabled(),
                "a recent list with nothing in it says so, in grey");
    }
}
// "Mark all as read" in the list of letters: the unread letters the list shows, on every page of
// it, and nothing else.
static void testMarkAllRead(const QTemporaryDir &temp) {
    const auto vaultFile = temp.filePath("mark-all.bmvault");
    const auto boxFile = temp.filePath("mark-all.bmmail");
    const auto sender = bm::Protocol::identity("mark-all sender").address;
    const auto other = bm::Protocol::identity("mark-all other").address;
    const auto anonymous = bm::Protocol::identity("mark-all anonymous").address;
    const auto chanA = bm::Protocol::identity("mark-all chan a").address;
    const auto chanB = bm::Protocol::identity("mark-all chan b").address;
    QString me;
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        me = made.addIdentity("Mine");
        const auto key = made.addMailboxKey();
        bm::Mailbox box;
        box.create(boxFile, key, made.mailboxKey(key));
        bm::Mailbox::Batch batch(box);
        for (int n = 1; n <= 230; ++n)
            box.store("inbox-" + QString::number(n), sender, me, "Letter " + QString::number(n),
                      "body", -1);
        for (int n = 1; n <= 10; ++n) // from an address to itself: the Anonymous filter's
            box.store("anon-" + QString::number(n), anonymous, anonymous,
                      "Anonymous " + QString::number(n), "body", -1);
        for (int n = 1; n <= 6; ++n)
            box.store("needle-" + QString::number(n), other, me, "Needle " + QString::number(n),
                      "body", -1);
        for (int n = 1; n <= 3; ++n)
            box.store("archive-" + QString::number(n), sender, me, "Kept " + QString::number(n),
                      "body", -1, "Archive");
        for (int n = 1; n <= 4; ++n)
            box.store("chan-a-" + QString::number(n), sender, chanA, "A " + QString::number(n),
                      "body", -1, "Channels");
        for (int n = 1; n <= 3; ++n)
            box.store("chan-b-" + QString::number(n), sender, chanB, "B " + QString::number(n),
                      "body", -1, "Channels");
        batch.commit();
        for (int n = 1; n <= 5; ++n) // five are read already
            box.markRead("inbox-" + QString::number(n));
        made.lock();
    }
    QDir().mkpath(temp.filePath("mark-all-node"));
    {
        QSettings config(temp.filePath("mark-all-node/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultFile);
        config.setValue("mailbox", boxFile);
    }
    bm::Session own(temp.filePath("mark-all-node"), true);
    own.beginVaultUnlock();
    own.submitVaultPassword("test password");
    auto model = own.messageModel();
    require(own.mailboxOpen() && model->folder() == "Inbox", "test sanity: the Inbox is listed");
    const auto unreadInbox = [&] { return own.messageCount("Inbox", {}, {}, true); };
    require(unreadInbox() == 225 + 10 + 6, "test sanity: the Inbox has its unread letters");
    require(model->hasUnread(), "a list with unread letters knows it");
    // A filter narrows it: Anonymous only marks the anonymous letters.
    model->setAnonymousOnly(true);
    require(model->rowCount() == 10, "test sanity: the Anonymous filter lists ten letters");
    require(model->markAllRead() == 10 && unreadInbox() == 225 + 6,
            "with the Anonymous filter on, Mark all as read marks what it lists and no others");
    model->setAnonymousOnly(false);
    // A search narrows it: the letters it found.
    model->setSearch("Needle");
    require(searched(model) && model->rowCount() == 6 && model->hasUnread(),
            "test sanity: the search finds six letters");
    require(model->markAllRead() == 6 && unreadInbox() == 225,
            "with a search, Mark all as read marks the letters it found and no others");
    model->setSearch({});
    // The folder, on every page of it.
    require(model->rowCount() == 230 + 10 + 6, "test sanity: three pages of letters");
    // Newest first, so the read ones (the needles, the anonymous, the first five) are at the ends.
    const auto unreadRow = [&](int row) {
        return model->data(model->index(row), Qt::UserRole + 9).toBool();
    };
    require(unreadRow(16) && unreadRow(150) && unreadRow(230),
            "test sanity: rows on three pages are drawn as unread");
    require(model->markAllRead() == 225 && unreadInbox() == 0 && !model->hasUnread(),
            "Mark all as read marks every unread letter of the folder, on every page");
    require(!unreadRow(16) && !unreadRow(150) && !unreadRow(230), "and the rows are drawn as read");
    require(model->markAllRead() == 0, "with nothing unread, it marks nothing");
    require(own.messageCount("Archive", {}, {}, true) == 3,
            "the letters of another folder are not touched");
    // A channel's letters: that channel's.
    model->setFolder("Channels");
    require(model->markAllRead() == 0 && !model->hasUnread(),
            "with no channel chosen, the list is empty and there is nothing to mark");
    model->setChannel(chanA);
    require(model->rowCount() == 4 && model->hasUnread(), "test sanity: a channel lists its four");
    require(model->markAllRead() == 4 && !own.channelUnread(chanA) && own.channelUnread(chanB),
            "in a channel, Mark all as read marks that channel's letters and no other's");
    // Unread only: the list empties as they are read.
    model->setFolder("Archive");
    model->setUnreadOnly(true);
    require(model->rowCount() == 3, "test sanity: Unread only lists the three");
    require(model->markAllRead() == 3 && model->rowCount() == 0,
            "with Unread only on, the letters leave the list as they are marked");
}
// Right-click in the list of letters: Mark all as read, on while the list has something unread.
static void testLettersContextMenu(const QTemporaryDir &temp) {
    const auto vaultFile = temp.filePath("letters-menu.bmvault");
    const auto boxFile = temp.filePath("letters-menu.bmmail");
    const auto sender = bm::Protocol::identity("letters menu sender").address;
    const auto chanA = bm::Protocol::identity("letters menu chan a").address;
    const auto chanB = bm::Protocol::identity("letters menu chan b").address;
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        const auto me = made.addIdentity("Mine");
        const auto key = made.addMailboxKey();
        bm::Mailbox box;
        box.create(boxFile, key, made.mailboxKey(key));
        for (int n = 1; n <= 3; ++n)
            box.store("inbox-" + QString::number(n), sender, me, "Letter " + QString::number(n),
                      "body", -1);
        for (int n = 1; n <= 2; ++n)
            box.store("archive-" + QString::number(n), sender, me, "Kept " + QString::number(n),
                      "body", -1, "Archive");
        for (int n = 1; n <= 2; ++n)
            box.store("chan-a-" + QString::number(n), sender, chanA, "A " + QString::number(n),
                      "body", -1, "Channels");
        box.store("chan-b-1", sender, chanB, "B 1", "body", -1, "Channels");
        made.lock();
    }
    QDir().mkpath(temp.filePath("letters-menu-node"));
    {
        QSettings config(temp.filePath("letters-menu-node/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultFile);
        config.setValue("mailbox", boxFile);
    }
    bm::Session own(temp.filePath("letters-menu-node"), true);
    own.beginVaultUnlock();
    own.submitVaultPassword("test password");
    bm::DesktopWindow window(own, temp.filePath("sounds-letters-menu"));
    window.show();
    auto letters = window.findChild<QListView *>("letters");
    require(letters && letters->contextMenuPolicy() == Qt::CustomContextMenu,
            "the list of letters has a context menu of its own");
    const auto openFolder = [&](const char *name) {
        window.findChild<QToolButton *>(name)->click();
        QCoreApplication::processEvents();
    };
    struct Offer {
        bool shown = false, exists = false, enabled = false;
        QString text;
    };
    // Right-clicks the list and says what the menu offered; with pick, chooses the item.
    const auto rightClick = [&](bool pick) {
        Offer offer;
        auto popup = new QTimer(&window);
        QObject::connect(popup, &QTimer::timeout, &window, [&, popup] {
            auto menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            popup->stop();
            popup->deleteLater();
            offer.shown = true;
            for (auto action : menu->actions())
                if (action->objectName() == "markAllReadAction") {
                    offer.exists = true;
                    offer.enabled = action->isEnabled();
                    offer.text = action->text();
                    if (pick && action->isEnabled()) {
                        menu->setActiveAction(action);
                        QTest::keyClick(menu, Qt::Key_Return);
                    }
                }
            if (menu->isVisible())
                menu->close();
        });
        popup->start(10);
        emit letters->customContextMenuRequested(QPoint(5, 5));
        QCoreApplication::processEvents();
        return offer;
    };
    openFolder("folderIcon_Inbox");
    auto offer = rightClick(false);
    require(offer.shown && offer.exists && offer.text == "Mark all as read" && offer.enabled,
            "right-clicking the list offers Mark all as read, on while it has unread letters");
    offer = rightClick(true);
    require(own.messageCount("Inbox", {}, {}, true) == 0 &&
                own.messageCount("Archive", {}, {}, true) == 2,
            "choosing it marks the letters of the list as read, and no other folder's");
    offer = rightClick(false);
    require(offer.shown && offer.exists && !offer.enabled,
            "with nothing unread in the list, the item is grey");
    openFolder("folderIcon_Archive");
    offer = rightClick(true);
    require(offer.enabled && own.messageCount("Archive", {}, {}, true) == 0,
            "in another folder it marks that folder's letters");
    openFolder("folderIcon_Drafts");
    offer = rightClick(false);
    require(offer.shown && offer.exists && !offer.enabled, "an empty list has nothing to mark");
    // In a channel it is that channel's letters, and the rail follows: its chip stops being bold.
    openFolder("folderIcon_Channels");
    const auto chip = [&](const QString &address) -> QPushButton * {
        for (auto candidate : window.findChildren<QPushButton *>("channelChip"))
            if (candidate->text() == address || candidate->toolTip().contains(address))
                return candidate;
        return nullptr;
    };
    require(chip(chanA) && chip(chanB) && chip(chanA)->font().bold() && chip(chanB)->font().bold(),
            "test sanity: the chips of two channels with unread letters are bold");
    chip(chanA)->click();
    QCoreApplication::processEvents();
    offer = rightClick(true);
    require(offer.enabled && !own.channelUnread(chanA) && own.channelUnread(chanB) &&
                !chip(chanA)->font().bold() && chip(chanB)->font().bold(),
            "in a channel, Mark all as read marks that channel's letters, and its chip stops "
            "being bold");
}
// A vault with two mailboxes. The first has unread letters in Inbox (3), Channels (2) and
// Broadcasts (4), five in the Archive and one in Sent (which are not for mail), and three unread
// chat messages from two peers; the second has five unread letters of its own.
struct UnreadFixture {
    QString vault, box, other, me, peerA, peerB;
};
static UnreadFixture makeUnreadFixture(const QTemporaryDir &temp, const QString &name) {
    UnreadFixture f;
    f.vault = temp.filePath(name + ".bmvault");
    f.box = temp.filePath(name + ".bmmail");
    f.other = temp.filePath(name + "-other.bmmail");
    const auto sender = bm::Protocol::identity(name + " sender").address;
    const auto chan = bm::Protocol::identity(name + " chan").address;
    f.peerA = bm::Protocol::identity(name + " peer a").address;
    f.peerB = bm::Protocol::identity(name + " peer b").address;
    bm::Vault made;
    made.create(f.vault, "test password");
    f.me = made.addIdentity("Mine");
    const auto key = made.addMailboxKey();
    bm::Mailbox box;
    box.create(f.box, key, made.mailboxKey(key));
    const auto number = [](int n) { return QString::number(n); };
    for (int n = 1; n <= 3; ++n)
        box.store("inbox-" + number(n), sender, f.me, "Letter " + number(n), "body", -1);
    for (int n = 1; n <= 2; ++n)
        box.store("chan-" + number(n), sender, chan, "Chan " + number(n), "body", -1, "Channels");
    for (int n = 1; n <= 4; ++n)
        box.store("bc-" + number(n), sender, sender, "Post " + number(n), "body", -1, "Broadcasts");
    for (int n = 1; n <= 5; ++n) // stored unread like the rest, but not in a folder for mail
        box.store("kept-" + number(n), sender, f.me, "Kept " + number(n), "body", -1, "Archive");
    box.store("sent-1", f.me, sender, "Sent", "body", -1, "Sent");
    const qint64 expiry = QDateTime::currentSecsSinceEpoch() + 86400;
    box.storeChat("chat-a1", f.peerA, f.me, 1, "hi", expiry, false);
    box.storeChat("chat-a2", f.peerA, f.me, 2, "hi again", expiry, false);
    box.storeChat("chat-b1", f.peerB, f.me, 1, "hello", expiry, false);
    const auto otherKey = made.addMailboxKey();
    bm::Mailbox other;
    other.create(f.other, otherKey, made.mailboxKey(otherKey));
    for (int n = 1; n <= 5; ++n)
        other.store("other-" + number(n), sender, f.me, "Other " + number(n), "body", -1);
    made.lock();
    // A session on a node folder of its own starts with this vault and its first mailbox.
    QDir().mkpath(temp.filePath(name + "-node"));
    QSettings config(temp.filePath(name + "-node/desktop.ini"), QSettings::IniFormat);
    config.setValue("vault", f.vault);
    config.setValue("mailbox", f.box);
    return f;
}
// The number on the app's icon: the unread letters of the folders that receive mail, and the unread
// chat messages, together.
static void testUnreadTotal(const QTemporaryDir &temp) {
    const auto f = makeUnreadFixture(temp, "unread-total");
    const auto boxFile = f.box, otherBox = f.other, me = f.me, peerA = f.peerA, peerB = f.peerB;
    bm::Session own(temp.filePath("unread-total-node"), true);
    // Inbox, Channels, Subscriptions and chats, in the order of the rail's pages.
    const auto counts = [&] {
        const auto c = own.unreadCounts();
        return QList<int>{c.inbox, c.channels, c.broadcasts, c.chats};
    };
    require(own.unreadTotal() == 0 && counts() == QList<int>({0, 0, 0, 0}),
            "with no vault open, there is nothing unread to show");
    own.beginVaultUnlock();
    own.submitVaultPassword("test password");
    require(own.mailboxOpen(), "test sanity: the mailbox is open");
    require(counts() == QList<int>({3, 2, 4, 3}),
            "the counts are the unread letters of Inbox, Channels and Subscriptions, and chats");
    require(
        own.unreadTotal() == 3 + 2 + 4 + 3,
        "the total is the unread letters of Inbox, Channels and Broadcasts, and the unread chats");
    own.readLetter("inbox-1");
    require(own.unreadTotal() == 11 && counts() == QList<int>({2, 2, 4, 3}),
            "reading a letter takes one off its page");
    own.markLettersRead({"chan-1", "chan-2"});
    require(own.unreadTotal() == 9 && counts() == QList<int>({2, 0, 4, 3}),
            "marking letters read takes them off");
    require(own.markChatRead(me, peerA) == 2 && own.unreadTotal() == 7 &&
                counts() == QList<int>({2, 0, 4, 1}),
            "reading a conversation takes its messages off");
    require(own.markChatMessageRead("chat-b1") && own.unreadTotal() == 6 &&
                counts() == QList<int>({2, 0, 4, 0}),
            "so does reading one chat message");
    require(own.messageCount("Archive", {}, {}, true) == 5,
            "test sanity: the Archive's letters are still unread");
    // Another mailbox is its own count, though nothing in this one changed in between.
    own.openMailboxAt(otherBox);
    require(own.unreadTotal() == 5 && counts() == QList<int>({5, 0, 0, 0}),
            "another mailbox has its own count");
    own.openMailboxAt(boxFile);
    require(own.unreadTotal() == 6 && counts() == QList<int>({2, 0, 4, 0}),
            "and this one is back as it was");
    // A letter written through another connection to the file, which the session's own change
    // counter knows nothing of, is counted all the same.
    {
        bm::Vault peek;
        peek.unlock(f.vault, "test password");
        bm::Mailbox elsewhere;
        for (const auto &id : peek.mailboxIds()) {
            try {
                elsewhere.open(boxFile, peek.mailboxKey(id));
                break;
            } catch (...) {
            }
        }
        require(elsewhere.isOpen(), "test sanity: another connection opens the mailbox");
        elsewhere.store("elsewhere-1", peerA, me, "Written elsewhere", "body", -1);
        elsewhere.close();
    }
    require(own.unreadTotal() == 7 && counts() == QList<int>({3, 0, 4, 0}),
            "a letter written through another connection is counted");
    own.closeMailbox();
    require(own.unlocked() && own.unreadTotal() == 0 && counts() == QList<int>({0, 0, 0, 0}),
            "with no mailbox open there is nothing to show");
    own.openMailboxAt(boxFile);
    own.lock();
    require(own.unreadTotal() == 0 && counts() == QList<int>({0, 0, 0, 0}),
            "locked, there is nothing to show");
}
// The number on the app's icon follows the window: the unread letters and chats as the mailbox
// opens and is read, nothing while it is locked, and the switch in Settings.
static void testBadgeInWindow(const QTemporaryDir &temp) {
    const auto f = makeUnreadFixture(temp, "badge-window");
    bm::AppBadge::setEnabled(true); // a run that died with it off must not start this one off
    std::vector<qint64> given;
    bm::AppBadge::setSinkForTesting([&](qint64 number) { given.push_back(number); });
    {
        bm::Session own(temp.filePath("badge-window-node"), true);
        bm::DesktopWindow window(own, temp.filePath("sounds-badge"));
        window.show();
        QCoreApplication::processEvents();
        require(given.empty(), "locked, with nothing unread to show, nothing is given");
        own.beginVaultUnlock();
        own.submitVaultPassword("test password");
        require(own.mailboxOpen() && !given.empty() && given.back() == 12,
                "the mailbox opening gives the badge its unread letters and chats");
        own.readLetter("inbox-1");
        require(given.back() == 11, "reading a letter takes one off the badge");
        own.markChatRead(f.me, f.peerA);
        own.markLettersRead({"bc-1", "bc-2"});
        require(QTest::qWaitFor([&] { return given.back() == 7; }, 5000),
                "so do reading a conversation and marking letters read");
        // The switch in Settings.
        window.findChild<QAction *>("settingsAction")->trigger();
        QCoreApplication::processEvents();
        auto check = window.findChild<QCheckBox *>("badgeEnabledCheck");
        require(check && check->isChecked(), "Settings has the switch, on by default");
        check->click();
        require(!bm::AppBadge::enabled() && given.back() == 0,
                "switching it off takes the badge off");
        own.readLetter("inbox-2");
        require(given.back() == 0, "and it stays off as letters are read");
        check->click();
        require(bm::AppBadge::enabled() && given.back() == 6,
                "switching it on shows the number as it is now");
        own.lock();
        require(given.back() == 0, "locking the vault takes the badge off");
    }
    bm::AppBadge::setSinkForTesting({});
    bm::AppBadge::setEnabled(true);
}
// The round count on a rail icon.
static void testRailBadgeWidget() {
    QWidget rail;
    rail.setFixedSize(38, 38);
    bm::RailBadge badge(&rail);
    rail.show();
    require(!badge.isVisible(), "with no count it is not there");
    badge.setCount(5);
    require(badge.isVisible() && badge.text() == "5", "a count shows its number");
    const int single = badge.width();
    require(single > 10 && badge.height() > 10, "test sanity: it has a size");
    badge.setCount(42);
    require(badge.text() == "42" && badge.width() > single, "two digits make it wider");
    badge.setCount(99);
    require(badge.text() == "99", "99 is shown as it is");
    badge.setCount(100);
    require(badge.text() == "99+" && badge.isVisible(), "past 99 it says 99+");
    badge.setCount(123456);
    require(badge.text() == "99+", "however far past");
    badge.setCount(0);
    require(!badge.isVisible(), "no count takes it away");
    badge.setCount(-4);
    require(!badge.isVisible(), "and so does one below zero");
    // In the corner of the icon, inside it.
    badge.setCount(88);
    require(rail.rect().contains(badge.geometry()) && badge.geometry().right() >= 34 &&
                badge.geometry().top() <= 3,
            "it sits inside the icon's top right corner");
    // Drawn in the theme's accent, away from the digits.
    const auto sample = [&](bool dark) {
        badge.setDark(dark);
        return badge.grab().toImage().pixelColor(4, badge.height() / 2);
    };
    require(sample(false) == QColor("#126d65"), "in the light theme the badge is the light accent");
    require(sample(true) == QColor("#73d8c7"), "in the dark theme it is the dark one");
}
// The rail's badges: where the unread letters and chats are, page by page.
static void testRailBadges(const QTemporaryDir &temp) {
    const auto f = makeUnreadFixture(temp, "rail-badges");
    bm::Session own(temp.filePath("rail-badges-node"), true);
    bm::DesktopWindow window(own, temp.filePath("sounds-rail"));
    window.show();
    QCoreApplication::processEvents();
    const auto badgeOf = [&](const QString &page) {
        auto button = window.findChild<QToolButton *>("folderIcon_" + page);
        return button ? button->findChild<bm::RailBadge *>() : nullptr;
    };
    // What a page's icon shows: its badge's text, empty when there is no badge on show.
    const auto shown = [&](const QString &page) {
        auto badge = badgeOf(page);
        return badge && badge->isVisible() ? badge->text() : QString();
    };
    const auto all = [&] {
        return QStringList{shown("Chats"), shown("Inbox"), shown("Channels"), shown("Broadcasts")};
    };
    require(all() == QStringList({"", "", "", ""}), "locked, no icon has a badge");
    own.beginVaultUnlock();
    own.submitVaultPassword("test password");
    require(all() == QStringList({"3", "3", "2", "4"}),
            "an open mailbox's icons show their unread: Chats, Inbox, Channels, Subscriptions");
    for (const QString page :
         {"Drafts", "Outbox", "Sent", "Archive", "Trash", "Identities", "Contacts", "Settings"})
        require(window.findChild<QToolButton *>("folderIcon_" + page) && !badgeOf(page),
                "an icon that holds nothing new has no badge");
    own.readLetter("inbox-1");
    require(all() == QStringList({"3", "2", "2", "4"}), "reading a letter takes one off its icon");
    own.markLettersRead({"chan-1", "chan-2"});
    require(all() == QStringList({"3", "2", "", "4"}),
            "an icon with nothing unread loses its badge");
    own.markChatRead(f.me, f.peerA); // the window hears of it with its next tick
    require(QTest::qWaitFor([&] { return shown("Chats") == "1"; }, 5000),
            "reading a conversation takes its messages off the Chats icon");
    require(window.findChild<QToolButton *>("folderIcon_Chats")->property("unread").toInt() == 1,
            "and the Chats button keeps the number where it was kept");
    // The theme: the badges follow it.
    window.findChild<QAction *>("settingsAction")->trigger();
    QCoreApplication::processEvents();
    auto theme = window.findChild<QComboBox *>("themeCombo");
    require(theme, "test sanity: Settings has the theme switch");
    theme->setCurrentIndex(theme->findData("dark"));
    require(badgeOf("Inbox")->dark() && badgeOf("Broadcasts")->dark(),
            "the dark theme darkens the badges");
    theme->setCurrentIndex(theme->findData("light"));
    require(!badgeOf("Inbox")->dark() && !badgeOf("Broadcasts")->dark(),
            "and the light theme lightens them");
    own.lock();
    require(all() == QStringList({"", "", "", ""}), "locking takes every badge off");
}
// Importing the data of a PyBitmessage folder: the File menu item, what the dialog shows before
// anything is brought in, the progress, the stop, the result, and what lands in the vault and the
// mailbox.
static void testPyBitmessageImport(const QTemporaryDir &temp) {
    // The target: a vault with one identity of its own, and its mailbox.
    const auto vaultFile = temp.filePath("pybm.bmvault"), boxFile = temp.filePath("pybm.bmmail");
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        made.addIdentity("Mine");
        const auto key = made.addMailboxKey();
        bm::Mailbox box;
        box.create(boxFile, key, made.mailboxKey(key));
        made.lock();
    }
    QDir().mkpath(temp.filePath("pybm-node"));
    {
        QSettings config(temp.filePath("pybm-node/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultFile);
        config.setValue("mailbox", boxFile);
    }
    // PyBitmessage's folder: three identities (a chan, one switched off), and a bit of everything.
    pybmfix::Folder source;
    const auto me = bmc::Protocol::identity("me"), room = bmc::Protocol::identity("room"),
               old = bmc::Protocol::identity("old");
    source.keys(pybmfix::kSettings + pybmfix::section(me, "Me") +
                pybmfix::section(room, "[chan] general", true) +
                pybmfix::section(old, "Old one", false, false));
    const std::string broadcast = bmc::pybm::kBroadcastAddress;
    source.inbox(pybmfix::rawHash(1), me.address, "BM-aaa", "d1", "1700000100", "body", "inbox",
                 false);
    source.inbox(pybmfix::rawHash(2), me.address, "BM-aaa", "d2", "1700000200", "body", "inbox",
                 true);
    source.inbox(pybmfix::rawHash(3), room.address, "BM-ccc", "c1", "1700000300", "body", "inbox",
                 false);
    source.inbox(pybmfix::rawHash(4), broadcast, "BM-ddd", "b1", "1700000400", "body", "inbox",
                 true);
    source.inbox(pybmfix::rawHash(5), me.address, "BM-aaa", "t1", "1700000500", "body", "trash",
                 true);
    // Chat messages of this app, which PyBitmessage on the same identity holds as letters in its
    // inbox: one unread, one read, one deleted over there.
    source.inbox(pybmfix::rawHash(7), me.address, "BM-eee", "", "1700000800",
                 bmc::chatMessageBody(1792000000001, "Hello **there**"), "inbox", false);
    source.inbox(pybmfix::rawHash(8), me.address, "BM-eee", "", "1700000900",
                 bmc::chatMessageBody(1792000000002, "And again"), "inbox", true);
    source.inbox(pybmfix::rawHash(9), me.address, "BM-eee", "", "1700001000",
                 bmc::chatMessageBody(1792000000003, "Deleted"), "trash", true);
    source.sent(pybmfix::rawHash(6), "BM-aaa", me.address, "s1", "body", bmc::Bytes(38, 1),
                1700000600, "msgsent");
    source.sent(bmc::Bytes(16, 2), "BM-aaa", me.address, "u1", "body", bmc::Bytes(38, 2),
                1700000700, "awaitingpubkey");
    source.subscription("News", "BM-news", true);
    source.contact("Ann", "BM-ann");

    bm::Session own(temp.filePath("pybm-node"), true);
    bm::DesktopWindow window(own, temp.filePath("sounds-pybm"));
    window.show();
    // The File menu item: there when a vault and a mailbox are, grey when one is not.
    auto action = window.findChild<QAction *>("importPyBitmessageAction");
    require(action != nullptr, "the File menu has an item to import PyBitmessage data");
    require(action->text() == window.tr("Import PyBitmessage data…"), "its text");
    require(!action->isEnabled(), "locked, there is nothing to import into");
    own.beginVaultUnlock();
    own.submitVaultPassword("test password");
    require(own.mailboxOpen() && action->isEnabled(), "with a vault and a mailbox open it works");
    own.closeMailbox();
    require(!action->isEnabled(), "without a mailbox there is nowhere to put letters");
    own.openMailboxAt(boxFile);
    require(own.mailboxOpen(), "test sanity: the mailbox opens again");
    QCoreApplication::processEvents();
    require(action->isEnabled(), "and it works again");

    // How long is left, said as a person would.
    using Dialog = bm::PyBitmessageImportDialog;
    require(Dialog::timeLeftText(-1) == Dialog::tr("estimating the time left…"),
            "before there is something to go by it says so");
    require(Dialog::timeLeftText(20) == Dialog::tr("less than a minute left"), "seconds");
    require(Dialog::timeLeftText(44.4) == Dialog::tr("less than a minute left"), "under 45 s");
    require(Dialog::timeLeftText(60) == Dialog::tr("about %1 min left").arg(1), "a minute");
    require(Dialog::timeLeftText(200) == Dialog::tr("about %1 min left").arg(3), "minutes");
    require(Dialog::timeLeftText(4000) == Dialog::tr("about %1 min left").arg(67),
            "an hour or less");
    require(Dialog::timeLeftText(5400) == Dialog::tr("about %1 h %2 min left").arg(1).arg(30),
            "hours and minutes");
    require(Dialog::timeLeftText(5500) == Dialog::tr("about %1 h %2 min left").arg(1).arg(30),
            "an hour or more in steps of five minutes");
    require(Dialog::timeLeftText(0) == Dialog::tr("less than a minute left"),
            "done is not unknown");

    // The first page: the folder, and what it holds, counted.
    const auto sourceDir = QString::fromStdString(source.dir);
    Dialog dialog(own, {sourceDir});
    dialog.show();
    QCoreApplication::processEvents();
    const auto label = [&](const char *name) {
        auto l = dialog.findChild<QLabel *>(name);
        require(l != nullptr, name);
        return l->text();
    };
    const auto check = [&](const char *name) {
        auto c = dialog.findChild<QCheckBox *>(name);
        require(c != nullptr, name);
        return c;
    };
    const auto button = [&](const char *name) {
        auto b = dialog.findChild<QPushButton *>(name);
        require(b != nullptr, name);
        return b;
    };
    auto folders = dialog.findChild<QComboBox *>("pybmFolderCombo");
    require(folders && folders->itemData(0).toString() == sourceDir,
            "the folder given is the one shown");
    require(folders->itemText(folders->count() - 1) == Dialog::tr("Choose another folder…"),
            "and another can be chosen");
    require(label("pybmIdentitiesCount") == Dialog::tr("%1, chans: %2").arg(3).arg(1),
            "the identities are counted, and the chans among them");
    require(label("pybmContactsCount") == "1", "contacts");
    require(label("pybmSubscriptionsCount") == "2", "subscriptions");
    require(label("pybmInboxCount") == Dialog::tr("%1, unread: %2").arg(6).arg(3),
            "the letters, and how many are unread");
    require(label("pybmSentCount") == Dialog::tr("%1, not sent: %2").arg(2).arg(1),
            "sent letters, and those never sent");
    require(label("pybmTrashCount") == "2", "deleted letters");
    for (const char *name : {"pybmIdentitiesCheck", "pybmContactsCheck", "pybmSubscriptionsCheck",
                             "pybmInboxCheck", "pybmSentCheck", "pybmTrashCheck"})
        require(check(name)->isChecked() && check(name)->isEnabled(), name);
    require(button("pybmImportButton")->isEnabled(), "it can be started");
    require(button("pybmImportButton")->isDefault(), "and Enter starts it");
    require(label("pybmNotes")
                .contains(Dialog::tr("Switched off in PyBitmessage, active here after "
                                     "the import: %1.")
                              .arg(1)),
            "what comes in switched on, though it was off, is said");
    require(dialog.findChild<QLabel *>("pybmTrouble")->isHidden(), "nothing is wrong");

    // A lock file in the folder says that PyBitmessage looks to be running.
    {
        bmc::test::writeFile(source.dir + "/singleton.lock", bmc::view(std::string("4242")));
        Dialog running(own, {sourceDir});
        running.show();
        QCoreApplication::processEvents();
        require(running.findChild<QLabel *>("pybmNotes")
                    ->text()
                    .contains(Dialog::tr(
                        "PyBitmessage seems to be running. You can import now, but closing "
                        "it first gives a complete copy.")),
                "a PyBitmessage that seems to be running is said so");
        std::filesystem::remove(source.dir + "/singleton.lock");
    }
    // A folder with no data is said so, and cannot be imported from.
    const auto empty = temp.filePath("pybm-empty");
    QDir().mkpath(empty);
    {
        Dialog nothing(own, {empty});
        nothing.show();
        QCoreApplication::processEvents();
        auto trouble = nothing.findChild<QLabel *>("pybmTrouble");
        require(trouble && trouble->isVisible() &&
                    trouble->text().contains(Dialog::tr("There is no PyBitmessage data in this "
                                                        "folder. Choose the folder that holds "
                                                        "keys.dat and messages.dat.")),
                "an empty folder says why it cannot be imported from");
        require(!nothing.findChild<QPushButton *>("pybmImportButton")->isEnabled(),
                "and nothing can be started");
    }
    {
        Dialog nowhere(own, {});
        nowhere.show();
        QCoreApplication::processEvents();
        require(nowhere.findChild<QLabel *>("pybmTrouble")
                    ->text()
                    .contains(Dialog::tr(
                        "No PyBitmessage data found on this computer. Choose its folder.")),
                "with no folder to offer it asks for one");
    }

    // What the folder does not hold is not offered.
    {
        pybmfix::Folder little;
        little.inbox(pybmfix::rawHash(50), me.address, "BM-aaa", "only", "1700000100", "body",
                     "inbox", true);
        Dialog partial(own, {QString::fromStdString(little.dir)});
        partial.show();
        QCoreApplication::processEvents();
        for (const char *name :
             {"pybmIdentitiesCheck", "pybmContactsCheck", "pybmSentCheck", "pybmTrashCheck"}) {
            auto c = partial.findChild<QCheckBox *>(name);
            require(c && !c->isEnabled() && !c->isChecked(), name);
        }
        require(partial.findChild<QCheckBox *>("pybmInboxCheck")->isChecked() &&
                    partial.findChild<QPushButton *>("pybmImportButton")->isEnabled(),
                "what it holds is offered");
        require(partial.findChild<QLabel *>("pybmContactsCount")->text() == QString::fromUtf8("—"),
                "and nothing is counted for the rest");
        require(partial.findChild<QLabel *>("pybmTrashHint")->isHidden(),
                "nothing is said of deleted letters it has none of");
    }

    // The import: leave the deleted letters out (and be told what that may do), start, and wait.
    auto hint = dialog.findChild<QLabel *>("pybmTrashHint");
    require(hint && hint->isHidden(), "nothing is said while the deleted letters come along");
    check("pybmTrashCheck")->setChecked(false);
    require(hint->isVisible() && hint->text() == Dialog::tr("Without them, deleted letters that "
                                                            "your node still holds can come back "
                                                            "into your inbox."),
            "leaving them out says what may follow");
    button("pybmImportButton")->click();
    require(own.importing() || dialog.findChild<QLabel *>("pybmResultTitle")->isVisible(),
            "the scan waits while it goes");
    require(QTest::qWaitFor(
                [&] { return dialog.findChild<QLabel *>("pybmResultTitle")->isVisible(); }, 15000),
            "the import comes to its result page");
    require(label("pybmResultTitle") == Dialog::tr("Import finished"), "it finished");
    const auto result = label("pybmResultText");
    for (const auto &line :
         {Dialog::tr("Letters added: %1").arg(6),
          Dialog::tr("Identities and chans added: %1").arg(3),
          Dialog::tr("Chat messages added: %1").arg(2), Dialog::tr("Contacts added: %1").arg(1),
          Dialog::tr("Subscriptions added: %1").arg(2),
          Dialog::tr("Unsent letters kept as drafts: %1").arg(1)})
        require(result.contains(line), qPrintable("the result says: " + line));
    require(!own.importing(), "the scan goes on after it");
    require(own.identities().size() == 4, "the identities are in the vault");
    require(own.messageCount("Inbox", {}, {}, false) == 2 &&
                own.messageCount("Channels", {}, {}, false) == 1 &&
                own.messageCount("Broadcasts", {}, {}, false) == 1 &&
                own.messageCount("Sent", {}, {}, false) == 1 &&
                own.messageCount("Drafts", {}, {}, false) == 1 &&
                own.messageCount("Trash", {}, {}, false) == 0,
            "every letter is in its folder, and the deleted one was left out");
    require(own.messageCount("Inbox", {}, {}, true) == 1, "read as they were");
    // The chat messages are chats: a conversation with the one who sent them, in the order they
    // were said, the unread one counted, and no letter among the inbox's.
    {
        const QString local = QString::fromStdString(me.address);
        const auto chats = own.chatConversations();
        require(chats.size() == 1 && chats[0].toMap()["address"] == "BM-eee" &&
                    chats[0].toMap()["local"] == local && chats[0].toMap()["unread"] == 1,
                "the chat messages of the inbox are a conversation, one of them unread");
        const auto thread = own.chatMessages(local, "BM-eee");
        require(thread.size() == 2 && thread[0].toMap()["body"] == "Hello **there**" &&
                    thread[1].toMap()["body"] == "And again",
                "with their text, not the document they were sent as, oldest first");
    }
    require(own.unreadTotal() == 3, "and the icons count the unread ones, chats among them");
    require(!button("pybmCopyButton")->text().isEmpty() && button("pybmCloseButton")->isVisible(),
            "the result can be copied and closed");
    require(button("pybmCloseButton")->isDefault() && !button("pybmImportButton")->isDefault(),
            "and Enter closes it");
    button("pybmCloseButton")->click();
    require(!dialog.isVisible(), "closing closes");

    // Again: nothing is added twice; the deleted letters are asked for this time.
    {
        Dialog again(own, {sourceDir});
        again.show();
        QCoreApplication::processEvents();
        again.findChild<QPushButton *>("pybmImportButton")->click();
        auto title = again.findChild<QLabel *>("pybmResultTitle");
        require(QTest::qWaitFor([&] { return title->isVisible(); }, 15000),
                "the second import ends");
        const auto text = again.findChild<QLabel *>("pybmResultText")->text();
        require(text.contains(Dialog::tr("Letters added: %1").arg(1)) &&
                    text.contains(Dialog::tr("Letters already here: %1").arg(6)),
                "only the letter left out the first time is new");
        require(text.contains(Dialog::tr("Chat messages already here: %1").arg(2)) &&
                    !text.contains(Dialog::tr("Chat messages added: %1").arg(2)) &&
                    text.contains(Dialog::tr("Deleted chat messages left out: %1").arg(1)),
                "the chat messages are there already, and a deleted one has no folder to go to");
        require(own.chatConversations().size() == 1 && own.chatUnread() == 1,
                "no chat is added twice");
        require(text.contains(Dialog::tr("Identities already in the vault: %1").arg(3)),
                "the identities are there already");
        require(own.messageCount("Trash", {}, {}, false) == 1 && own.identities().size() == 4,
                "nothing is added twice");
    }

    // A slow import can be watched and stopped.
    {
        const auto before = own.messageCount("Inbox", {}, {}, false);
        pybmfix::Folder many;
        many.keys(pybmfix::kSettings);
        for (int i = 0; i < 100; ++i)
            many.inbox(pybmfix::rawHash(100 + i), me.address, "BM-zzz", "many " + std::to_string(i),
                       std::to_string(1700001000 + i), "body", "inbox", true);
        Dialog slow(own, {QString::fromStdString(many.dir)});
        slow.setPaceForTesting(2, 30);
        slow.show();
        QCoreApplication::processEvents();
        slow.findChild<QPushButton *>("pybmImportButton")->click();
        auto bar = slow.findChild<QProgressBar *>("pybmProgressBar");
        require(bar &&
                    QTest::qWaitFor([&] { return bar->value() > 0 && bar->maximum() > 0; }, 10000),
                "the bar moves");
        require(own.importing() && slow.running(), "and the scan waits meanwhile");
        require(slow.findChild<QLabel *>("pybmProgressDetail")->text().contains(" of 100 "),
                "it says how many letters of how many");
        slow.findChild<QPushButton *>("pybmStopButton")->click();
        auto title = slow.findChild<QLabel *>("pybmResultTitle");
        require(QTest::qWaitFor([&] { return title->isVisible(); }, 10000),
                "a stopped import ends");
        require(title->text() == Dialog::tr("Import stopped"), "and says it was stopped");
        require(!own.importing() && !slow.running(), "the scan goes on");
        const auto got = own.messageCount("Inbox", {}, {}, false) - before;
        require(got > 0 && got < 100, "what was stored stays, and not all of it was");
        // Escape does not close a window that is busy: it stops it, and the result is shown.
        pybmfix::Folder more;
        more.keys(pybmfix::kSettings);
        for (int i = 0; i < 100; ++i)
            more.inbox(pybmfix::rawHash(200 + i), me.address, "BM-yyy", "more " + std::to_string(i),
                       std::to_string(1700002000 + i), "body", "inbox", true);
        Dialog escaped(own, {QString::fromStdString(more.dir)});
        escaped.setPaceForTesting(2, 30);
        escaped.show();
        QCoreApplication::processEvents();
        escaped.findChild<QPushButton *>("pybmImportButton")->click();
        auto bar2 = escaped.findChild<QProgressBar *>("pybmProgressBar");
        require(QTest::qWaitFor([&] { return bar2->value() > 0 && bar2->maximum() > 0; }, 10000),
                "the second slow import goes");
        escaped.reject();
        require(escaped.isVisible() && escaped.running(), "Escape does not close it at once");
        auto title2 = escaped.findChild<QLabel *>("pybmResultTitle");
        require(QTest::qWaitFor([&] { return title2->isVisible(); }, 10000) &&
                    title2->text() == Dialog::tr("Import stopped") && escaped.isVisible(),
                "it stops, and shows what it did");
        escaped.reject();
        require(!escaped.isVisible(), "and Escape then closes it");
        // A dialog closed from outside (the app quits) lets the scan go on, with what was stored.
        {
            Dialog gone(own, {QString::fromStdString(more.dir)});
            gone.setPaceForTesting(2, 30);
            gone.show();
            QCoreApplication::processEvents();
            gone.findChild<QPushButton *>("pybmImportButton")->click();
            require(own.importing(), "a third import goes");
        }
        require(!own.importing(), "a dialog that is gone leaves the scan free");
    }

    // The scan of the node's objects waits while an import goes, and goes on after it.
    {
        const auto node = temp.filePath("pybm-node");
        auto db = ntb_object_db_open(node.toUtf8().constData());
        require(db != nullptr, "test sanity: the node's store opens");
        uint8_t hash[32] = {0x42};
        const QByteArray object(100, 'z'); // nothing the scan can read, but something to pass over
        require(ntb_object_db_save(db, hash, reinterpret_cast<const uint8_t *>(object.constData()),
                                   size_t(object.size()), 1700000000),
                "test sanity: an object is stored");
        ntb_object_db_close(db);
        own.setImporting(true);
        QTest::qWait(1800); // two ticks of the session
        require(own.coreMailbox().checkpoint() == 0, "the scan waits while an import goes");
        own.setImporting(false);
        require(QTest::qWaitFor([&] { return own.coreMailbox().checkpoint() > 0; }, 8000),
                "and goes on when it is over");
    }
}
// The offer of the import: each time a mailbox that has not dealt with it opens or is made, and
// PyBitmessage's data is on the computer, a box offers it. Not now asks again next time; Don't ask
// again and an import that finishes settle the mailbox, and the import that finishes warns that
// PyBitmessage's folder is not encrypted.
static void testPyBitmessageOffer(const QTemporaryDir &temp) {
    using Dialog = bm::PyBitmessageImportDialog;
    // One vault, three mailboxes that have not dealt with PyBitmessage, and a folder of its.
    const auto vaultFile = temp.filePath("offer.bmvault");
    const QStringList mailboxes = {temp.filePath("offer-a.bmmail"), temp.filePath("offer-b.bmmail"),
                                   temp.filePath("offer-c.bmmail")};
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        made.addIdentity("Mine");
        for (const auto &path : mailboxes) {
            const auto key = made.addMailboxKey();
            bm::Mailbox box;
            box.create(path, key, made.mailboxKey(key));
        }
        made.lock();
    }
    QDir().mkpath(temp.filePath("offer-node"));
    {
        QSettings config(temp.filePath("offer-node/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultFile);
        config.setValue("mailbox", mailboxes[0]);
    }
    pybmfix::Folder source;
    const auto me = bmc::Protocol::identity("me");
    source.keys(pybmfix::kSettings + pybmfix::section(me, "Me"));
    source.inbox(pybmfix::rawHash(1), me.address, "BM-aaa", "d1", "1700000100", "body", "inbox",
                 false);
    const auto sourceDir = QString::fromStdString(source.dir);
    QStringList found; // what the computer holds of PyBitmessage
    bm::Session::setPyBitmessageFoldersForTesting([&found] { return found; });
    struct Restore { // the other tests find nothing, whatever way this one ends
        ~Restore() {
            bm::Session::setPyBitmessageFoldersForTesting([] { return QStringList(); });
        }
    } restore;
    bm::Session own(temp.filePath("offer-node"), true);
    bm::DesktopWindow window(own, temp.filePath("sounds-offer"));
    window.show();
    // The offer box, if one is up; the window is given its turn first.
    const auto offer = [&]() -> QMessageBox * {
        QCoreApplication::processEvents();
        for (auto box : window.findChildren<QMessageBox *>("pybmOfferBox"))
            if (box->isVisible())
                return box;
        return nullptr;
    };
    const auto answer = [&](const char *name) {
        auto box = offer();
        require(box != nullptr, "test sanity: there is an offer to answer");
        auto button = box->findChild<QPushButton *>(name);
        require(button != nullptr, name);
        button->click();
        QCoreApplication::processEvents();
    };

    // With no PyBitmessage data on the computer there is nothing to offer, and nothing is marked.
    own.beginVaultUnlock();
    own.submitVaultPassword("test password");
    require(own.mailboxOpen() && !offer(),
            "with no PyBitmessage data on the computer nothing is offered");
    require(!own.pyBitmessageImportSettled(),
            "and the mailbox is not marked: the data may turn up");

    // With some, the box: what it is about, where the data is, three answers.
    found = {sourceDir};
    own.openMailboxAt(mailboxes[0]);
    auto box = offer();
    require(box != nullptr,
            "a mailbox that opens while PyBitmessage's data is about is offered the import");
    require(box->text() ==
                bm::DesktopWindow::tr("Import your PyBitmessage data into this mailbox?"),
            "the offer says what it is about");
    require(box->informativeText().contains(QDir::toNativeSeparators(sourceDir)) &&
                box->informativeText().contains(bm::DesktopWindow::tr(
                    "You can also do this later with Import PyBitmessage data… in the File menu.")),
            "and where the data was found, and how to do it later");
    const auto text = [&](const char *name) {
        auto button = box->findChild<QPushButton *>(name);
        return button ? button->text() : QString("(missing)");
    };
    require(text("pybmOfferImportButton") == bm::DesktopWindow::tr("Import…") &&
                text("pybmOfferLaterButton") == bm::DesktopWindow::tr("Not now") &&
                text("pybmOfferNeverButton") == bm::DesktopWindow::tr("Don't ask again"),
            "there are three answers: import, not now, and don't ask again");
    require(box->defaultButton() == box->findChild<QPushButton *>("pybmOfferImportButton"),
            "and Enter imports");
    require(box->windowModality() == Qt::WindowModal,
            "it is a sheet of the window, not an app-wide box");

    // Not now leaves the mailbox as it was: it is offered the import again when it opens next.
    answer("pybmOfferLaterButton");
    require(!offer() && !own.pyBitmessageImportSettled(),
            "Not now closes the offer and leaves the mailbox unmarked");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(window.findChildren<QMessageBox *>("pybmOfferBox").isEmpty(),
            "and a closed offer is gone, not kept hidden");
    own.rescan(); // the window hears of changes all the time; the offer is made once per opening
    require(!offer(), "and it does not come back while the mailbox stays open");
    own.openMailboxAt(mailboxes[0]);
    require(offer() != nullptr, "and the mailbox is offered the import again when it opens next");
    // Escape is Not now.
    QTest::keyClick(offer(), Qt::Key_Escape);
    require(QTest::qWaitFor([&] { return !offer(); }, 3000) && !own.pyBitmessageImportSettled(),
            "Escape does what Not now does");
    own.openMailboxAt(mailboxes[0]);
    require(offer() != nullptr, "test sanity: Escape left the mailbox to be offered the import");

    // Don't ask again marks it, in the mailbox's own file: it holds when the file is opened again.
    answer("pybmOfferNeverButton");
    require(!offer() && own.pyBitmessageImportSettled(), "Don't ask again marks the mailbox");
    own.openMailboxAt(mailboxes[0]);
    require(own.mailboxOpen() && own.pyBitmessageImportSettled() && !offer(),
            "and it is not offered the import again, whenever it is opened");

    // The flag is the mailbox's: another one has not dealt with it.
    own.openMailboxAt(mailboxes[1]);
    require(offer() != nullptr && !own.pyBitmessageImportSettled(),
            "another mailbox is offered the import: the flag belongs to the mailbox");

    // Import… opens the import dialog on the data that was found. The dialog is waited for where
    // Import… is pressed, so a timer works it: it starts the import and reads the result page, or,
    // with run false, cancels at the first page.
    struct Seen {
        bool opened = false, warningShown = false, plain = false;
        QString folder, warning;
    };
    const auto importFromOffer = [&](bool run) {
        Seen seen;
        auto driver = new QTimer(&window);
        QObject::connect(driver, &QTimer::timeout, &window, [&, driver, run] {
            auto d = window.findChild<QDialog *>("pybmImportDialog");
            if (!d || !d->isVisible())
                return;
            auto title = d->findChild<QLabel *>("pybmResultTitle");
            if (title && title->isVisible()) {
                auto warning = d->findChild<QLabel *>("pybmWarning");
                seen.warningShown = warning && warning->isVisible();
                seen.plain = warning && warning->textFormat() == Qt::PlainText;
                seen.warning = warning ? warning->text() : QString();
                driver->stop();
                d->findChild<QPushButton *>("pybmCloseButton")->click();
                return;
            }
            auto start = d->findChild<QPushButton *>("pybmImportButton");
            if (!seen.opened && start && start->isVisible()) {
                seen.opened = true;
                seen.folder = d->findChild<QComboBox *>("pybmFolderCombo")->itemData(0).toString();
                if (run) {
                    start->click();
                } else {
                    driver->stop();
                    d->findChild<QPushButton *>("pybmCancelButton")->click();
                }
            }
        });
        driver->start(20);
        answer("pybmOfferImportButton"); // the dialog runs inside this, until the driver ends it
        driver->deleteLater();
        return seen;
    };
    const auto cancelled = importFromOffer(false);
    require(cancelled.opened && cancelled.folder == sourceDir,
            "Import… opens the import dialog on the folder that was found");
    require(!own.pyBitmessageImportSettled(),
            "and Cancel there deals with nothing: Import… marks no mailbox by itself");
    own.openMailboxAt(mailboxes[1]);
    require(offer() != nullptr, "so the mailbox is offered the import again");
    const auto seen = importFromOffer(true);
    require(seen.opened && seen.folder == sourceDir, "Import… opens the dialog once more");
    require(own.pyBitmessageImportSettled(), "an import that finishes marks the mailbox");
    require(seen.warningShown &&
                seen.warning ==
                    Dialog::tr("PyBitmessage keeps its data without encryption, your private keys "
                               "included. If you have fully moved to ynotbit, consider deleting "
                               "the folder %1. ynotbit never deletes it for you.")
                        .arg(QDir::toNativeSeparators(sourceDir)),
            "and its result warns that PyBitmessage's folder is not encrypted, and names it");
    require(seen.plain, "the folder's name in the warning is text, never markup");
    own.openMailboxAt(mailboxes[1]);
    require(own.mailboxOpen() && !offer(),
            "a mailbox that took the import is not offered it again");

    // A mailbox made now is offered the import too.
    const auto madeNow = temp.filePath("offer-made-now.bmmail");
    {
        auto reply = new QTimer(&window);
        QObject::connect(reply, &QTimer::timeout, &window, [&, reply] {
            if (auto d = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                reply->stop();
                chooseFile(d, madeNow);
            }
        });
        reply->start(10);
        own.createMailbox();
        reply->deleteLater();
    }
    require(own.mailboxOpen() && own.mailPath() == madeNow && offer() != nullptr,
            "a mailbox made now is offered the import too");
    answer("pybmOfferLaterButton");
    // So is one that opens because the vault is unlocked.
    own.lock();
    own.beginVaultUnlock();
    own.submitVaultPassword("test password");
    require(own.mailboxOpen() && own.mailPath() == madeNow && offer() != nullptr,
            "and one that opens when the vault is unlocked");
    answer("pybmOfferLaterButton");

    // Closing the mailbox takes an offer away with it, shown or not; the next one gets its own.
    own.openMailboxAt(mailboxes[2]);
    require(offer() != nullptr, "test sanity: the third mailbox is offered the import");
    own.closeMailbox();
    require(!offer(), "an offer does not outlive the mailbox it was made for");
    own.openMailboxAt(mailboxes[2]);
    own.closeMailbox(); // before the window has had its turn
    require(!offer(), "and an offer for a mailbox that is closed before it shows is not made");
    own.openMailboxAt(mailboxes[2]);
    answer("pybmOfferLaterButton");

    // An import that was stopped has not dealt with it: no mark, no warning.
    {
        pybmfix::Folder many;
        many.keys(pybmfix::kSettings);
        for (int i = 0; i < 100; ++i)
            many.inbox(pybmfix::rawHash(300 + i), me.address, "BM-zzz", "many " + std::to_string(i),
                       std::to_string(1700001000 + i), "body", "inbox", true);
        Dialog slow(own, {QString::fromStdString(many.dir)});
        slow.setPaceForTesting(2, 30);
        slow.show();
        QCoreApplication::processEvents();
        slow.findChild<QPushButton *>("pybmImportButton")->click();
        auto bar = slow.findChild<QProgressBar *>("pybmProgressBar");
        require(bar &&
                    QTest::qWaitFor([&] { return bar->value() > 0 && bar->maximum() > 0; }, 10000),
                "test sanity: the slow import goes");
        slow.findChild<QPushButton *>("pybmStopButton")->click();
        auto title = slow.findChild<QLabel *>("pybmResultTitle");
        require(QTest::qWaitFor([&] { return title->isVisible(); }, 10000) &&
                    title->text() == Dialog::tr("Import stopped"),
                "test sanity: it is stopped");
        require(!own.pyBitmessageImportSettled(),
                "an import that was stopped does not mark the mailbox");
        auto warning = slow.findChild<QLabel *>("pybmWarning");
        require(warning != nullptr && warning->isHidden(),
                "and gives no warning: the data is not all in");
    }
    // The same dialog, from the File menu, to its end: it marks the mailbox all the same.
    {
        Dialog complete(own, {sourceDir});
        complete.show();
        QCoreApplication::processEvents();
        complete.findChild<QPushButton *>("pybmImportButton")->click();
        auto title = complete.findChild<QLabel *>("pybmResultTitle");
        require(QTest::qWaitFor([&] { return title->isVisible(); }, 15000) &&
                    title->text() == Dialog::tr("Import finished"),
                "test sanity: the import finishes");
        require(own.pyBitmessageImportSettled(), "an import that finishes marks the mailbox");
    }
}
// The people met in the letters an import brings in join the address book, as they do for a mailbox
// from before the address book was filled: named from their address, a name already given stays,
// the vault's own are left out. What is added is what the import stored, so a second import with
// no new letters adds no one, and a contact deleted since stays deleted.
static void testPyBitmessageImportAddsPeople(const QTemporaryDir &temp) {
    using Dialog = bm::PyBitmessageImportDialog;
    const auto vaultFile = temp.filePath("people.bmvault"),
               boxFile = temp.filePath("people.bmmail");
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        made.addIdentity("Mine");
        const auto key = made.addMailboxKey();
        bm::Mailbox box;
        box.create(boxFile, key, made.mailboxKey(key));
        made.lock();
    }
    QDir().mkpath(temp.filePath("people-node"));
    {
        QSettings config(temp.filePath("people-node/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultFile);
        config.setValue("mailbox", boxFile);
    }
    const auto me = bmc::Protocol::identity("me"); // comes in with the keys
    const auto ann = bmc::Protocol::identity("Ann who wrote").address,
               ben = bmc::Protocol::identity("Ben written to").address,
               cy = bmc::Protocol::identity("Cy deleted over there").address,
               dee = bmc::Protocol::identity("Dee in both books").address,
               eve = bmc::Protocol::identity("Eve who writes later").address,
               fay = bmc::Protocol::identity("Fay of the long import").address;
    pybmfix::Folder source;
    source.keys(pybmfix::kSettings + pybmfix::section(me, "Me"));
    source.inbox(pybmfix::rawHash(1), me.address, ann, "From Ann", "1700000100", "body", "inbox",
                 false);
    source.inbox(pybmfix::rawHash(2), me.address, ann, "From Ann again", "1700000200", "body",
                 "inbox", true);
    source.inbox(pybmfix::rawHash(3), me.address, dee, "From Dee", "1700000300", "body", "inbox",
                 true);
    source.inbox(pybmfix::rawHash(4), me.address, me.address, "A note to self", "1700000350",
                 "body", "inbox", true);
    source.inbox(pybmfix::rawHash(5), me.address, cy, "Deleted", "1700000400", "body", "trash",
                 true);
    source.sent(pybmfix::rawHash(6), ben, me.address, "To Ben", "body", bmc::Bytes(38, 1),
                1700000500, "msgsent");
    source.contact("Dee from PyBitmessage", dee);
    const auto sourceDir = QString::fromStdString(source.dir);

    bm::Session own(temp.filePath("people-node"), true);
    own.beginVaultUnlock();
    own.submitVaultPassword("test password");
    require(own.mailboxOpen(), "test sanity: the mailbox opens");
    const auto book = [&] {
        QMap<QString, QString> names;
        for (const auto &c : own.contacts())
            names[c.toMap()["address"].toString()] = c.toMap()["label"].toString();
        return names;
    };
    // Runs an import to its end, and says what its result page said.
    const auto importFrom = [&](const QString &dir) {
        Dialog dialog(own, {dir});
        dialog.show();
        QCoreApplication::processEvents();
        dialog.findChild<QPushButton *>("pybmImportButton")->click();
        auto title = dialog.findChild<QLabel *>("pybmResultTitle");
        require(QTest::qWaitFor([&] { return title->isVisible(); }, 15000),
                "test sanity: the import ends");
        return dialog.findChild<QLabel *>("pybmResultText")->text();
    };
    const auto q = [](const std::string &address) { return QString::fromStdString(address); };
    const auto line = [](int n) {
        return Dialog::tr("Contacts added from the letters: %1").arg(n);
    };

    auto expected = book(); // what a mailbox starts with
    const auto first = importFrom(sourceDir);
    expected[q(ann)] = bm::addressTitle(q(ann));
    expected[q(ben)] = bm::addressTitle(q(ben));
    expected[q(dee)] = "Dee from PyBitmessage";
    require(book() == expected,
            "the people the letters came from or went to join the address book, named from their "
            "address: Ann wrote and Ben was written to; Dee keeps the name PyBitmessage gave her; "
            "not the identity that came in, nor Cy, whose letter was deleted over there");
    require(first.contains(line(2)), "and the result says how many joined");

    // A second import has no new letters, so no new people: Ann, deleted since, stays out.
    own.removeContact(q(ann));
    const auto second = importFrom(sourceDir);
    require(!own.isContact(q(ann)) &&
                !second.contains(Dialog::tr("Contacts added from the letters")),
            "a second import adds no one: a contact deleted since is not added again");

    // A letter that comes in with a later import brings its sender, and only the new ones.
    source.inbox(pybmfix::rawHash(7), me.address, eve, "From Eve", "1700000600", "body", "inbox",
                 false);
    const auto third = importFrom(sourceDir);
    require(own.isContact(q(eve)) && !own.isContact(q(ann)) && third.contains(line(1)),
            "a later import brings the sender of its new letters, and no one else");

    // A stopped import has stored letters, and their people join the book all the same.
    pybmfix::Folder many;
    many.keys(pybmfix::kSettings);
    for (int i = 0; i < 100; ++i)
        many.inbox(pybmfix::rawHash(400 + i), me.address, fay, "many " + std::to_string(i),
                   std::to_string(1700001000 + i), "body", "inbox", true);
    Dialog slow(own, {QString::fromStdString(many.dir)});
    slow.setPaceForTesting(2, 30);
    slow.show();
    QCoreApplication::processEvents();
    slow.findChild<QPushButton *>("pybmImportButton")->click();
    auto bar = slow.findChild<QProgressBar *>("pybmProgressBar");
    require(bar && QTest::qWaitFor([&] { return bar->value() > 0 && bar->maximum() > 0; }, 10000),
            "test sanity: the slow import goes");
    slow.findChild<QPushButton *>("pybmStopButton")->click();
    auto title = slow.findChild<QLabel *>("pybmResultTitle");
    require(QTest::qWaitFor([&] { return title->isVisible(); }, 10000) &&
                title->text() == Dialog::tr("Import stopped"),
            "test sanity: it is stopped");
    require(own.isContact(q(fay)) &&
                slow.findChild<QLabel *>("pybmResultText")->text().contains(line(1)),
            "what a stopped import stored brings its people, and the result says so");
}
// The nickbots of a mailbox: the list, the hidden chan that lookups go out from, the lookups, the
// answers that come back, the registration of a nickname, and the nicknames the address book keeps.
static void testNickbots(const QTemporaryDir &temp) {
    const auto vaultFile = temp.filePath("nick.bmvault"), boxFile = temp.filePath("nick.bmmail");
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        made.addIdentity("Mine");
        const auto key = made.addMailboxKey();
        bm::Mailbox box;
        box.create(boxFile, key, made.mailboxKey(key));
        made.lock();
    }
    QDir().mkpath(temp.filePath("nick-node"));
    {
        QSettings config(temp.filePath("nick-node/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultFile);
        config.setValue("mailbox", boxFile);
    }
    const auto addressOf = [](const char *who) {
        return QString::fromStdString(bmc::Protocol::identity(who).address);
    };
    const auto bot = addressOf("the nickbot"), second = addressOf("a second nickbot"),
               ann = addressOf("Ann"), ben = addressOf("Ben"), cy = addressOf("Cy"),
               dee = addressOf("Dee");
    bm::updates::setDefaultNickbotsForTesting({{bot, "nickbot"}});
    struct Restore {
        ~Restore() {
            bm::updates::setDefaultNickbotsForTesting({});
        }
    } restore;

    bm::Session s(temp.filePath("nick-node"), true);
    s.beginVaultUnlock();
    s.submitVaultPassword("test password");
    require(s.mailboxOpen(), "test sanity: the mailbox opens");
    auto &nick = s.nicks();
    qint64 now = QDateTime::currentSecsSinceEpoch();
    nick.setClockForTesting([&] { return now; });
    nick.setDelayForTesting([](int low, int) { return low; }); // no chance in a test
    const auto me = s.identities().first().toMap()["address"].toString();

    // --- the list ------------------------------------------------------------------------------
    require(nick.bots().size() == 1 && nick.bots()[0].address == bot && nick.botIndex(bot) == 0 &&
                nick.botIndex(second) == -1,
            "a mailbox that never set its list has the built-in one: the default nickbot");
    require(nick.enabled(), "nickbots start on");
    QString why;
    require(nick.setBots({{bot, "one"}, {second, "two"}}, &why) && nick.bots().size() == 2 &&
                nick.bots()[1].address == second && nick.botIndex(second) == 1 &&
                nick.bots()[1].label == "two",
            "a list of two is kept, in order, with its labels");
    require(!nick.setBots({{bot, ""}, {bot, ""}}, &why) && !why.isEmpty() &&
                nick.bots().size() == 2,
            "a nickbot twice is refused, and the list stays");
    require(!nick.setBots({{"BM-nope", ""}}, &why) && !why.isEmpty(), "a bad address is refused");
    require(!nick.setBots({{me, ""}}, &why) && !why.isEmpty(),
            "one of the user's own addresses is no nickbot");
    QList<bm::Nickbot> six;
    for (int i = 0; i < 6; ++i)
        six.push_back({addressOf(QByteArray("bot" + QByteArray::number(i)).constData()), ""});
    require(!nick.setBots(six, &why) && nick.bots().size() == 2, "at most five");
    require(nick.setBots(six.mid(0, 5), &why) && nick.bots().size() == 5, "five are fine");
    require(nick.setBots({{bot, "nickbot"}}, &why) && nick.bots().size() == 1,
            "the list shrinks again");
    nick.setEnabled(false);
    require(!nick.enabled(), "the switch turns off");
    nick.setEnabled(true);

    // --- words ---------------------------------------------------------------------------------
    for (const auto &[name, want] :
         QList<QPair<QString, QString>>{{"Quick Otter of Zeta", "@quick_otter_of_zeta"},
                                        {"Ann", "@ann"},
                                        {"  Ben  ", "@ben"},
                                        {"Ünï Ben!!", "@n_ben"},
                                        {"a-b_c", "@a-b_c"},
                                        {"Al", ""},
                                        {"123 456", ""},
                                        {"", ""},
                                        {"BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN", ""},
                                        {"9lives", "@lives"},
                                        {QString(50, 'x'), "@" + QString(32, 'x')}})
        require(bm::NickClient::suggestNick(name) == want,
                qPrintable("a name becomes a nickname: " + name));
    for (int i = 0; i < 4; ++i)
        require(bm::nickbotColor(i) == bm::quoteColor(i + 1),
                "the first four nickbots have the colours of the first four quote levels");
    require(bm::nickbotColor(4) != bm::nickbotColor(0) &&
                bm::nickbotColor(4) != bm::nickbotColor(3) &&
                bm::nickbotColor(-3) == bm::nickbotColor(0) &&
                bm::nickbotColor(9) == bm::nickbotColor(4),
            "the fifth is its own, and a place outside the five takes the nearest");
    require(bm::NickClient::errorText("taken") != bm::NickClient::errorText("syntax") &&
                bm::NickClient::errorText("something new").contains("something new"),
            "every code has words, and an unknown one is quoted");

    // --- the lane ------------------------------------------------------------------------------
    require(nick.laneAddress().isEmpty(), "no lookup chan before one is needed");
    const auto lane = nick.ensureLane();
    require(!lane.isEmpty() && nick.laneAddress() == lane && !nick.laneIsTheUsers(),
            "the chan lookups are sent from is joined when it is first needed");
    require(lane == QString::fromStdString(bmc::Protocol::channel("anonymous", "x", 4).address),
            "it is the chan of the phrase anonymous");
    bool inVault = false;
    for (const auto &i : s.coreVault().identities())
        inVault = inVault || (i.address == lane.toStdString() && i.chan);
    require(inVault, "it is a chan of the vault");
    for (const auto &i : s.identities())
        require(i.toMap()["address"].toString() != lane, "but no list of identities shows it");
    require(nick.ensureLane() == lane, "asked again it is the same chan, joined once");
    require(s.coreClient().delivery().serviceAddresses() ==
                std::set<std::string>{lane.toStdString()},
            "and the scan is told that what comes to it is not mail");
    s.coreClient().delivery().setServiceAddresses({});
    nick.mailboxOpened();
    require(s.coreClient().delivery().serviceAddresses() ==
                std::set<std::string>{lane.toStdString()},
            "a mailbox that opens tells it again");

    // --- a lookup goes out late, from the chan, short-lived --------------------------------------
    QSignalSpy finished(&nick, &bm::NickClient::lookupFinished);
    const auto sentFrom = [&](const QString &from, const QString &to) {
        QStringList texts;
        for (const auto &m : s.coreMailbox().chatMessages(from.toStdString(), to.toStdString()))
            if (m.outgoing)
                texts << QString::fromStdString(m.body);
        return texts;
    };
    require(nick.lookupAddress("@ben").isEmpty(), "a lookup is queued");
    nick.tick();
    require(sentFrom(lane, bot).isEmpty(), "and not sent at once");
    now += 2;
    nick.tick();
    require(sentFrom(lane, bot).isEmpty(), "nor before its delay is over");
    now += 2;
    nick.tick();
    require(sentFrom(lane, bot) == QStringList{"/address @ben"},
            "after the delay it goes, from the lane, to the nickbot, as the command");
    const auto lookupChat =
        s.coreMailbox().chatMessages(lane.toStdString(), bot.toStdString()).front();
    require(lookupChat.expires <= QDateTime::currentSecsSinceEpoch() + 3601 &&
                lookupChat.expires > QDateTime::currentSecsSinceEpoch() + 3000,
            "its life on the network is an hour, not a day");
    require(nick.lookupAddress("@ben").isEmpty(), "asking the same again is no new message");
    now += 600;
    nick.tick();
    require(sentFrom(lane, bot).size() == 1,
            "the same question is not sent twice while one is out");

    // --- the answer ------------------------------------------------------------------------------
    int serial = 0;
    const auto answer = [&](const QString &from, const QString &json, const QString &to = {}) {
        const auto text = "Some words.\nnickbot: " + json.toStdString();
        s.coreMailbox().store("answer-" + std::to_string(++serial), from.toStdString(),
                              (to.isEmpty() ? lane : to).toStdString(), "",
                              bmc::chatMessageBody(1, text), -1, "Service");
    };
    const auto serviceLeft = [&] { return s.coreMailbox().takeService(100).size(); };
    // A stranger's word is nothing: the sender is what the signature proves.
    answer(
        ann,
        R"({"v":1,"cmd":"address","ok":true,"query":"@ben","found":true,"nick":"@ben","address":")" +
            cy + R"("})");
    nick.tick();
    require(
        finished.isEmpty() && serviceLeft() == 0,
        "an answer from an address that is no listed nickbot is not believed, and is cleared away");
    answer(
        bot,
        R"({"v":1,"cmd":"address","ok":true,"query":"@ben","found":true,"nick":"@Ben","address":")" +
            ben + R"("})");
    nick.tick();
    require(finished.size() == 1, "the nickbot's answer finishes the lookup");
    {
        const auto args = finished.takeFirst();
        const auto result = args[3].toMap();
        require(args[0].toString() == "address" && args[2].toString() == bot &&
                    result["found"].toBool() && result["nick"].toString() == "@Ben" &&
                    result["address"].toString() == ben,
                "with what was asked, who answered and the result");
    }
    require(serviceLeft() == 0, "the Service folder is emptied by the tick");
    now += 600;
    nick.tick();
    require(sentFrom(lane, bot).size() == 1, "an answered lookup is not asked again");

    // --- answers that are wrong, and ones nobody asked for
    // ----------------------------------------
    answer(bot, R"({"v":1,"cmd":"address","ok":true,"found":true,"nick":"ben","address":")" + ben +
                    R"("})");
    answer(bot, R"(garbage)");
    nick.tick();
    require(finished.isEmpty(), "an answer with a bad field is not believed");

    // --- a failed lookup says why; a lookup nobody answers times out
    // -------------------------------
    require(nick.lookupAddress("@nobody").isEmpty(), "test sanity: another lookup");
    now += 4;
    nick.tick();
    answer(bot, R"({"v":1,"cmd":"","ok":false,"error":"rate_limited"})");
    nick.tick();
    // The slow-down notice names no command, so it finishes nothing; the lookup times out.
    require(finished.isEmpty(), "a notice that names no command finishes nothing");
    now += 3950;
    nick.tick();
    require(finished.size() == 1 && finished[0][0].toString() == "address" &&
                finished[0][3].toMap()["error"].toString() == "timeout",
            "a lookup that nobody answers ends, with timeout, after its hour and a little more");
    finished.clear();

    // --- what the address book keeps
    // ----------------------------------------------------------------
    s.addContact(ann, "Ann");
    s.addContact(ben, "Ben");
    s.addContact(cy, "Cy");
    require(nick.nicknameOf(ann).text.isEmpty(), "a contact has no nickname to begin with");
    // Anybody can write to the chan: what one who is no listed nickbot says about a contact is not
    // taken.
    answer(addressOf("Mallory who is no nickbot"),
           R"({"v":1,"cmd":"nickname","ok":true,"results":[{"address":")" + ann +
               R"(","nicks":["@mallory"]}]})");
    nick.tick();
    require(nick.nicknameOf(ann).text.isEmpty(),
            "what a stranger says about a contact is not taken");
    require(nick.lookupNicknames({ann, ben}, {}).isEmpty(), "test sanity: a batch lookup");
    now += 4;
    nick.tick();
    require(sentFrom(lane, bot).size() == 3 &&
                sentFrom(lane, bot)[2] == "/nickname " + ann + " " + ben,
            "two addresses go in one message");
    QSignalSpy changed(&nick, &bm::NickClient::changed);
    answer(bot, R"({"v":1,"cmd":"nickname","ok":true,"results":[{"address":")" + ann +
                    R"(","nicks":["@ann","/annbot"]},{"address":")" + ben + R"(","nicks":[]}]})");
    nick.tick();
    require(finished.size() == 1 && !changed.isEmpty(),
            "the batch answer finishes the lookup and changes the book");
    auto info = nick.nicknameOf(ann);
    require(info.text == "@ann" && info.color == 0 && info.bot == "nickbot" &&
                s.nickname(ann) == "@ann" && nick.nicknameOf(ben).text.isEmpty(),
            "a contact gets the first nickname, in the colour of the nickbot that said it");
    finished.clear();
    // A nickname typed or built in is never replaced, only confirmed.
    s.setNickname(cy, "/cyber");
    answer(bot, R"({"v":1,"cmd":"nickname","ok":true,"results":[{"address":")" + cy +
                    R"(","nicks":["@cyrus"]}]})");
    nick.tick();
    require(s.nickname(cy) == "/cyber" && nick.nicknameOf(cy).color == -1,
            "a nickname that was not the nickbot's stays, with no colour");
    require(nick.nicknames().contains(cy) && nick.nicknames()[cy].color == -1,
            "also in the list of them all");
    answer(bot, R"({"v":1,"cmd":"nickname","ok":true,"results":[{"address":")" + cy +
                    R"(","nicks":["/cyber"]}]})");
    nick.tick();
    require(nick.nicknameOf(cy).text == "/cyber" && nick.nicknameOf(cy).color == 0,
            "but if the nickbot says the same, it is the nickbot's from then on");
    // One the nickbot vouched for goes when the nickbot says there is none.
    answer(bot, R"({"v":1,"cmd":"nickname","ok":true,"results":[{"address":")" + ann +
                    R"(","nicks":[]}]})");
    nick.tick();
    require(nick.nicknameOf(ann).text.isEmpty() && s.nickname(ann).isEmpty(),
            "a nickname the nickbot vouched for is dropped when the nickbot has none");
    {
        const auto eve = addressOf("Eve with a nickname of her own");
        s.addContact(eve, "Eve");
        s.setNickname(eve, "/evebot");
        answer(bot, R"({"v":1,"cmd":"nickname","ok":true,"results":[{"address":")" + eve +
                        R"(","nicks":[]}]})");
        nick.tick();
        require(s.nickname(eve) == "/evebot",
                "a nickname the nickbot never vouched for stays when it says the address has none");
    }
    // An answer nobody asked for still teaches the book.
    answer(
        bot,
        R"({"v":1,"cmd":"address","ok":true,"query":"@Ben","found":true,"nick":"@Ben","address":")" +
            ben + R"("})");
    nick.tick();
    require(nick.nicknameOf(ben).text == "@Ben",
            "an answer nobody asked for still teaches the contact");
    // Colours follow the list.
    nick.setBots({{second, "second"}, {bot, "nickbot"}});
    require(nick.nicknameOf(ben).color == 1 && nick.nicknameOf(ben).bot == "nickbot",
            "the colour is the nickbot's place in the list, so it moves with it");
    nick.setBots({{second, "second"}});
    require(nick.nicknameOf(ben).text == "@Ben" && nick.nicknameOf(ben).color == -1,
            "a nickbot no longer listed gives its names no colour");
    nick.setBots({{bot, "nickbot"}});
    const auto all = nick.nicknames();
    require(all.contains(ben) && all[ben].text == "@Ben" && all.contains(cy) && !all.contains(ann),
            "nicknames() lists every address that has one");

    // --- two nickbots: an answer from one does not finish a question put to the other
    // -----------------------
    {
        nick.setBots({{bot, "nickbot"}, {second, "second"}});
        require(nick.lookupAddress("@zeta", bot).isEmpty(), "test sanity: a question to the first");
        now += 4;
        nick.tick();
        finished.clear();
        answer(
            second,
            R"({"v":1,"cmd":"address","ok":true,"query":"@zeta","found":true,"nick":"@zeta","address":")" +
                dee + R"("})");
        nick.tick();
        require(finished.isEmpty(),
                "the second nickbot's answer does not finish the question put to the first");
        answer(
            bot,
            R"({"v":1,"cmd":"address","ok":true,"query":"@zeta","found":true,"nick":"@zeta","address":")" +
                dee + R"("})");
        nick.tick();
        require(finished.size() == 1 && finished[0][2].toString() == bot, "the first one's does");
        finished.clear();
        nick.setBots({{bot, "nickbot"}});
    }

    // --- the switches
    // --------------------------------------------------------------------------------
    nick.setEnabled(false);
    require(!nick.lookupAddress("@ben").isEmpty() && !nick.lookupNicknames({ann}).isEmpty() &&
                !nick.search("benn").isEmpty() && !nick.registerNickname(me, "@mine").isEmpty(),
            "with nickbots off nothing is asked, and each says so");
    nick.setEnabled(true);
    require(!nick.lookupAddress("ben").isEmpty() && !nick.lookupNicknames({"BM-nope"}).isEmpty() &&
                !nick.lookupNicknames({}).isEmpty() && !nick.search("ben").isEmpty() &&
                !nick.search("b%n_").isEmpty() && !nick.lookupAddress("@ben", second).isEmpty(),
            "a bad nickname, a bad address, a short or odd term and a nickbot that is not listed "
            "are refused");
    // What the dialog that asks for a nickname says before anything is typed.
    require(nick.registrationProblem().isEmpty(),
            "with nickbots on and listed nothing is in the way");
    nick.setEnabled(false);
    require(nick.registrationProblem() ==
                bm::NickClient::tr("Nicknames are turned off in Settings"),
            "nickbots off is in the way of a registration, and it says so");
    nick.setEnabled(true);
    {
        const auto listed = nick.bots();
        nick.setBots({});
        require(nick.registrationProblem() == bm::NickClient::tr("No nickbot is listed"),
                "so is an empty list");
        nick.setBots(listed);
    }

    // --- searching
    // ---------------------------------------------------------------------------------
    now += 4000;
    nick.tick();
    finished.clear();
    const auto before = sentFrom(lane, bot).size();
    require(nick.search("BENN").isEmpty(), "a search is queued");
    now += 4;
    nick.tick();
    require(sentFrom(lane, bot).size() == before + 1 &&
                sentFrom(lane, bot).last() == "/search benn",
            "it goes out in lower case, as a command");
    answer(
        bot,
        R"({"v":1,"cmd":"search","ok":true,"query":"benn","results":[{"nick":"@benny","address":")" +
            dee + R"("}],"more":true})");
    nick.tick();
    require(finished.size() == 1 && finished[0][0].toString() == "search" &&
                finished[0][3].toMap()["hits"].toList().size() == 1 &&
                finished[0][3].toMap()["more"].toBool(),
            "the hits come with the answer");
    require(!nick.nicknameOf(dee).text.isEmpty() == s.isContact(dee),
            "a hit who is no contact is not put in the book by a search");
    finished.clear();

    // --- registration
    // --------------------------------------------------------------------------------
    require(nick.registration(me)["state"].toString() == "none",
            "no registration before one is asked for");
    require(!nick.registerNickname(me, "mine").isEmpty() &&
                nick.registerNickname(lane, "@mine") ==
                    bm::NickClient::tr("Choose one of your identities") &&
                nick.registerNickname(addressOf("a stranger"), "@mine") ==
                    bm::NickClient::tr("Choose one of your identities"),
            "a nickname that is none, a chan and an address that is not the vault's are refused, "
            "and it says why");
    require(nick.registerNickname(me, "@mine").isEmpty(), "a registration is queued");
    require(sentFrom(me, bot) == QStringList{"/register @mine"},
            "it is a chat from the identity itself to the nickbot, which the user can read");
    auto reg = nick.registration(me);
    require(reg["state"].toString() == "waiting" && reg["nick"].toString() == "@mine" &&
                reg["bot"].toString() == bot,
            "and it waits for the answer");
    // The answer is a chat in that conversation; its trailer says how it went.
    const auto incoming = [&](const QString &json) {
        const auto text = "Words.\nnickbot: " + json.toStdString();
        s.coreMailbox().storeChat("reg-answer-" + std::to_string(++serial), bot.toStdString(),
                                  me.toStdString(), 100 + serial, text, bmc::now() + 3600);
    };
    incoming(R"({"v":1,"cmd":"register","ok":false,"error":"taken","query":"@mine"})");
    nick.tick();
    reg = nick.registration(me);
    require(reg["state"].toString() == "refused" && reg["error"].toString() == "taken" &&
                s.nickname(me).isEmpty(),
            "a refusal is kept with its reason, and the identity gets no nickname");
    require(nick.registerNickname(me, "@mine2").isEmpty() &&
                nick.registration(me)["state"].toString() == "waiting",
            "a new try starts again");
    now += 5;
    incoming(R"({"v":1,"cmd":"register","ok":true,"query":"@mine2","nick":"@mine2","address":")" +
             me + R"("})");
    nick.tick();
    reg = nick.registration(me);
    require(reg["state"].toString() == "registered" && reg["nick"].toString() == "@mine2" &&
                s.nickname(me) == "@mine2" && nick.nicknameOf(me).color == 0,
            "an acceptance makes the identity's nickname, in the nickbot's colour");
    // Nothing comes back: no answer, after a day.
    require(nick.registerNickname(me, "@mine3").isEmpty(), "test sanity: another try");
    now += 3600;
    nick.tick();
    require(nick.registration(me)["state"].toString() == "waiting", "an hour is not yet no answer");
    now += 24 * 3600;
    nick.tick();
    require(nick.registration(me)["state"].toString() == "noanswer", "a day and more is");
    // A lookup is held back from a registration for a while: the two must not line up.
    nick.registerNickname(me, "@mine4");
    nick.lookupAddress("@ben");
    now += 4;
    nick.tick();
    const auto lanes = sentFrom(lane, bot).size();
    now += 100;
    nick.tick();
    require(sentFrom(lane, bot).size() == lanes,
            "a lookup is not sent within two minutes of a message that names the user");
    now += 30;
    nick.tick();
    require(sentFrom(lane, bot).size() == lanes + 1, "and goes after");

    nick.setBots({{bot, "nickbot"}, {second, "second"}});
    require(nick.registerNickname(me, "@second_one", second).isEmpty() &&
                sentFrom(me, second) == QStringList{"/register @second_one"} &&
                nick.registration(me)["bot"].toString() == second,
            "a registration goes to the nickbot that was asked for, not always to the default");
    nick.setBots({{bot, "nickbot"}});

    // --- a registration waits for a mailbox that is not there yet
    // ---------------------------------------
    nick.registerWhenAMailboxOpens(me, "@later");
    nick.mailboxOpened();
    nick.tick();
    require(sentFrom(me, bot).contains("/register @later") &&
                nick.registration(me)["nick"].toString() == "@later",
            "a registration asked for before the mailbox existed goes out when one opens");
    const auto count = sentFrom(me, bot).size();
    nick.mailboxOpened();
    nick.tick();
    require(sentFrom(me, bot).size() == count, "and only once");

    // --- the book is checked when the user asks, and never on its own
    // ---------------------------------------------------
    {
        QStringList fresh;
        for (int i = 0; i < 11; ++i) {
            const auto a = addressOf(("a contact nobody has checked " + std::to_string(i)).c_str());
            s.addContact(a, "Contact " + QString::number(i));
            fresh << a;
        }
        const auto contacts = s.coreMailbox().contacts().size();
        // The addresses of the /nickname messages sent from the lane after the first `since`.
        const auto asked = [&](int since) {
            QStringList out;
            for (const auto &text : sentFrom(lane, bot).mid(since))
                if (text.startsWith("/nickname "))
                    out << text.mid(10).split(' ');
            return out;
        };
        // The answer to a /nickname message, from a nickbot that has what the book has, and gives
        // `named` the nickname.
        const auto reply = [&](const QString &text, const QString &named, const QString &nickname) {
            const auto addresses = text.mid(10).split(' ');
            QStringList entries;
            for (const auto &a : addresses) {
                const auto has = a == named ? nickname : nick.nicknameOf(a).text;
                entries << R"({"address":")" + a + R"(","nicks":[)" +
                               (has.isEmpty() ? QString() : "\"" + has + "\"") + "]}";
            }
            return R"({"v":1,"cmd":"nickname","ok":true,"results":[)" + entries.join(',') + "]}";
        };
        now += 200; // clear of the registration above
        finished.clear();
        const auto start = sentFrom(lane, bot).size();
        // Nothing is asked about the contacts on its own, however long the time.
        for (int round = 0; round < 3; ++round) {
            now += 10 * 86400;
            nick.tick();
            now += 700;
            nick.tick();
        }
        require(sentFrom(lane, bot).size() == start && !nick.checking(),
                "the contacts are not asked about unless the user asks");
        QString problem;
        nick.setEnabled(false);
        require(nick.checkContacts(&problem) == 0 &&
                    problem == bm::NickClient::tr("Nicknames are turned off in Settings") &&
                    !nick.checking(),
                "with nickbots off a check says so, and asks nothing");
        nick.setEnabled(true);
        problem.clear();
        require(nick.checkContacts(&problem) == contacts && problem.isEmpty() && nick.checking(),
                "a check asks about every contact");
        require(nick.checkContacts(&problem) == 0 &&
                    problem == bm::NickClient::tr("A check is already under way"),
                "and is not started a second time while it is under way");
        QSignalSpy changes(&nick, &bm::NickClient::changed);
        now += 2;
        nick.tick();
        require(sentFrom(lane, bot).size() == start,
                "nothing goes out at once: the first message waits its random time");
        now += 1;
        nick.tick();
        require(sentFrom(lane, bot).size() == start + 1 &&
                    sentFrom(lane, bot).last().split(' ').size() == 1 + 8,
                "then the first goes, eight addresses to a message");
        now += 9;
        nick.tick();
        require(sentFrom(lane, bot).size() == start + 1,
                "the next is ten seconds behind it, not close: a long address book is no burst");
        now += 1;
        nick.tick();
        require(sentFrom(lane, bot).size() == start + 2, "and goes then");
        const int batches = (contacts + 7) / 8;
        for (int i = 2; i < batches; ++i) {
            now += 10;
            nick.tick();
        }
        auto got = asked(start);
        auto want = QStringList();
        for (const auto &c : s.coreMailbox().contacts())
            want << QString::fromStdString(c.address);
        std::sort(got.begin(), got.end());
        std::sort(want.begin(), want.end());
        require(sentFrom(lane, bot).size() == start + batches && got == want,
                "every contact is asked about, once, in the messages of the whole check");
        // The answers come as the nickbot sends them; the check is over with the last.
        const auto messages = sentFrom(lane, bot).mid(start);
        const auto before = changes.size();
        // A contact of this block, with no nickname yet, that the first message asks about.
        QString named;
        for (const auto &a : messages[0].mid(10).split(' '))
            if (named.isEmpty() && fresh.contains(a))
                named = a;
        require(!named.isEmpty(), "test sanity: the first message asks about one of them");
        for (int i = 0; i < messages.size() - 1; ++i) {
            answer(bot, reply(messages[i], named, "@checked_one"));
            nick.tick();
        }
        require(nick.nicknameOf(named).text == "@checked_one",
                "what an answer holds is in the book while the rest are awaited");
        require(nick.checking(), "the check is under way until the last answer");
        answer(bot, reply(messages.last(), {}, {}));
        nick.tick();
        require(!nick.checking() && changes.size() > before,
                "with the last answer it is over, and the client says so");
        // One that is never answered ends too, by the time the answer is overdue.
        require(nick.checkContacts() == contacts, "test sanity: a second check");
        now += 3;
        nick.tick();
        require(nick.checking(), "test sanity: it is under way");
        for (int i = 1; i < batches; ++i) {
            now += 10;
            nick.tick();
        }
        now += 3900 + 1; // longer than a lookup is waited for
        nick.tick();
        require(!nick.checking(), "a check whose answers never come is over when they are overdue");
        nick.setEnabled(true);
    }

    // --- on the screen
    // ------------------------------------------------------------------------------------
    {
        nick.setBots({{bot, "nickbot"}});
        require(s.addContact(dee, bm::addressTitle(dee)),
                "test sanity: a contact whose name is made up");
        nick.adopt(dee, "@dee", bot);
        const auto rowFrom = [&](const QString &sender) {
            const auto id = std::string("show-") + sender.left(12).toStdString();
            s.coreMailbox().store(id, sender.toStdString(), me.toStdString(), "Hello", "body", -1,
                                  "Inbox");
            for (const auto &r : s.messagePage("Inbox", {}, 0, 50))
                if (r.toMap()["hash"].toString() == QString::fromStdString(id))
                    return r.toMap();
            return QVariantMap();
        };
        const auto fromBen = rowFrom(ben);
        require(
            fromBen["fromName"] == "Ben" && fromBen["fromNick"] == "@Ben" &&
                fromBen["fromNickColor"] == 0,
            "a letter from a contact with a nickname carries both, and the colour of the nickbot");
        const auto fromDee = rowFrom(dee);
        require(fromDee["fromName"].toString().isEmpty() && fromDee["fromNick"] == "@dee",
                "a name that is only made up from the address gives way to the nickname");
        const auto fromStranger = rowFrom(addressOf("a stranger with no nickname"));
        require(fromStranger["fromNick"].toString().isEmpty() &&
                    fromStranger["fromNickColor"] == -1,
                "someone with no nickname has none");
        QVariantMap benContact;
        for (const auto &c : s.contacts())
            if (c.toMap()["address"].toString() == ben)
                benContact = c.toMap();
        require(benContact["nickname"] == "@Ben" && benContact["nickColor"] == 0 &&
                    benContact["nickBot"] == "nickbot",
                "the address book rows carry the nickname with its colour and nickbot");
        s.coreMailbox().storeChat("show-chat", ben.toStdString(), me.toStdString(), 1, "hello",
                                  bmc::now() + 3600);
        QString chatLabel;
        for (const auto &c : s.chatConversations())
            if (c.toMap()["address"].toString() == ben)
                chatLabel = c.toMap()["label"].toString();
        require(chatLabel == "@Ben · Ben", "a chat is titled by the nickname and the name");
        // A nickbot's chat is shown without its line for programs; anyone else's, as it is.
        const auto words =
            std::string("Registered @x for you.\nnickbot: {\"v\":1,\"cmd\":\"help\",\"ok\":true}");
        s.coreMailbox().storeChat("shown-bot", bot.toStdString(), me.toStdString(), 7, words,
                                  bmc::now() + 3600);
        s.coreMailbox().storeChat("shown-ben", ben.toStdString(), me.toStdString(), 8, words,
                                  bmc::now() + 3600);
        const auto lastBody = [&](const QString &peer) {
            const auto list = s.chatMessages(me, peer);
            return list.isEmpty() ? QString() : list.last().toMap()["body"].toString();
        };
        require(lastBody(bot) == "Registered @x for you." && lastBody(ben).contains("nickbot: {"),
                "a nickbot's chat is shown without its line for programs, and nobody else's is "
                "changed");
        QString botPreview, benPreview;
        for (const auto &c : s.chatConversations()) {
            if (c.toMap()["address"].toString() == bot)
                botPreview = c.toMap()["body"].toString();
            if (c.toMap()["address"].toString() == ben)
                benPreview = c.toMap()["body"].toString();
        }
        require(botPreview == "Registered @x for you." && benPreview.contains("nickbot: {"),
                "so is the line of the chat list");
        require(
            bm::NickClient::withoutTrailer("Hello.\nnickbot: garbage") ==
                    "Hello.\nnickbot: garbage" &&
                bm::NickClient::withoutTrailer("nickbot: {\"v\":1,\"cmd\":\"help\",\"ok\":true}")
                    .isEmpty(),
            "a last line that is no good is left, and a message that is only a trailer is empty");
        const auto shown = bm::shownName(ben, "Ben", nick.nicknameOf(ben));
        require(shown.html() == "<span style=\"color:" + bm::nickbotColor(0).name() +
                                    "\">@Ben</span> · Ben" &&
                    shown.plain() == "@Ben · Ben",
                "as HTML the nickname is in its colour, and escaped");
        require(bm::shownName(ben, "<b>x</b>", {"@a", -1, {}}).html() ==
                    "@a · &lt;b&gt;x&lt;/b&gt;",
                "and nothing the user's names say can be markup");
        require(bm::shownName(ben, "/echo", {"/echo", 0, "x"}).plain() == "/echo",
                "a name that says what the nickname says is shown once");
    }


    // --- the user's own chan is left alone
    // -------------------------------------------------------------------
    {
        const auto vaultTwo = temp.filePath("nick2.bmvault"),
                   boxTwo = temp.filePath("nick2.bmmail");
        {
            bm::Vault made;
            made.create(vaultTwo, "test password");
            made.addIdentity("Mine");
            made.addChannel("anonymous", "anonymous"); // joined by hand, as any chan is
            const auto key = made.addMailboxKey();
            bm::Mailbox box;
            box.create(boxTwo, key, made.mailboxKey(key));
            made.lock();
        }
        QDir().mkpath(temp.filePath("nick-node-two"));
        {
            QSettings config(temp.filePath("nick-node-two/desktop.ini"), QSettings::IniFormat);
            config.setValue("vault", vaultTwo);
            config.setValue("mailbox", boxTwo);
        }
        bm::Session other(temp.filePath("nick-node-two"), true);
        other.beginVaultUnlock();
        other.submitVaultPassword("test password");
        require(other.mailboxOpen(), "test sanity: the second mailbox opens");
        auto &two = other.nicks();
        QString problem;
        require(two.laneIsTheUsers() && two.laneAddress().isEmpty() &&
                    two.ensureLane(&problem).isEmpty() && !problem.isEmpty() &&
                    !two.lookupAddress("@ben").isEmpty(),
                "a chan 'anonymous' the user joined is not taken over: nothing is looked up, and "
                "it says why");
        bool listed = false;
        for (const auto &i : other.identities())
            listed = listed || i.toMap()["chan"].toBool();
        require(listed, "and the user's chan stays in the lists");
    }
}
// The chan lookups go out from belongs to the vault, which has many mailboxes: one that is opened
// later finds it made and the app's own, and no list shows it with no mailbox open.
static void testLaneBelongsToTheVault(const QTemporaryDir &temp) {
    const auto vaultFile = temp.filePath("nicklane.bmvault"),
               boxOne = temp.filePath("nicklane1.bmmail"),
               boxTwo = temp.filePath("nicklane2.bmmail");
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        made.addIdentity("Mine");
        for (const auto &file : {boxOne, boxTwo}) {
            const auto key = made.addMailboxKey();
            bm::Mailbox box;
            box.create(file, key, made.mailboxKey(key));
        }
        made.lock();
    }
    QDir().mkpath(temp.filePath("nick-node-lane"));
    {
        QSettings config(temp.filePath("nick-node-lane/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultFile);
        config.setValue("mailbox", boxOne);
    }
    bm::Session s(temp.filePath("nick-node-lane"), true);
    s.beginVaultUnlock();
    s.submitVaultPassword("test password");
    require(s.mailboxOpen() && s.document() == "nicklane1.bmmail",
            "test sanity: the first mailbox opens");
    auto &nick = s.nicks();
    const auto lane = nick.ensureLane();
    require(!lane.isEmpty(), "test sanity: the lane is made through the first mailbox");

    s.openMailboxAt(boxTwo);
    require(s.mailboxOpen() && s.document() == "nicklane2.bmmail",
            "test sanity: the second mailbox of the vault opens");
    QString problem;
    require(nick.laneAddress() == lane && !nick.laneIsTheUsers() &&
                nick.ensureLane(&problem) == lane && problem.isEmpty(),
            "another mailbox of the vault finds the chan made, and the app's, not the user's");
    require(s.coreClient().delivery().serviceAddresses() ==
                std::set<std::string>{lane.toStdString()},
            "and its scan is told that what comes to the chan is not mail");
    int copies = 0;
    for (const auto &i : s.coreVault().identities())
        copies += i.address == lane.toStdString();
    require(copies == 1, "the vault has it once");

    s.closeMailbox();
    require(!s.mailboxOpen() && nick.laneAddress() == lane,
            "it is the vault's: closing the mailbox does not lose it");
    for (const auto &i : s.identities())
        require(i.toMap()["address"].toString() != lane,
                "and with no mailbox open the identities do not show it");
}
// Settings > Nickbots: the list with its colours, the order and the switches.
static void testNickbotsPage(const QTemporaryDir &temp) {
    const auto vaultFile = temp.filePath("nickpage.bmvault"),
               boxFile = temp.filePath("nickpage.bmmail");
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        made.addIdentity("Mine");
        made.addChannel("a chan of the nickbots page test", "A chan");
        const auto key = made.addMailboxKey();
        bm::Mailbox box;
        box.create(boxFile, key, made.mailboxKey(key));
        made.lock();
    }
    QDir().mkpath(temp.filePath("nickpage-node"));
    {
        QSettings config(temp.filePath("nickpage-node/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultFile);
        config.setValue("mailbox", boxFile);
    }
    const auto addressOf = [](const char *who) {
        return QString::fromStdString(bmc::Protocol::identity(who).address);
    };
    const auto bot = addressOf("the page's nickbot"),
               second = addressOf("the page's second nickbot");
    bm::updates::setDefaultNickbotsForTesting({{bot, "nickbot"}});
    struct Restore {
        ~Restore() {
            bm::updates::setDefaultNickbotsForTesting({});
        }
    } restore;
    bm::Session s(temp.filePath("nickpage-node"), true);
    s.beginVaultUnlock();
    s.submitVaultPassword("test password");
    require(s.mailboxOpen(), "test sanity: the mailbox opens");
    bm::NickbotsPage page(s);
    page.resize(700, 700);
    page.show();
    QCoreApplication::processEvents();
    auto list = page.findChild<QTreeWidget *>("nickbotsList");
    auto click = [&](const char *name) { page.findChild<QPushButton *>(name)->click(); };
    // Drives a dialog that is open and waiting: finds it as the modal window and calls f on it.
    auto inDialog = [&](const char *name, const std::function<void(QDialog *)> &f) {
        QTimer::singleShot(50, [name, f] {
            auto d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            require(d && d->objectName() == name, "the dialog is open");
            f(d);
        });
    };
    require(list, "the page has its list of nickbots");
    require(list->topLevelItemCount() == 1 &&
                list->topLevelItem(0)->text(0) ==
                    bm::NickbotsPage::tr("%1 (default)").arg("nickbot") &&
                list->topLevelItem(0)->text(1) == bot && !list->topLevelItem(0)->icon(0).isNull(),
            "the list holds the default nickbot, named as the default, with a colour");
    require(!page.findChild<QTreeWidget *>("nickIdentitiesList") &&
                !page.findChild<QPushButton *>("nickRegisterButton"),
            "the identities and their nicknames are not on this page");
    require(page.findChild<QLabel *>("nickbotsIdentitiesNote")->text() ==
                bm::NickbotsPage::tr("To get a nickname for one of your identities, select it on "
                                     "the Identities page and choose Nickname…"),
            "but it says where they are");
    require(page.findChild<QCheckBox *>("nickbotsEnabledCheck")->isChecked() &&
                !page.findChild<QCheckBox *>("nickbotsAutoCheck"),
            "the switch starts on, and there is no other: the contacts are checked when the user "
            "asks, from the Contacts page");

    // Add a nickbot.
    inDialog("nickbotAddDialog", [&](QDialog *d) {
        auto address = d->findChild<QLineEdit *>("nickbotAddressField");
        auto ok = d->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        require(!ok->isEnabled(), "Add waits for an address");
        address->setText("BM-nope");
        require(!ok->isEnabled(), "and for a good one");
        address->setText(s.coreVault().identities().front().address.c_str());
        require(ok->isEnabled(), "an address that is valid enables Add");
        ok->click();
        require(
            d->isVisible() && !d->findChild<QLabel *>("nickbotAddProblem")->text().isEmpty(),
            "one of the user's own addresses is refused, with the reason, and the dialog stays");
        address->setText(second);
        d->findChild<QLineEdit *>("nickbotLabelField")->setText("second");
        ok->click();
    });
    click("nickbotAddButton");
    require(list->topLevelItemCount() == 2 && s.nicks().bots().size() == 2 &&
                list->topLevelItem(1)->text(0) == "second" &&
                list->topLevelItem(1)->text(1) == second,
            "the added nickbot is last in the list");
    require(list->topLevelItem(0)->icon(0).pixmap(16, 16).toImage().pixelColor(8, 8) ==
                    bm::nickbotColor(0) &&
                list->topLevelItem(1)->icon(0).pixmap(16, 16).toImage().pixelColor(8, 8) ==
                    bm::nickbotColor(1) &&
                bm::nickbotColor(0) != bm::nickbotColor(1),
            "each row shows the colour of its place");
    // Order: moving a nickbot moves its colour and the default.
    list->setCurrentItem(list->topLevelItem(1));
    click("nickbotUpButton");
    require(s.nicks().bots()[0].address == second &&
                list->topLevelItem(0)->text(0) ==
                    bm::NickbotsPage::tr("%1 (default)").arg("second"),
            "Move up puts it first, and so makes it the default");
    require(!page.findChild<QPushButton *>("nickbotUpButton")->isEnabled() &&
                page.findChild<QPushButton *>("nickbotDownButton")->isEnabled(),
            "the first row cannot go up, and can go down");
    list->setCurrentItem(list->topLevelItem(1));
    require(!page.findChild<QPushButton *>("nickbotDownButton")->isEnabled() &&
                page.findChild<QPushButton *>("nickbotUpButton")->isEnabled(),
            "the last row cannot go down, and can go up");
    list->setCurrentItem(list->topLevelItem(0));
    click("nickbotRemoveButton");
    require(s.nicks().bots().size() == 1 && s.nicks().bots()[0].address == bot,
            "Remove takes the selected nickbot out");
    for (int i = 0; i < 4; ++i) { // fill to five, and the sixth is not offered
        auto more = s.nicks().bots();
        more.push_back({addressOf(QByteArray("fill" + QByteArray::number(i)).constData()), ""});
        s.nicks().setBots(more);
    }
    require(list->topLevelItemCount() == 5 &&
                !page.findChild<QPushButton *>("nickbotAddButton")->isEnabled(),
            "with five nickbots Add is grey");
    s.nicks().setBots({{bot, "nickbot"}});

    // The switch.
    page.findChild<QCheckBox *>("nickbotsEnabledCheck")->setChecked(false);
    require(!s.nicks().enabled(), "the switch turns nickbots off");
}
// The dialog that asks a nickbot for the nickname of one of the user's identities: what it offers,
// what it refuses, what it says of the last try, and how it ends.
static void testNicknameDialog(const QTemporaryDir &temp) {
    const auto vaultFile = temp.filePath("nickdlg.bmvault"),
               boxFile = temp.filePath("nickdlg.bmmail");
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        made.addIdentity("Mine");
        made.addChannel("a chan of the nickname dialog test", "A chan");
        const auto key = made.addMailboxKey();
        bm::Mailbox box;
        box.create(boxFile, key, made.mailboxKey(key));
        made.lock();
    }
    QDir().mkpath(temp.filePath("nickdlg-node"));
    {
        QSettings config(temp.filePath("nickdlg-node/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultFile);
        config.setValue("mailbox", boxFile);
    }
    const auto addressOf = [](const char *who) {
        return QString::fromStdString(bmc::Protocol::identity(who).address);
    };
    const auto bot = addressOf("the dialog's nickbot"),
               second = addressOf("the dialog's second nickbot");
    bm::updates::setDefaultNickbotsForTesting({{bot, "nickbot"}});
    struct Restore {
        ~Restore() {
            bm::updates::setDefaultNickbotsForTesting({});
        }
    } restore;
    bm::Session s(temp.filePath("nickdlg-node"), true);
    s.beginVaultUnlock();
    s.submitVaultPassword("test password");
    require(s.mailboxOpen(), "test sanity: the mailbox opens");
    QString me, chan;
    for (const auto &v : s.identities())
        (v.toMap()["chan"].toBool() ? chan : me) = v.toMap()["address"].toString();
    require(!me.isEmpty() && !chan.isEmpty(), "test sanity: an identity and a chan");
    int serial = 0;
    // What the nickbot says back is a chat to the identity, with a line for programs at its end.
    const auto answer = [&](const QString &json) {
        s.coreMailbox().storeChat("dialog-answer-" + std::to_string(++serial), bot.toStdString(),
                                  me.toStdString(), 100 + serial,
                                  "Words.\nnickbot: " + json.toStdString(), bmc::now() + 3600);
        s.nicks().tick();
    };
    struct Parts {
        QLineEdit *field;
        QPushButton *ok;
        QLabel *problem, *state;
        QComboBox *which;
    };
    const auto parts = [](bm::NicknameDialog &dialog) {
        return Parts{dialog.findChild<QLineEdit *>("nicknameField"),
                     dialog.findChild<QPushButton *>("nicknameRegisterButton"),
                     dialog.findChild<QLabel *>("nicknameProblem"),
                     dialog.findChild<QLabel *>("nicknameState"),
                     dialog.findChild<QComboBox *>("nicknameBot")};
    };

    // A new identity: a nickname made from its name, nothing said of an earlier try.
    {
        bm::NicknameDialog dialog(s, me, "Mine");
        dialog.show();
        const auto [field, ok, problem, state, which] = parts(dialog);
        require(field && ok && problem && state, "the dialog has its parts");
        require(dialog.findChild<QLabel *>("nicknameIdentityName")->text() == "Mine" &&
                    dialog.findChild<QLabel *>("nicknameIdentityAddress")->text() == me,
                "it says whose nickname it is");
        require(field->text() == "@mine" && ok->isEnabled() && !state->isVisible() &&
                    !problem->isVisible() && !which,
                "a new identity is offered a nickname made from its name, with no earlier try to "
                "report and no nickbot to choose from");
        field->setText("mine");
        require(!ok->isEnabled(), "Register waits for a valid nickname");
        field->setText("#mine");
        require(!ok->isEnabled() && problem->isVisible() &&
                    problem->text() ==
                        bm::NicknameDialog::tr(
                            "# is for chans. Start with @ for a person or / for a bot."),
                "a chan's sigil is refused for an identity, and the dialog says why");
        field->setText("/mine_bot");
        require(ok->isEnabled() && !problem->isVisible(), "a bot's nickname is fine");
        field->setText("@mine_dialog");
        ok->click();
        require(dialog.result() == QDialog::Accepted && !dialog.isVisible(),
                "Register closes the dialog");
        const auto reg = s.nicks().registration(me);
        require(reg["state"].toString() == "waiting" && reg["nick"].toString() == "@mine_dialog" &&
                    reg["bot"].toString() == bot,
                "and asks the default nickbot for the nickname");
    }
    // The try that waits for its answer is reported, and can be asked again.
    {
        bm::NicknameDialog dialog(s, me, "Mine");
        dialog.show();
        const auto [field, ok, problem, state, which] = parts(dialog);
        require(state->isVisible() &&
                    state->text() == bm::NickClient::describe(s.nicks().registration(me)) &&
                    field->text() == "@mine_dialog" && ok->isEnabled(),
                "a registration that waits is reported, its nickname is in the field, and it can "
                "be asked for again");
    }
    // Accepted: the nickname stands, and asking for it again is pointless until it changes.
    answer(
        R"({"v":1,"cmd":"register","ok":true,"query":"@mine_dialog","nick":"@mine_dialog","address":")" +
        me + R"("})");
    {
        bm::NicknameDialog dialog(s, me, "Mine");
        dialog.show();
        const auto [field, ok, problem, state, which] = parts(dialog);
        require(state->isVisible() &&
                    state->text() == bm::NicknameDialog::tr("Registered: %1").arg("@mine_dialog") &&
                    field->text() == "@mine_dialog",
                "a nickname that stands is reported and offered");
        require(!ok->isEnabled(), "and Register has nothing to do with it as it is");
        field->setText("@another_one");
        require(ok->isEnabled(), "but has with another");
        field->setText("@mine_dialog");
        require(!ok->isEnabled(), "and not when it is put back");
    }
    // Refused: the reason is reported in red, and the same nickname can be tried again.
    {
        bm::NicknameDialog dialog(s, me, "Mine");
        dialog.show();
        dialog.findChild<QLineEdit *>("nicknameField")->setText("@taken_name");
        dialog.findChild<QPushButton *>("nicknameRegisterButton")->click();
    }
    answer(R"({"v":1,"cmd":"register","ok":false,"error":"taken","query":"@taken_name"})");
    {
        bm::NicknameDialog dialog(s, me, "Mine");
        dialog.show();
        const auto [field, ok, problem, state, which] = parts(dialog);
        require(state->isVisible() &&
                    state->text() == bm::NickClient::describe(s.nicks().registration(me)) &&
                    state->text().contains(bm::NickClient::errorText("taken")) &&
                    state->styleSheet().contains("#d93025"),
                "a refusal is reported with its reason, in red");
        require(field->text() == "@taken_name" && ok->isEnabled(),
                "the nickname that was refused is in the field, and can be tried again");
    }
    // Several nickbots: the user chooses which is asked.
    s.nicks().setBots({{bot, "nickbot"}, {second, "second"}});
    {
        bm::NicknameDialog dialog(s, me, "Mine");
        dialog.show();
        const auto [field, ok, problem, state, which] = parts(dialog);
        require(which && which->count() == 2 && which->currentData().toString() == bot &&
                    which->itemData(1).toString() == second && !which->itemIcon(0).isNull() &&
                    which->itemIcon(0).pixmap(16, 16).toImage().pixelColor(8, 8) ==
                        bm::nickbotColor(0),
                "with several nickbots there is a choice, the default first, each in its colour");
        which->setCurrentIndex(1);
        field->setText("@second_one");
        ok->click();
        require(s.nicks().registration(me)["bot"].toString() == second,
                "the registration goes to the nickbot that was chosen");
    }
    {
        bm::NicknameDialog dialog(s, me, "Mine");
        dialog.show();
        require(dialog.findChild<QComboBox *>("nicknameBot")->currentData().toString() == second,
                "the next time the nickbot of the last try is the one offered");
    }
    s.nicks().setBots({{bot, "nickbot"}});
    // What is in the way is said before anything is typed.
    s.nicks().setEnabled(false);
    {
        bm::NicknameDialog dialog(s, me, "Mine");
        dialog.show();
        const auto [field, ok, problem, state, which] = parts(dialog);
        require(!ok->isEnabled() && problem->isVisible() &&
                    problem->text() == bm::NickClient::tr("Nicknames are turned off in Settings"),
                "with nickbots off Register is grey, and the dialog says why");
    }
    s.nicks().setEnabled(true);
    s.nicks().setBots({});
    {
        bm::NicknameDialog dialog(s, me, "Mine");
        dialog.show();
        const auto [field, ok, problem, state, which] = parts(dialog);
        require(!ok->isEnabled() && problem->isVisible() &&
                    problem->text() == bm::NickClient::tr("No nickbot is listed"),
                "with no nickbot listed it says so");
    }
    s.nicks().setBots({{bot, "nickbot"}});
    // What the nickbot client refuses stays in the dialog, in words.
    {
        bm::NicknameDialog dialog(s, chan, "A chan");
        dialog.show();
        const auto [field, ok, problem, state, which] = parts(dialog);
        field->setText("@a_chan");
        require(ok->isEnabled(), "test sanity: the nickname is good");
        ok->click();
        require(dialog.isVisible() && problem->isVisible() &&
                    problem->text() == bm::NickClient::tr("Choose one of your identities"),
                "a chan is no identity: the dialog stays, with the reason");
        dialog.reject();
    }
}
// The icons the identities toolbar has that no button had before: a star, an @, and a dot in a
// nickbot's colour.
static void testIdentityIcons() {
    const auto ink = [](const QIcon &icon) {
        const auto image =
            icon.pixmap(QSize(80, 80)).toImage().convertToFormat(QImage::Format_ARGB32);
        int count = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                count += qAlpha(image.pixel(x, y)) >= 200;
        return count;
    };
    const QColor color("#435867");
    const auto star = bm::materialIcon("star", color), at = bm::materialIcon("nickname", color),
               rename = bm::materialIcon("rename", color);
    require(ink(star) > 250 && ink(at) > 250, "the star and the @ are drawn, not blank squares");
    require(star.pixmap(QSize(80, 80)).toImage() != at.pixmap(QSize(80, 80)).toImage() &&
                star.pixmap(QSize(80, 80)).toImage() != rename.pixmap(QSize(80, 80)).toImage() &&
                at.pixmap(QSize(80, 80)).toImage() != rename.pixmap(QSize(80, 80)).toImage(),
            "and differ from each other and from the buttons beside them");
    const auto dot = bm::dotIcon(QColor("#4a8fd6")).pixmap(16, 16).toImage();
    require(dot.pixelColor(8, 8) == QColor("#4a8fd6") && dot.pixelColor(0, 0).alpha() == 0,
            "a dot is in its colour, with nothing round it");
}
// The Identities page: a table like the address book's, with the buttons for the selected row in
// the toolbar, and the nicknames the nickbots give.
static void testIdentitiesPage(const QTemporaryDir &temp) {
    const auto vaultFile = temp.filePath("idpage.bmvault"),
               boxFile = temp.filePath("idpage.bmmail");
    QString me, work;
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        me = made.addIdentity("Mine");
        work = made.addIdentity("Work");
        made.addChannel("a chan of the identities page test", "A chan");
        const auto key = made.addMailboxKey();
        bm::Mailbox box;
        box.create(boxFile, key, made.mailboxKey(key));
        made.lock();
    }
    QDir().mkpath(temp.filePath("idpage-node"));
    {
        QSettings config(temp.filePath("idpage-node/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultFile);
        config.setValue("mailbox", boxFile);
    }
    const auto bot =
        QString::fromStdString(bmc::Protocol::identity("the identities page's nickbot").address);
    bm::updates::setDefaultNickbotsForTesting({{bot, "nickbot"}});
    struct Restore {
        ~Restore() {
            bm::updates::setDefaultNickbotsForTesting({});
        }
    } restore;
    bm::Session s(temp.filePath("idpage-node"), true);
    s.beginVaultUnlock();
    s.submitVaultPassword("test password");
    require(s.mailboxOpen(), "test sanity: the mailbox opens");
    QString chan;
    for (const auto &v : s.identities())
        if (v.toMap()["chan"].toBool())
            chan = v.toMap()["address"].toString();
    require(!chan.isEmpty(), "test sanity: a chan");
    bm::DesktopWindow window(s, temp.filePath("sounds-idpage"));
    window.show();
    window.findChild<QToolButton *>("folderIcon_Identities")->click();
    QCoreApplication::processEvents();

    auto tree = window.findChild<QTreeView *>("identitiesTree");
    require(window.findChild<QWidget *>("identitiesPane")->isVisible() &&
                !window.findChild<QWidget *>("listColumn")->isVisible() && tree,
            "Identities is a whole-pane page, and its identities are a tree view");
    const auto model = [&] { return tree->model(); };
    const auto cell = [&](int row, int column) { return model()->index(row, column); };
    const auto text = [&](int row, int column) { return cell(row, column).data().toString(); };
    const auto rowOf = [&](const QString &address) {
        for (int row = 0; row < model()->rowCount(); ++row)
            if (text(row, 0) == address)
                return row;
        return -1;
    };
    const auto selected = [&] {
        const auto rows = tree->selectionModel()->selectedRows(0);
        return rows.isEmpty() ? QString() : rows.first().data().toString();
    };
    const auto select = [&](const QString &address) {
        tree->setCurrentIndex(cell(rowOf(address), 0));
    };
    const auto button = [&](const char *name) {
        auto b = window.findChild<QAbstractButton *>(name);
        require(b, name);
        return b;
    };
    const auto isDefault = [&](const QString &address) {
        return cell(rowOf(address), 2).data(bm::DesktopWindow::kIdentityDefaultRole).toBool();
    };
    // A dialog's exec() waits, so a timer answers it: runs f on the modal window or the menu that
    // opens next.
    const auto onNext = [&](const std::function<void(QWidget *)> &f, bool popup = false) {
        auto poll = new QTimer(&window);
        poll->setObjectName("onNextPoll");
        QObject::connect(poll, &QTimer::timeout, &window, [poll, f, popup] {
            if (auto w =
                    popup ? QApplication::activePopupWidget() : QApplication::activeModalWidget()) {
                poll->stop();
                poll->deleteLater();
                f(w);
            }
        });
        poll->start(10);
    };

    // The columns and the rows: each identity by address, with its nickname and local name.
    QStringList columns;
    for (int column = 0; column < model()->columnCount(); ++column)
        columns << model()->headerData(column, Qt::Horizontal).toString();
    require(columns == QStringList({"Address", "Nickname", "Local name"}),
            "the identities have the address book's columns");
    require(model()->rowCount() == 3 && text(0, 0) == me && text(0, 2) == "Mine" &&
                text(1, 0) == work && text(1, 2) == "Work" && text(2, 0) == chan &&
                text(2, 2) == "#A chan",
            "a row per identity and chan, in the vault's order, by address and local name");
    {
        const auto ratio = tree->devicePixelRatioF();
        const auto side = tree->iconSize().width();
        for (int row = 0; row < model()->rowCount(); ++row)
            require(cell(row, 0)
                            .data(Qt::DecorationRole)
                            .value<QIcon>()
                            .pixmap(tree->iconSize(), ratio)
                            .toImage() ==
                        bm::identiconPixmap(text(row, 0), qRound(side * ratio)).toImage(),
                    "each address is shown with its own identicon");
    }
    require(isDefault(me) && !isDefault(work) && !isDefault(chan) &&
                !cell(0, 2).data(bm::DesktopWindow::kIdentityChanRole).toBool() &&
                cell(2, 2).data(bm::DesktopWindow::kIdentityChanRole).toBool(),
            "the local name says which is the default identity and which is a chan");

    // The toolbar is icons, as the address book's: what is for the selected row is in one pill
    // (Nickname, Copy address, QR code, Set as default, Rename, Delete), and Join a channel and
    // New identity are in another.
    const QList<QAbstractButton *> rowButtons{
        button("identityNicknameButton"), button("copyAddressButton"),
        button("showQrButton"),           button("setDefaultIdentityButton"),
        button("renameIdentityButton"),   button("deleteIdentityButton")};
    auto join = button("joinChannelButton"), create = button("newIdentityButton");
    const auto drawn = [](const QIcon &icon) { // something is drawn, not a blank
        const auto image =
            icon.pixmap(QSize(80, 80)).toImage().convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (qAlpha(image.pixel(x, y)) > 128)
                    return true;
        return false;
    };
    for (auto b : rowButtons + QList<QAbstractButton *>{join, create}) {
        auto tool = qobject_cast<QToolButton *>(b);
        require(tool && tool->text().isEmpty() &&
                    tool->toolButtonStyle() == Qt::ToolButtonIconOnly && drawn(tool->icon()) &&
                    !tool->toolTip().isEmpty() && tool->accessibleName() == tool->toolTip(),
                "a button of the identities toolbar is an icon with a tip, which is its name");
    }
    const auto topOf = [&](QWidget *w) { return w->mapTo(&window, QPoint()).y(); };
    for (auto b : rowButtons)
        require(topOf(b) < topOf(tree) && topOf(b) < topOf(create) + create->height() &&
                    topOf(b) + b->height() > topOf(create),
                "a row's buttons sit in the toolbar of New identity, above the table");
    const auto inOnePill = [&](const QList<QAbstractButton *> &buttons) {
        auto pill = buttons.first()->parentWidget();
        QAbstractButton *before = nullptr;
        for (auto b : buttons) {
            if (b->parentWidget() != pill || (before && b->x() <= before->x()))
                return false;
            before = b;
        }
        return pill->grab().toImage().pixelColor(0, pill->height() / 2).alpha() == 255;
    };
    require(inOnePill(rowButtons), "Nickname and the row's other buttons sit in one pill");
    require(inOnePill({join, create}) &&
                join->parentWidget() != button("copyAddressButton")->parentWidget() &&
                button("copyAddressButton")->parentWidget()->mapTo(&window, QPoint()).x() <
                    join->parentWidget()->mapTo(&window, QPoint()).x(),
            "Join a channel and New identity sit in a pill of their own, after the first");
    require(button("identityNicknameButton")->toolTip() == bm::DesktopWindow::tr("Nickname…") &&
                button("copyAddressButton")->toolTip() == "Copy address" &&
                button("showQrButton")->toolTip() == "Show QR code" &&
                button("setDefaultIdentityButton")->toolTip() == "Set as default" &&
                button("renameIdentityButton")->toolTip() == "Rename identity" &&
                button("deleteIdentityButton")->toolTip() == "Delete identity" &&
                join->toolTip() == bm::DesktopWindow::tr("Join a channel…") &&
                create->toolTip() == bm::DesktopWindow::tr("New identity"),
            "the buttons say what they act on");
    require(create->isEnabled() && join->isEnabled(), "New identity and Join are always on");

    // The page opens on the default identity, so that its buttons work at once.
    require(selected() == me, "the default identity is selected when the page opens");
    for (auto b : rowButtons)
        require(b->isEnabled() == (b != button("setDefaultIdentityButton")),
                "with the default selected everything is on but Set as default");
    select(work);
    require(button("setDefaultIdentityButton")->isEnabled(),
            "another identity can be made the default");
    select(chan);
    require(!button("identityNicknameButton")->isEnabled() &&
                button("setDefaultIdentityButton")->isEnabled() &&
                button("renameIdentityButton")->isEnabled(),
            "a chan has no nickname to ask for, and the rest is on");

    // Set as default: the pill moves, the selection stays.
    select(work);
    button("setDefaultIdentityButton")->click();
    QTest::qWait(30);
    require(isDefault(work) && !isDefault(me) && selected() == work &&
                !button("setDefaultIdentityButton")->isEnabled(),
            "Set as default moves the mark and keeps the row selected");
    // Copy.
    QApplication::clipboard()->clear();
    button("copyAddressButton")->click();
    require(QApplication::clipboard()->text() == work, "Copy address copies the selected address");
    QApplication::clipboard()->clear();
    QTest::keyClick(tree, Qt::Key_C, Qt::ControlModifier);
    require(QApplication::clipboard()->text() == work, "so does the copy shortcut");
    // QR.
    {
        bool shown = false;
        onNext([&](QWidget *w) {
            shown = w->objectName() == "qrDialog" && w->isVisible();
            w->close();
        });
        button("showQrButton")->click();
        require(shown, "Show QR code opens the QR dialog");
    }
    // Nickname asks for the selected identity's nickname.
    {
        select(me);
        QString opened, about;
        onNext([&](QWidget *w) {
            opened = w->objectName();
            if (auto address = w->findChild<QLabel *>("nicknameIdentityAddress"))
                about = address->text();
            w->close();
        });
        button("identityNicknameButton")->click();
        require(opened == "nicknameDialog" && about == me,
                "Nickname opens the nickname dialog for the selected identity");
        select(work);
        QTest::qWait(10);
    }
    // The page's own buttons open their dialogs.
    for (const auto &[tool, dialogName] :
         {std::pair{join, "joinChannelDialog"}, std::pair{create, "newIdentityDialog"}}) {
        QString opened;
        onNext([&](QWidget *w) {
            opened = w->objectName();
            w->close();
        });
        tool->click();
        require(opened == dialogName, "Join a channel and New identity open their dialogs");
    }

    // The filter: names, nicknames and addresses.
    auto filter = window.findChild<QLineEdit *>("identitiesFilter");
    auto empty = window.findChild<QLabel *>("identitiesEmpty");
    require(filter && empty && !empty->isVisible() && tree->isVisible(),
            "with identities listed there is no empty note");
    filter->setText("wOrK");
    require(model()->rowCount() == 1 && text(0, 0) == work,
            "the filter matches names, ignoring case");
    filter->setText(me.mid(5, 6));
    require(model()->rowCount() == 1 && text(0, 0) == me, "and addresses");
    filter->setText("nobody-matches");
    require(model()->rowCount() == 0 && empty->isVisible() && !tree->isVisible(),
            "a filter with no match says so");
    for (auto b : rowButtons)
        require(!b->isEnabled(), "a selection the filter hides turns the buttons off");
    filter->clear();
    require(model()->rowCount() == 3 && tree->isVisible() && !empty->isVisible(),
            "clearing it brings the rows back");

    // The nickname column: what stands, what waits, what was refused.
    require(
        text(0, 1).isEmpty() &&
            cell(rowOf(me), 1).data(bm::DesktopWindow::kIdentityHintRole).toString() ==
                bm::DesktopWindow::tr("Register a nickname…") &&
            cell(rowOf(chan), 1).data(bm::DesktopWindow::kIdentityHintRole).toString().isEmpty(),
        "an identity with no nickname offers to get one, and a chan does not");
    s.nicks().adopt(me, "@mine_nick", bot);
    QCoreApplication::processEvents();
    require(text(rowOf(me), 1) == "@mine_nick" &&
                cell(rowOf(me), 1).data(Qt::ForegroundRole).value<QBrush>().color() ==
                    bm::nickbotColor(0) &&
                cell(rowOf(me), 1).data(Qt::ToolTipRole).toString() == "nickbot" &&
                cell(rowOf(me), 1).data(bm::DesktopWindow::kIdentityHintRole).toString().isEmpty(),
            "a nickname that stands shows in the colour of the nickbot that said it, which the "
            "tip names");
    require(s.nicks().registerNickname(work, "@work_nick").isEmpty(), "test sanity: a request");
    QCoreApplication::processEvents();
    const auto muted = tree->palette().color(QPalette::Mid);
    require(text(rowOf(work), 1) == bm::DesktopWindow::tr("%1 (waiting)").arg("@work_nick") &&
                cell(rowOf(work), 1).data(Qt::ForegroundRole).value<QBrush>().color() == muted &&
                cell(rowOf(work), 1)
                    .data(Qt::ToolTipRole)
                    .toString()
                    .contains(bm::NickClient::describe(s.nicks().registration(work))),
            "a registration that waits shows its nickname and says so, muted, with the state as "
            "the tip");
    int serial = 0;
    const auto answer = [&](const QString &identity, const QString &json) {
        s.coreMailbox().storeChat("idpage-answer-" + std::to_string(++serial), bot.toStdString(),
                                  identity.toStdString(), 100 + serial,
                                  "Words.\nnickbot: " + json.toStdString(), bmc::now() + 3600);
        s.nicks().tick();
        QCoreApplication::processEvents();
    };
    answer(work, R"({"v":1,"cmd":"register","ok":false,"error":"taken","query":"@work_nick"})");
    require(text(rowOf(work), 1) == bm::DesktopWindow::tr("%1 (refused)").arg("@work_nick") &&
                cell(rowOf(work), 1).data(Qt::ForegroundRole).value<QBrush>().color() ==
                    QColor("#d93025") &&
                cell(rowOf(work), 1)
                    .data(Qt::ToolTipRole)
                    .toString()
                    .contains(bm::NickClient::errorText("taken")),
            "a refusal shows the nickname that was asked for, says it was refused, in red, with "
            "the reason as the tip");
    require(s.nicks().registerNickname(work, "@work_two").isEmpty(), "test sanity: another try");
    answer(
        work,
        R"({"v":1,"cmd":"register","ok":true,"query":"@work_two","nick":"@work_two","address":")" +
            work + R"("})");
    require(text(rowOf(work), 1) == "@work_two" &&
                cell(rowOf(work), 1).data(Qt::ForegroundRole).value<QBrush>().color() ==
                    bm::nickbotColor(0),
            "an acceptance makes it the nickname, in the nickbot's colour");
    // A change while a nickname stands leaves it, in its colour; the tip tells of the try.
    require(s.nicks().registerNickname(work, "@work_three").isEmpty(), "test sanity: a change");
    QCoreApplication::processEvents();
    require(text(rowOf(work), 1) == "@work_two" &&
                cell(rowOf(work), 1).data(Qt::ForegroundRole).value<QBrush>().color() ==
                    bm::nickbotColor(0) &&
                cell(rowOf(work), 1).data(Qt::ToolTipRole).toString() ==
                    "nickbot\n" + bm::NickClient::describe(s.nicks().registration(work)),
            "a change that waits leaves the nickname that stands, and the tip tells of the try");
    answer(work, R"({"v":1,"cmd":"register","ok":false,"error":"taken","query":"@work_three"})");
    require(text(rowOf(work), 1) == "@work_two" &&
                cell(rowOf(work), 1)
                    .data(Qt::ToolTipRole)
                    .toString()
                    .contains(bm::NickClient::errorText("taken")),
            "and so does a change that was refused");
    // The filter finds nicknames too.
    filter->setText("@WORK_t");
    require(model()->rowCount() == 1 && text(0, 0) == work, "the filter matches nicknames");
    filter->clear();
    s.setNickname(me, ""); // an identity with no nickname to show
    QCoreApplication::processEvents();
    require(cell(rowOf(me), 1).data(bm::DesktopWindow::kIdentityHintRole).toString() ==
                bm::DesktopWindow::tr("Register a nickname…"),
            "an identity whose nickname is gone offers to get one");
    s.nicks().setEnabled(false);
    QCoreApplication::processEvents();
    require(cell(rowOf(chan), 1).data(bm::DesktopWindow::kIdentityHintRole).toString().isEmpty() &&
                cell(rowOf(me), 1).data(bm::DesktopWindow::kIdentityHintRole).toString().isEmpty(),
            "with nickbots off nothing offers a nickname");
    s.nicks().setEnabled(true);

    // Sorting is the address book's: the vault's order until a header is clicked.
    {
        const auto header = tree->header();
        const auto names = [&] {
            QStringList out;
            for (int row = 0; row < model()->rowCount(); ++row)
                out << text(row, 2);
            return out;
        };
        require(names() == QStringList({"Mine", "Work", "#A chan"}),
                "the rows keep the vault's order until a header is clicked");
        QTest::mouseClick(header->viewport(), Qt::LeftButton, {},
                          QPoint(header->sectionViewportPosition(2) + 8, header->height() / 2));
        require(names() == QStringList({"#A chan", "Mine", "Work"}),
                "a click on Local name sorts the rows by name");
        QTest::mouseClick(header->viewport(), Qt::LeftButton, {},
                          QPoint(header->sectionViewportPosition(2) + 8, header->height() / 2));
        require(names() == QStringList({"Work", "Mine", "#A chan"}), "a second click reverses it");
        header->setSortIndicator(-1, Qt::AscendingOrder);
        require(names() == QStringList({"Mine", "Work", "#A chan"}),
                "and the vault's order comes back");
    }

    // Nothing is redrawn when nothing changed: the rows, the selection and the scroll stay.
    {
        select(me);       // not the default: a redraw that forgets the selection would pick that
        QTest::qWait(30); // what is still waiting to be drawn is drawn first
        int removed = 0;
        const auto onRemoved = QObject::connect(model(), &QAbstractItemModel::rowsRemoved, &window,
                                                [&] { ++removed; });
        const auto onReset =
            QObject::connect(model(), &QAbstractItemModel::modelReset, &window, [&] { ++removed; });
        for (int i = 0; i < 3; ++i) {
            emit s.changed();
            QCoreApplication::processEvents();
        }
        require(removed == 0 && selected() == me,
                "a change that is none of the identities' leaves the rows and the selection alone");
        s.nicks().adopt(chan, "#a_chan_nick", bot);
        QCoreApplication::processEvents();
        require(selected() == me && text(rowOf(chan), 1) == "#a_chan_nick",
                "and one that is keeps the selection while the cell changes");
        QObject::disconnect(onRemoved); // the counter is gone with this block
        QObject::disconnect(onReset);
    }

    // A theme switch recolours the toolbar's icons, and the table keeps its selection.
    {
        select(work);
        const auto themeTo = [&](const QString &mode) {
            window.findChild<QAction *>("settingsAction")->trigger();
            auto combo =
                window.findChild<QWidget *>("settingsPane")->findChild<QComboBox *>("themeCombo");
            combo->setCurrentIndex(combo->findData(mode));
            window.findChild<QToolButton *>("folderIcon_Identities")->click();
            QCoreApplication::processEvents();
        };
        window.findChild<QAction *>("settingsAction")->trigger();
        const auto before = window.findChild<QWidget *>("settingsPane")
                                ->findChild<QComboBox *>("themeCombo")
                                ->currentData()
                                .toString();
        const QList<QAbstractButton *> toolbar =
            rowButtons + QList<QAbstractButton *>{join, create};
        themeTo("light");
        QList<QImage> light;
        for (auto b : toolbar)
            light << b->icon().pixmap(18, 18).toImage();
        const auto rowPill = button("copyAddressButton")->parentWidget()->styleSheet(),
                   toolsPill = join->parentWidget()->styleSheet();
        themeTo("dark");
        for (int i = 0; i < toolbar.size(); ++i)
            require(toolbar[i]->icon().pixmap(18, 18).toImage() != light[i],
                    "a theme switch recolours the identities toolbar's icons");
        require(button("copyAddressButton")->parentWidget()->styleSheet() != rowPill &&
                    join->parentWidget()->styleSheet() != toolsPill,
                "and both pills' borders");
        require(selected() == work && model()->rowCount() == 3,
                "and the table keeps its rows and its selection");
        themeTo(before);
    }

    // Double-click does what the cell is about.
    const auto doubleClick = [&](const QString &address, int column) {
        const auto rect = tree->visualRect(cell(rowOf(address), column));
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, rect.center()); // the first click
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, {}, rect.center());
    };
    {
        QApplication::clipboard()->clear();
        doubleClick(me, 0);
        require(QApplication::clipboard()->text() == me, "a double-click on an address copies it");
        QString opened;
        onNext([&](QWidget *w) {
            opened = w->objectName() + "|" +
                     (w->findChild<QLabel *>("nicknameIdentityAddress")
                          ? w->findChild<QLabel *>("nicknameIdentityAddress")->text()
                          : QString());
            static_cast<QDialog *>(w)->reject();
        });
        doubleClick(work, 1);
        require(opened == "nicknameDialog|" + work, "a double-click on a nickname asks for one");
        onNext([&](QWidget *w) {
            auto input = qobject_cast<QInputDialog *>(w);
            require(input, "a double-click on a local name opens the rename prompt");
            require(input->textValue() == "Work", "which starts from the name there is");
            input->setTextValue("Work renamed");
            input->accept();
        });
        doubleClick(work, 2);
        QTest::qWait(30);
        require(text(rowOf(work), 2) == "Work renamed" && selected() == work,
                "and the new name shows at once, the row still selected");
        // Not on a chan's nickname: there is nothing to ask for.
        bool asked = false;
        onNext([&](QWidget *w) {
            asked = true;
            static_cast<QDialog *>(w)->reject();
        });
        doubleClick(chan, 1);
        QTest::qWait(60);
        require(!asked, "a chan's nickname is not asked for");
        for (auto poll : window.findChildren<QTimer *>("onNextPoll")) // the one that never fired
            delete poll;
    }

    // The menu of a row holds what the toolbar does.
    {
        QStringList items;
        QList<bool> on;
        onNext(
            [&](QWidget *w) {
                for (auto action : w->actions())
                    if (!action->isSeparator()) {
                        items << action->text();
                        on << action->isEnabled();
                    }
                w->close();
            },
            true);
        select(me);
        emit tree->customContextMenuRequested(tree->visualRect(cell(rowOf(chan), 0)).center());
        require(items == QStringList({"Copy address", "Show QR code", "Set as default", "Nickname…",
                                      "Rename identity", "Delete identity"}),
                "the menu of a row has the toolbar's actions, in words");
        require(selected() == chan, "and a right-click selects its row first");
        require(on == QList<bool>({true, true, true, false, true, true}),
                "a chan's menu has no nickname to ask for");
    }

    // A new identity is selected, as is one that is joined: the page shows what was added.
    {
        const auto before = QSet<QString>{me, work, chan};
        onNext([&](QWidget *w) { w->findChild<QPushButton *>("newVaultCreateButton")->click(); });
        create->click();
        QTest::qWait(50);
        require(model()->rowCount() == 4, "New identity adds its row");
        const auto added = selected();
        require(!added.isEmpty() && !before.contains(added),
                "and selects it, so its buttons are ready");
        // Deleting it takes the row away, and the selection goes to the default.
        onNext(
            [&](QWidget *w) { qobject_cast<QMessageBox *>(w)->button(QMessageBox::Yes)->click(); });
        button("deleteIdentityButton")->click();
        QTest::qWait(50);
        require(model()->rowCount() == 3 && rowOf(added) < 0 && selected() == work,
                "Delete takes its row away and selects the default");
    }
}
// An address in a letter's header is on one line when the room is there, and wrapped, within the
// room, only when it is not. A plain wrapping QLabel reports a broken-up text as its size hint, so
// a layout with room to spare cut the address after "BM-"; and a width of the text's rounded
// advance leaves the label a fraction of a pixel short, with the same result, at some sizes of the
// font.
static void testAddressLabel() {
    // One that can be broken after the "BM-" (a letter follows it; before a digit the text cannot
    // be broken at all), which is the one that was cut. Its length is such that the width of a font
    // of 0.6 em, the usual one for a fixed pitch, has a fraction of a pixel at most sizes.
    const QString address = "BM-NBSeFsd41rbt2emv1HBTMwQagLtdSsYRx";
    QWidget host;
    auto row = new QHBoxLayout(&host);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto label = new bm::AddressLabel(address);
    label->setFont(bm::addressFont());
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    row->addWidget(label, 0, Qt::AlignTop); // not stretched to the height of the row
    row->addStretch();                      // as in the header: a button, then what is left
    // What the text needs on one line, as Qt itself measures it: a label that does not wrap.
    QLabel oneLine(address);
    oneLine.setTextFormat(Qt::PlainText);
    host.resize(2000, 300);
    host.show();
    for (int pixels = 9; pixels <= 24; ++pixels) {
        auto font = bm::addressFont();
        font.setPixelSize(pixels);
        label->setFont(font);
        oneLine.setFont(font);
        QCoreApplication::processEvents();
        const QSize single = oneLine.sizeHint();
        require(
            label->width() >= single.width() &&
                label->heightForWidth(label->width()) <= single.height(),
            qPrintable(
                QString("an address with the room it needs is on one line, at %1 px").arg(pixels)));
        require(label->sizeHint().width() >= single.width() &&
                    label->sizeHint().height() == single.height(),
                qPrintable(QString("and asks for that line, no less, at %1 px").arg(pixels)));
    }
    auto font = bm::addressFont();
    font.setPixelSize(13);
    label->setFont(font);
    oneLine.setFont(font);
    QCoreApplication::processEvents(); // the window's least width follows the font before it is
                                       // narrowed
    const QSize single = oneLine.sizeHint();
    host.resize(single.width() - 8, 300);
    QCoreApplication::processEvents();
    require(label->width() < single.width() && label->width() <= host.width() &&
                label->height() > single.height(),
            "and is wrapped, within the room, when the room is not there");
}
// The subtitle of a page gives way to the buttons beside it, and says so with "…", where a plain
// label kept its width and was cut off mid-word at the least width of the window.
static void testEllipsisLabel() {
    const QString text = "People you write to. Names are private to this mailbox.";
    QWidget host;
    auto row = new QHBoxLayout(&host);
    row->setContentsMargins(0, 0, 0, 0);
    auto label = new bm::EllipsisLabel(text);
    auto button = new QPushButton("A button that wants its room");
    row->addWidget(label);
    row->addStretch();
    row->addWidget(button);
    host.resize(2000, 100);
    host.show();
    QCoreApplication::processEvents();
    require(label->width() >= label->sizeHint().width() && label->shownText() == text,
            "with the room there is, the whole text is shown");
    host.resize(label->sizeHint().width() / 2 + button->sizeHint().width(), 100);
    QCoreApplication::processEvents();
    require(host.width() < label->sizeHint().width() + button->sizeHint().width() &&
                label->width() < label->sizeHint().width() && label->shownText().endsWith("…") &&
                label->shownText().size() < text.size() &&
                button->width() >= button->sizeHint().width(),
            "with less the label ends in an ellipsis, and the button keeps its room");
}
// The letter's header: the sender's address whole on one line beside its name and the button, and a
// name that does not come from the address book saying where it does.
static void testReaderHeader(const QTemporaryDir &temp) {
    const auto vaultFile = temp.filePath("header.bmvault"),
               boxFile = temp.filePath("header.bmmail");
    // An address with two zero bytes at the front of its RIPE, like the shorter ones PyBitmessage
    // makes on request: a letter follows its "BM-", where the text can be broken (before a digit it
    // cannot), and where a sender's address was cut.
    ntb_address made{};
    made.version = 3;
    made.stream = 1;
    for (int i = 2; i < 20; ++i)
        made.ripe[i] = uint8_t(0x30 + i);
    char encoded[NTB_ADDRESS_MAX_LENGTH + 1];
    ntb_address_encode(&made, encoded);
    const QString sender = encoded;
    require(sender.at(3).isLetter() && bmc::Wire::validAddress(sender.toStdString()),
            "test sanity: a valid address with a letter after BM-");
    // The built-in notices of ynotbit come from an address that is named, and no subscription names
    // it.
    const auto publisher =
        QString::fromStdString(bmc::Protocol::identity("the header test's publisher").address);
    const auto publisherBefore = bm::updates::publisherAddress();
    bm::updates::setPublisherAddressForTesting(publisher);
    struct Restore {
        QString was;
        ~Restore() {
            bm::updates::setPublisherAddressForTesting(was);
        }
    } restore{publisherBefore};
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        const auto me = made.addIdentity("Mine");
        const auto key = made.addMailboxKey();
        bm::Mailbox box;
        box.create(boxFile, key, made.mailboxKey(key));
        // The senders of the Inbox were put in the address book when the mailbox was first opened;
        // this one is a later one.
        box.setSetting("migrated:inbox-senders", "1");
        box.store("header-1", sender, me, "A letter", "Body", 1800, "Inbox");
        box.subscribe(sender, "MiNode-Refined-updates");
        box.store("header-2", publisher, me, "Release notes", "Body", 1801, "Inbox");
        made.lock();
    }
    QDir().mkpath(temp.filePath("header-node"));
    {
        QSettings config(temp.filePath("header-node/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultFile);
        config.setValue("mailbox", boxFile);
    }
    const auto bot =
        QString::fromStdString(bmc::Protocol::identity("the header test's nickbot").address);
    bm::updates::setDefaultNickbotsForTesting({{bot, "nickbot"}});
    struct RestoreBots {
        ~RestoreBots() {
            bm::updates::setDefaultNickbotsForTesting({});
        }
    } restoreBots;
    bm::Session s(temp.filePath("header-node"), true);
    s.beginVaultUnlock();
    s.submitVaultPassword("test password");
    require(s.mailboxOpen(), "test sanity: the mailbox opens");
    bm::DesktopWindow window(s, temp.filePath("sounds-header"));
    window.resize(1160, 780);
    window.show();
    window.findChild<QToolButton *>("folderIcon_Inbox")->click();
    window.selectMessage("header-1");
    QCoreApplication::processEvents();
    auto address = window.findChild<QLabel *>("messageAddresses");
    auto name = window.findChild<QLabel *>("fromName");
    auto add = window.findChild<QToolButton *>("addSenderContact");
    require(address && address->isVisible() && address->text() == sender && name->isVisible() &&
                name->text().contains("MiNode-Refined-updates") && add->isVisible(),
            "test sanity: a sender named by a subscription, who is not in the address book");
    {
        QLabel oneLine(address->text()); // the text on one line, as Qt measures it
        oneLine.setFont(address->font());
        oneLine.setTextFormat(Qt::PlainText);
        require(address->width() >= oneLine.sizeHint().width() &&
                    address->heightForWidth(address->width()) <= oneLine.sizeHint().height(),
                "the sender's address is on one line when the pane has the room for it");
    }
    const auto at = [&](QWidget *w) { return w->mapTo(&window, QPoint()); };
    require(at(add).x() - (at(address).x() + address->width()) <= 8 &&
                at(add).y() < at(address).y() + address->height(),
            "and the add button is beside the end of the address, not far off");
    require(name->toolTip() ==
                bm::DesktopWindow::tr("Named by your subscription; not in your contacts"),
            "the name says it comes from a subscription, which is why the button is there");
    require(s.addContact(sender, "Minode"), "test sanity: the sender joins the address book");
    QCoreApplication::processEvents();
    require(!add->isVisible() && name->text().contains("Minode") && name->toolTip().isEmpty(),
            "once a contact the button is gone and the name needs no explanation");
    // A nickname a nickbot gave goes beside the name, and the tip names the nickbot.
    s.nicks().adopt(sender, "@minode", bot);
    QCoreApplication::processEvents();
    require(name->text().contains("@minode") && name->toolTip() == "nickbot",
            "the nickbot that gave the nickname is named by the tip");
    // A name from the built-in notices is not a subscription's: the button is there and says
    // nothing.
    window.selectMessage("header-2");
    QCoreApplication::processEvents();
    require(name->isVisible() && name->text().contains(bm::Session::tr("ynotbit updates")) &&
                add->isVisible() && name->toolTip().isEmpty(),
            "a name that no subscription gave has no such tip");
}
// Contacts > Find people: the question asked from what is typed, the answer in the table, and what
// is added to the address book.
static void testFindPeople(const QTemporaryDir &temp) {
    const auto vaultFile = temp.filePath("find.bmvault"), boxFile = temp.filePath("find.bmmail");
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        made.addIdentity("Mine");
        const auto key = made.addMailboxKey();
        bm::Mailbox box;
        box.create(boxFile, key, made.mailboxKey(key));
        made.lock();
    }
    QDir().mkpath(temp.filePath("find-node"));
    {
        QSettings config(temp.filePath("find-node/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultFile);
        config.setValue("mailbox", boxFile);
    }
    const auto addressOf = [](const char *who) {
        return QString::fromStdString(bmc::Protocol::identity(who).address);
    };
    const auto bot = addressOf("the finder's nickbot"), ben = addressOf("Ben found"),
               benny = addressOf("Benny found");
    bm::updates::setDefaultNickbotsForTesting({{bot, "nickbot"}});
    struct Restore {
        ~Restore() {
            bm::updates::setDefaultNickbotsForTesting({});
        }
    } restore;
    bm::Session s(temp.filePath("find-node"), true);
    s.beginVaultUnlock();
    s.submitVaultPassword("test password");
    require(s.mailboxOpen(), "test sanity: the mailbox opens");
    auto &nick = s.nicks();
    qint64 now = QDateTime::currentSecsSinceEpoch();
    nick.setClockForTesting([&] { return now; });
    nick.setDelayForTesting([](int low, int) { return low; });
    int serial = 0;
    const auto answer = [&](const QString &json) {
        const auto text = "Words.\nnickbot: " + json.toStdString();
        s.coreMailbox().store("find-answer-" + std::to_string(++serial), bot.toStdString(),
                              nick.laneAddress().toStdString(), "", bmc::chatMessageBody(1, text),
                              -1, "Service");
        nick.tick();
    };
    const auto sent = [&] {
        QStringList texts;
        for (const auto &m :
             s.coreMailbox().chatMessages(nick.laneAddress().toStdString(), bot.toStdString()))
            texts << QString::fromStdString(m.body);
        return texts;
    };
    bm::FindPeopleDialog dialog(s);
    dialog.show();
    QCoreApplication::processEvents();
    auto field = dialog.findChild<QLineEdit *>("findPeopleField");
    auto search = dialog.findChild<QPushButton *>("findPeopleSearchButton");
    auto add = dialog.findChild<QPushButton *>("findPeopleAddButton");
    auto results = dialog.findChild<QTreeWidget *>("findPeopleResults");
    auto status = dialog.findChild<QLabel *>("findPeopleStatus");
    require(!search->isEnabled() && !add->isEnabled(), "nothing to ask, nothing to add");
    field->setText("ben");
    require(search->isEnabled(), "something typed can be asked");
    search->click();
    require(results->topLevelItemCount() == 0 && !status->text().isEmpty() && sent().isEmpty(),
            "three letters are too few: it says so and asks nothing");

    // A word is searched for.
    field->setText("benn");
    search->click();
    now += 4;
    nick.tick();
    require(sent() == QStringList{"/search benn"} && status->text().contains("nickbot"),
            "a word is searched for, and the dialog says whom it asked");
    answer(
        R"({"v":1,"cmd":"search","ok":true,"query":"benn","results":[{"nick":"@benny","address":")" +
        benny + R"("},{"nick":"@bennett","address":")" + ben + R"("}],"more":false})");
    require(results->topLevelItemCount() == 2 && results->topLevelItem(0)->text(0) == "@benny" &&
                results->topLevelItem(0)->text(1) == benny &&
                results->topLevelItem(0)->foreground(0).color() == bm::nickbotColor(0),
            "the hits fill the table, in the nickbot's colour");
    require(!add->isEnabled(), "nothing is added until something is chosen");
    results->topLevelItem(0)->setSelected(true);
    require(add->isEnabled(), "a chosen row can be added");
    add->click();
    require(s.isContact(benny) && s.nickname(benny) == "@benny" &&
                nick.nicknameOf(benny).color == 0 && !s.isContact(ben) &&
                results->topLevelItem(0)->text(2) == bm::FindPeopleDialog::tr("yes"),
            "it joins the address book under its nickname, which is kept as the nickbot's");
    bool named = false;
    for (const auto &c : s.contacts())
        named = named || (c.toMap()["address"].toString() == benny &&
                          c.toMap()["label"].toString() == "@benny");
    require(named, "and the contact is called by it");

    // A whole nickname is asked for its address, an address for its nicknames.
    field->setText("@Ben");
    search->click();
    now += 4;
    nick.tick();
    require(sent().last() == "/address @Ben", "a nickname is asked for its address");
    answer(
        R"({"v":1,"cmd":"address","ok":true,"query":"@Ben","found":true,"nick":"@Ben","address":")" +
        ben + R"("})");
    require(results->topLevelItemCount() == 1 && results->topLevelItem(0)->text(1) == ben &&
                results->currentItem() == results->topLevelItem(0),
            "the one it found is chosen already");
    field->setText(benny);
    search->click();
    now += 4;
    nick.tick();
    require(sent().last() == "/nickname " + benny, "an address is asked for its nicknames");
    answer(R"({"v":1,"cmd":"nickname","ok":true,"query":")" + benny +
           R"(","results":[{"address":")" + benny + R"(","nicks":["@benny","/bennybot"]}]})");
    require(results->topLevelItemCount() == 2 && results->topLevelItem(1)->text(0) == "/bennybot",
            "one row for each of its nicknames");
    // Nobody by that name.
    field->setText("@nobody");
    search->click();
    now += 4;
    nick.tick();
    answer(R"({"v":1,"cmd":"address","ok":true,"query":"@nobody","found":false})");
    require(results->topLevelItemCount() == 0 && status->text().contains("nickbot"),
            "an empty answer says that the nickbot knows no one by that");
    // An answer to somebody else's question, of the same kind, does not fill the table.
    require(nick.lookupAddress("@other").isEmpty(), "test sanity: another asker's question");
    field->setText("@later");
    search->click();
    now += 4;
    nick.tick();
    answer(
        R"({"v":1,"cmd":"address","ok":true,"query":"@other","found":true,"nick":"@other","address":")" +
        ben + R"("})");
    require(results->topLevelItemCount() == 0,
            "an answer to another question is not shown as this one's");
    answer(
        R"({"v":1,"cmd":"address","ok":true,"query":"@later","found":true,"nick":"@later","address":")" +
        benny + R"("})");
    require(results->topLevelItemCount() == 1 && results->topLevelItem(0)->text(0) == "@later",
            "the answer to this one is");
    // Nickbots off: it says so.
    nick.setEnabled(false);
    field->setText("@ben");
    search->click();
    require(status->text() == bm::NickClient::tr("Nicknames are turned off in Settings"),
            "with nickbots off the dialog says why nothing is asked");
    nick.setEnabled(true);
}
// Contacts > Check nicknames: the whole address book is asked about when the user says so, the page
// says that it is under way and when it is over, and the answers fill the Nickname column.
static void testCheckNicknames(const QTemporaryDir &temp) {
    const auto vaultFile = temp.filePath("check.bmvault"), boxFile = temp.filePath("check.bmmail");
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        made.addIdentity("Mine");
        const auto key = made.addMailboxKey();
        bm::Mailbox box;
        box.create(boxFile, key, made.mailboxKey(key));
        made.lock();
    }
    QDir().mkpath(temp.filePath("check-node"));
    {
        QSettings config(temp.filePath("check-node/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultFile);
        config.setValue("mailbox", boxFile);
    }
    const auto addressOf = [](const std::string &who) {
        return QString::fromStdString(bmc::Protocol::identity(who).address);
    };
    const auto bot = addressOf("the check test's nickbot");
    bm::updates::setDefaultNickbotsForTesting({{bot, "nickbot"}});
    struct Restore {
        ~Restore() {
            bm::updates::setDefaultNickbotsForTesting({});
        }
    } restore;
    bm::Session s(temp.filePath("check-node"), true);
    s.beginVaultUnlock();
    s.submitVaultPassword("test password");
    require(s.mailboxOpen(), "test sanity: the mailbox opens");
    qint64 now = QDateTime::currentSecsSinceEpoch();
    s.nicks().setClockForTesting([&] { return now; });
    s.nicks().setDelayForTesting([](int low, int) { return low; });
    // An address book with nobody in it (the built-in contacts too).
    for (const auto &c : s.coreMailbox().contacts())
        s.removeContact(QString::fromStdString(c.address));
    require(s.coreMailbox().contacts().empty(), "test sanity: an empty address book");
    bm::DesktopWindow window(s, temp.filePath("sounds-check"));
    window.resize(1160, 780);
    window.show();
    window.findChild<QToolButton *>("folderIcon_Contacts")->click();
    QCoreApplication::processEvents();
    auto check = window.findChild<QToolButton *>("checkNicknamesButton");
    auto note = window.findChild<QLabel *>("contactsCheckNote");
    auto tree = window.findChild<QTreeView *>("contactsTree");
    require(check && note && tree && check->isEnabled() && !note->isVisible(),
            "the address book has a button that checks nicknames, and nothing is said yet");
    require(check->text().isEmpty() &&
                check->accessibleName() == bm::DesktopWindow::tr("Check nicknames") &&
                check->toolTip().contains(bm::DesktopWindow::tr("Check nicknames")) &&
                check->toolTip().contains(bm::DesktopWindow::tr(
                    "Ask the nickbot for the nicknames of all your contacts")),
            "which is an icon whose tip says what it does");
    // Nobody to ask about: it says so.
    check->click();
    require(note->isVisible() &&
                note->text() == bm::NickClient::tr("There are no contacts to check") &&
                note->styleSheet().contains("#d93025") && check->isEnabled(),
            "with an empty address book the page says there is nothing to check");
    QStringList contacts;
    for (int i = 0; i < 10; ++i) {
        contacts << addressOf("a contact of the check test " + std::to_string(i));
        require(s.addContact(contacts.last(), "Contact " + QString::number(i)),
                "test sanity: a contact");
    }
    const auto sent = [&] {
        int count = 0;
        for (const auto &m :
             s.coreMailbox().chatMessages(s.nicks().laneAddress().toStdString(), bot.toStdString()))
            count += m.outgoing;
        return count;
    };

    const auto at = [&](QWidget *w) { return w->mapTo(&window, QPoint()); };
    auto addContact = window.findChild<QToolButton *>("addContactButton");
    require(at(check).y() < at(tree).y() &&
                at(check).y() < at(addContact).y() + addContact->height() &&
                at(check).y() + check->height() > at(addContact).y(),
            "it sits in the toolbar of Add contact, above the table");

    // What is in the way is said on the page, in red, and nothing is sent.
    s.nicks().setBots({});
    check->click();
    require(note->isVisible() && note->text() == bm::NickClient::tr("No nickbot is listed") &&
                note->styleSheet().contains("#d93025") && check->isEnabled() &&
                !s.nicks().checking(),
            "with no nickbot listed the page says so, and the button stays on");
    s.nicks().setBots({{bot, "nickbot"}});

    // A check: under way, said so, one button press at a time.
    check->click();
    require(s.nicks().checking() && !check->isEnabled() && note->isVisible() &&
                note->text() ==
                    bm::DesktopWindow::tr("Asking the nickbot for the nicknames of your contacts. "
                                          "The answers take a few minutes.") &&
                !note->styleSheet().contains("#d93025"),
            "a check is under way: the page says so, and the button waits");
    now += 3;
    s.nicks().tick();
    require(sent() == 1, "the first message goes after its random wait");
    now += 10;
    s.nicks().tick();
    require(sent() == 2, "and the second ten seconds after");
    // The answers: the first message's first contact gets a nickname.
    const auto answer = [&](const QString &json, int number) {
        const auto text = "Words.\nnickbot: " + json.toStdString();
        s.coreMailbox().store("check-answer-" + std::to_string(number), bot.toStdString(),
                              s.nicks().laneAddress().toStdString(), "",
                              bmc::chatMessageBody(1, text), -1, "Service");
    };
    QStringList messages;
    for (const auto &m :
         s.coreMailbox().chatMessages(s.nicks().laneAddress().toStdString(), bot.toStdString()))
        if (m.outgoing)
            messages << QString::fromStdString(m.body);
    // One of the contacts of this test, which the first message asks about, has a nickname.
    QString firstAsked;
    for (const auto &a : messages[0].mid(10).split(' '))
        if (firstAsked.isEmpty() && contacts.contains(a))
            firstAsked = a;
    require(!firstAsked.isEmpty(), "test sanity: the first message asks about one of them");
    for (int i = 0; i < messages.size(); ++i) {
        const auto addresses = messages[i].mid(10).split(' ');
        QStringList entries;
        for (const auto &a : addresses)
            entries << R"({"address":")" + a + R"(","nicks":[)" +
                           (a == firstAsked ? "\"@page_check\"" : QString()) + "]}";
        answer(R"({"v":1,"cmd":"nickname","ok":true,"results":[)" + entries.join(',') + "]}", i);
    }
    s.nicks().tick();
    QCoreApplication::processEvents();
    require(!s.nicks().checking() && check->isEnabled() &&
                note->text() ==
                    bm::DesktopWindow::tr("Checked your contacts. The nicknames the nickbot has "
                                          "are in the table.") &&
                !note->styleSheet().contains("#d93025"),
            "with the last answer the check is over: the page says so, and the button is on again");
    bool shown = false;
    for (int row = 0; row < tree->model()->rowCount(); ++row)
        if (tree->model()->index(row, 0).data().toString() == firstAsked)
            shown = tree->model()->index(row, 1).data().toString() == "@page_check" &&
                    tree->model()->index(row, 1).data(Qt::ForegroundRole).value<QBrush>().color() ==
                        bm::nickbotColor(0);
    require(shown,
            "and the nickname that was found is in the Nickname column, in the nickbot's colour");
    // What the page says of a check does not outlive its mailbox.
    s.closeMailbox();
    QCoreApplication::processEvents();
    require(note->text().isEmpty() && check->isEnabled(),
            "with the mailbox closed the page keeps nothing of the check");
}
// The New identity dialog asks the nickbot for a nickname together with the identity.
// The address book is where a conversation with a contact starts. A letter is for people; a bot (a
// contact whose name or nickname starts with "/") answers in a chat, so Write opens the chat for
// it, and every contact has a Chat button too. The toolbar is icons, each with a tip.
static void testContactChats(const QTemporaryDir &temp) {
    const auto vaultFile = temp.filePath("cchat.bmvault"), boxFile = temp.filePath("cchat.bmmail");
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        made.addIdentity("Mine");
        const auto key = made.addMailboxKey();
        bm::Mailbox box;
        box.create(boxFile, key, made.mailboxKey(key));
        made.lock();
    }
    QDir().mkpath(temp.filePath("cchat-node"));
    {
        QSettings config(temp.filePath("cchat-node/desktop.ini"), QSettings::IniFormat);
        config.setValue("vault", vaultFile);
        config.setValue("mailbox", boxFile);
    }
    const auto addressOf = [](const std::string &who) {
        return QString::fromStdString(bmc::Protocol::identity(who).address);
    };
    bm::Session s(temp.filePath("cchat-node"), true);
    s.beginVaultUnlock();
    s.submitVaultPassword("test password");
    require(s.mailboxOpen(), "test sanity: the mailbox opens");
    for (const auto &c : s.coreMailbox().contacts())
        s.removeContact(QString::fromStdString(c.address));
    QString mine;
    for (const auto &v : s.identities())
        if (!v.toMap()["chan"].toBool())
            mine = v.toMap()["address"].toString();
    require(!mine.isEmpty(), "test sanity: an identity");
    const auto person = addressOf("the chat test's person"),
               echo = addressOf("the chat test's echo"),
               helper = addressOf("the chat test's helper"),
               veteran = addressOf("a contact with a chat"),
               newcomer = addressOf("a contact without one"),
               quiet = addressOf("a contact to cancel");
    require(s.addContact(person, "Pat") && s.addContact(echo, "Echo") &&
                s.addContact(helper, "/helper") && s.addContact(veteran, "Vic") &&
                s.addContact(newcomer, "Nia") && s.addContact(quiet, "Quin"),
            "test sanity: six contacts");
    s.setNickname(person, "@pat");
    s.setNickname(echo, "/echobot"); // a bot by its nickname; helper is one by its local name

    bm::DesktopWindow window(s, temp.filePath("sounds-cchat"));
    window.resize(1160, 780);
    window.show();
    const auto toContacts = [&] {
        window.findChild<QToolButton *>("folderIcon_Contacts")->click();
        QCoreApplication::processEvents();
    };
    toContacts();
    auto tree = window.findChild<QTreeView *>("contactsTree");
    auto pane = window.findChild<bm::ChatsPane *>("chatsPane");
    require(tree && pane, "the address book and the chats page exist");
    const auto toolButton = [&](const char *name) {
        auto b = window.findChild<QToolButton *>(name);
        require(b != nullptr, "the toolbar has the button");
        return b;
    };
    auto write = toolButton("writeToContactButton"), chat = toolButton("chatWithContactButton"),
         copy = toolButton("copyContactAddressButton"), rename = toolButton("renameContactButton"),
         remove = toolButton("deleteContactButton"), find = toolButton("findPeopleButton"),
         check = toolButton("checkNicknamesButton"), add = toolButton("addContactButton");

    // Icons, not words: every button is an icon with a tip, and a name for a screen reader.
    const auto drawn = [](const QIcon &icon) { // something is drawn, not a blank
        const auto image =
            icon.pixmap(QSize(80, 80)).toImage().convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (qAlpha(image.pixel(x, y)) > 128)
                    return true;
        return false;
    };
    for (auto b : {write, chat, copy, rename, remove, find, check, add}) {
        require(drawn(b->icon()), "each button's icon is drawn");
        require(b->text().isEmpty() && !b->icon().isNull() &&
                    b->toolButtonStyle() == Qt::ToolButtonIconOnly && !b->toolTip().isEmpty() &&
                    !b->accessibleName().isEmpty(),
                "a button of the address book's toolbar is an icon with a tip and a name");
    }
    require(find->toolTip() == bm::DesktopWindow::tr("Find people…") &&
                add->toolTip() == bm::DesktopWindow::tr("Add contact…") &&
                chat->toolTip() == bm::DesktopWindow::tr("Chat") &&
                check->toolTip().contains(bm::DesktopWindow::tr("Check nicknames")) &&
                check->toolTip().contains(bm::DesktopWindow::tr(
                    "Ask the nickbot for the nicknames of all your contacts")),
            "the tips say what the buttons do");
    // Two pills: what is for the selected contact, then what is for the address book.
    require(write->parentWidget() == chat->parentWidget() &&
                chat->parentWidget() == copy->parentWidget() &&
                copy->parentWidget() == rename->parentWidget() &&
                rename->parentWidget() == remove->parentWidget() &&
                find->parentWidget() == check->parentWidget() &&
                check->parentWidget() == add->parentWidget() &&
                write->parentWidget() != find->parentWidget(),
            "a contact's buttons are in one pill and the address book's in another");
    require(write->x() < chat->x() && chat->x() < copy->x() && copy->x() < rename->x() &&
                rename->x() < remove->x() &&
                write->parentWidget()->mapTo(&window, QPoint()).x() <
                    find->parentWidget()->mapTo(&window, QPoint()).x() &&
                find->x() < check->x() && check->x() < add->x(),
            "left to right: Write, Chat, Copy, Rename, Delete, then Find, Check, Add");
    require(!write->isEnabled() && !chat->isEnabled() && find->isEnabled() && check->isEnabled() &&
                add->isEnabled(),
            "with no contact selected Write and Chat are off, the address book's buttons on");

    const auto select = [&](const QString &address) {
        for (int row = 0; row < tree->model()->rowCount(); ++row) {
            const auto cell = tree->model()->index(row, 0);
            if (cell.data().toString() == address) {
                tree->setCurrentIndex(cell);
                QCoreApplication::processEvents();
                return cell;
            }
        }
        require(false, "test sanity: the contact is listed");
        return QModelIndex();
    };
    // The composer, if one opened, and who it is to; closed again.
    const auto composerTo = [&]() -> QString {
        auto dialog = window.findChild<QDialog *>("composer");
        if (!dialog)
            return {};
        const auto to = dialog->findChild<QLineEdit *>("recipientField")->text();
        dialog->reject();
        waitComposerGone(window);
        return to;
    };
    // The dialog that asks which identity, answered from a timer (it is modal).
    int asked = 0;
    QString askedAbout;
    bool askedWithChoice = false;
    std::function<void(bm::NewChatDialog &)> answer;
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, [&] {
        for (auto dialog : window.findChildren<bm::NewChatDialog *>())
            if (dialog->isVisible()) {
                ++asked;
                askedAbout = dialog->peer();
                auto box = dialog->findChild<QComboBox *>("chatIdentityBox");
                askedWithChoice = box && box->isVisibleTo(dialog) && box->count() > 1;
                if (answer)
                    answer(*dialog);
                else
                    dialog->reject();
            }
    });
    driver.start(10);

    // A person: Write is a letter and Chat is a chat, from the one identity, without a question.
    select(person);
    require(write->isEnabled() && chat->isEnabled() && copy->isEnabled(),
            "selecting a contact turns Write and Chat on");
    require(write->toolTip() == bm::DesktopWindow::tr("Write a letter"),
            "for a person Write says it writes a letter");
    write->click();
    require(composerTo() == person && !pane->isVisible(),
            "Write to a person opens a letter to them, and not a chat");
    chat->click();
    QCoreApplication::processEvents();
    require(pane->isVisible() && pane->contact() == person && pane->identity() == mine &&
                asked == 0 && window.findChild<QDialog *>("composer") == nullptr,
            "Chat opens a chat with the person from the only identity, asking nothing");

    // A bot by its nickname: Write is for a chat, because a bot answers there.
    toContacts();
    select(echo);
    require(write->toolTip() == bm::DesktopWindow::tr("Chat with this bot") &&
                write->accessibleName() == write->toolTip() &&
                chat->toolTip() == bm::DesktopWindow::tr("Chat"),
            "for a bot Write says it chats, to the eye and to a screen reader");
    write->click();
    QCoreApplication::processEvents();
    require(composerTo().isEmpty() && pane->isVisible() && pane->contact() == echo &&
                pane->identity() == mine,
            "Write to a bot opens the chat with it, not a letter");
    // A bot by the name the user gave it.
    toContacts();
    select(helper);
    require(write->toolTip() == bm::DesktopWindow::tr("Chat with this bot"),
            "a contact whose local name starts with / is a bot too");
    write->click();
    QCoreApplication::processEvents();
    require(composerTo().isEmpty() && pane->isVisible() && pane->contact() == helper,
            "and Write to it opens the chat");
    // Not a bot any more once the name is not one.
    toContacts();
    s.setNickname(echo, "@echo");
    select(echo);
    require(write->toolTip() == bm::DesktopWindow::tr("Write a letter"),
            "a nickname that does not start with / makes no bot");
    s.setNickname(echo, "/echobot");
    select(echo);
    require(write->toolTip() == bm::DesktopWindow::tr("Chat with this bot"),
            "and the tip follows the nickname back");

    // A double-click does what Write does.
    toContacts();
    {
        const auto cell = select(echo);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, tree->visualRect(cell).center());
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, {}, tree->visualRect(cell).center());
        QCoreApplication::processEvents();
        require(composerTo().isEmpty() && pane->isVisible() && pane->contact() == echo,
                "a double-click on a bot opens its chat");
    }
    toContacts();
    {
        const auto cell = select(person);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, tree->visualRect(cell).center());
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, {}, tree->visualRect(cell).center());
        QCoreApplication::processEvents();
        require(composerTo() == person && !pane->isVisible(),
                "a double-click on a person opens a letter");
    }

    // Two identities: a contact with a chat under one opens that chat, with no question; one
    // without any is asked about, once, with the contact filled in.
    auto work = bm::Protocol::identity("the chat test's second identity");
    work.label = "Work";
    require(s.addIdentity(work), "a second identity joins the vault");
    const auto expiry = QDateTime::currentSecsSinceEpoch() + 3600;
    s.coreMailbox().storeChat("cc-1", veteran.toStdString(), work.address.toStdString(), 1,
                              "hello Vic", expiry, false);
    emit s.changed();
    QCoreApplication::processEvents();
    toContacts();
    select(veteran);
    chat->click();
    QCoreApplication::processEvents();
    require(asked == 0 && pane->isVisible() && pane->contact() == veteran &&
                pane->identity() == work.address,
            "Chat with a contact that has a chat opens it, under its own identity");
    toContacts();
    select(newcomer);
    answer = [&](bm::NewChatDialog &dialog) {
        auto box = dialog.findChild<QComboBox *>("chatIdentityBox");
        box->setCurrentIndex(box->findData(work.address));
        dialog.findChild<QPushButton *>("startChatButton")->click();
    };
    chat->click();
    QCoreApplication::processEvents();
    require(asked == 1 && askedAbout == newcomer && askedWithChoice,
            "with several identities Chat asks which, with the contact filled in");
    require(pane->isVisible() && pane->contact() == newcomer && pane->identity() == work.address,
            "and the chat starts under the one chosen");
    // The same contact again is the chat there is now: no second question.
    toContacts();
    select(newcomer);
    chat->click();
    QCoreApplication::processEvents();
    require(asked == 1 && pane->contact() == newcomer && pane->identity() == work.address,
            "the chat started is the one Chat opens next time");
    // Cancelling the question starts nothing.
    toContacts();
    select(quiet);
    answer = nullptr; // rejects
    chat->click();
    QCoreApplication::processEvents();
    require(asked == 2 && !pane->isVisible() && pane->contact() != quiet,
            "cancelling the question opens no chat");
    // Write to a bot, with two identities, asks the same.
    s.setNickname(quiet, "/quietbot");
    select(quiet);
    write->click();
    QCoreApplication::processEvents();
    require(asked == 3 && askedAbout == quiet && composerTo().isEmpty(),
            "Write to a bot asks which identity, too, and writes no letter");
    driver.stop();
}

static void testNewIdentityRegisters(const QTemporaryDir &temp) {
    const auto vaultFile = temp.filePath("newid.bmvault"), boxFile = temp.filePath("newid.bmmail");
    {
        bm::Vault made;
        made.create(vaultFile, "test password");
        made.addIdentity("Mine");
        const auto key = made.addMailboxKey();
        bm::Mailbox box;
        box.create(boxFile, key, made.mailboxKey(key));
        made.lock();
    }
    QDir().mkpath(temp.filePath("newid-node"));
    QSettings config(temp.filePath("newid-node/desktop.ini"), QSettings::IniFormat);
    config.setValue("vault", vaultFile);
    config.setValue("mailbox", boxFile);
    config.sync();
    const auto bot =
        QString::fromStdString(bmc::Protocol::identity("the identity dialog's nickbot").address);
    bm::updates::setDefaultNickbotsForTesting({{bot, "nickbot"}});
    struct Restore {
        ~Restore() {
            bm::updates::setDefaultNickbotsForTesting({});
        }
    } restore;
    bm::Session s(temp.filePath("newid-node"), true);
    s.beginVaultUnlock();
    s.submitVaultPassword("test password");
    require(s.mailboxOpen(), "test sanity: the mailbox opens");
    const auto chatsTo = [&](const QString &identity) {
        QStringList texts;
        for (const auto &m :
             s.coreMailbox().chatMessages(identity.toStdString(), bot.toStdString()))
            if (m.outgoing)
                texts << QString::fromStdString(m.body);
        return texts;
    };
    {
        bm::NewVaultDialog dialog(s, {}, false, nullptr, bm::NewVaultDialog::IdentityOnly);
        dialog.show();
        QCoreApplication::processEvents();
        auto box = dialog.findChild<QCheckBox *>("registerNickBox");
        auto field = dialog.findChild<QLineEdit *>("registerNickField");
        auto nickname = dialog.findChild<QLineEdit *>("nicknameField");
        auto create = dialog.findChild<QPushButton *>("newVaultCreateButton");
        auto problem = dialog.findChild<QLabel *>("registerNickProblem");
        require(box && field && box->isVisible() && box->isChecked(),
                "the dialog offers to register the nickname, and it is on");
        QTest::qWait(50);
        require(field->text() == bm::NickClient::suggestNick(nickname->text()) &&
                    field->text().startsWith('@'),
                "the nickname asked for is made from the one above");
        nickname->setText("Ann Example");
        require(field->text() == "@ann_example", "and follows it as it is typed");
        field->setText("@ann_x");
        emit field->textEdited("@ann_x");
        nickname->setText("Ann Other");
        require(field->text() == "@ann_x", "until it is written by hand: then it stays");
        field->setText("ann");
        require(!create->isEnabled() && problem->isVisible(),
                "a nickname that is none stops Create, and the dialog says what one is");
        field->setText("#chan");
        require(!create->isEnabled(), "a chan's name is not asked for an identity");
        box->setChecked(false);
        require(create->isEnabled() && !problem->isVisible(),
                "with the box off the nickname does not matter");
        box->setChecked(true);
        field->setText("@ann_x");
        require(create->isEnabled(), "a good one lets it be created");
        const auto before = s.identities().size();
        create->click();
        require(s.identities().size() == before + 1, "test sanity: the identity is made");
        QString made;
        for (const auto &i : s.identities())
            if (i.toMap()["label"].toString() == "Ann Other")
                made = i.toMap()["address"].toString();
        require(!made.isEmpty() && chatsTo(made) == QStringList{"/register @ann_x"} &&
                    s.nicks().registration(made)["state"].toString() == "waiting",
                "the nickname is asked for at once, by a chat from the new identity");
    }
    {
        bm::NewVaultDialog dialog(s, {}, false, nullptr, bm::NewVaultDialog::IdentityOnly);
        dialog.show();
        QCoreApplication::processEvents();
        dialog.findChild<QCheckBox *>("registerNickBox")->setChecked(false);
        const auto before = s.identities().size();
        dialog.findChild<QPushButton *>("newVaultCreateButton")->click();
        require(
            s.identities().size() == before + 1 &&
                s.nicks()
                        .registration(s.identities().last().toMap()["address"].toString())["state"]
                        .toString() == "none",
            "with the box off nothing is asked for");
    }
    // A new vault has no mailbox yet: the nickname waits for one.
    {
        const auto vaultOnly = temp.filePath("newid-vault-only.bmvault");
        {
            bm::Vault made;
            made.create(vaultOnly, "test password");
            made.addIdentity("Mine");
            made.lock();
        }
        QDir().mkpath(temp.filePath("newid-node-two"));
        QSettings two(temp.filePath("newid-node-two/desktop.ini"), QSettings::IniFormat);
        two.setValue("vault", vaultOnly);
        two.sync();
        bm::Session bare(temp.filePath("newid-node-two"), true);
        bare.beginVaultUnlock();
        bare.submitVaultPassword("test password");
        require(bare.unlocked() && !bare.mailboxOpen(), "test sanity: a vault with no mailbox");
        bm::NewVaultDialog dialog(bare, temp.filePath("newid-vaults"), false, nullptr);
        dialog.show();
        QCoreApplication::processEvents();
        QDir().mkpath(temp.filePath("newid-vaults"));
        require(
            dialog.findChild<QCheckBox *>("registerNickBox")->isVisible(),
            "a new vault offers the nickname too: the default nickbot is known with no mailbox");
        dialog.findChild<QLineEdit *>("nicknameField")->setText("Bea Bare");
        dialog.findChild<QLineEdit *>("vaultFileField")
            ->setText(temp.filePath("newid-vaults/bea.bmvault"));
        dialog.findChild<QLineEdit *>("newVaultPasswordField")->setText("a long test password");
        dialog.findChild<QLineEdit *>("newVaultRepeatField")->setText("a long test password");
        auto create = dialog.findChild<QPushButton *>("newVaultCreateButton");
        require(create->isEnabled(), "test sanity: Create is on");
        create->click();
        require(bare.nicks().waitingForAMailbox().size() == 1,
                "the nickname waits until a mailbox is open: there is none yet");
    }
    // No nickbots, no row.
    s.nicks().setBots({});
    {
        bm::NewVaultDialog dialog(s, {}, false, nullptr, bm::NewVaultDialog::IdentityOnly);
        dialog.show();
        require(!dialog.findChild<QCheckBox *>("registerNickBox")->isVisible(),
                "with no nickbot listed the dialog offers nothing");
    }
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
    // No window of these tests finds a PyBitmessage that is on the machine they run on.
    bm::Session::setPyBitmessageFoldersForTesting([] { return QStringList(); });
    // And no mailbox lists a nickbot unless the test says so: one would look up the address book
    // on its own and join its hidden chan to the vault.
    const auto builtInNickbots = bm::updates::defaultNickbots();
    bm::updates::setDefaultNickbotsForTesting({});
    QTemporaryDir temp;
    // A long vault path, like a macOS runner's /var/folders/.../T/...: the
    // locked screen must show it whole-height there too.
    const QString vaultFile =
        temp.filePath("var/folders/xz/q1w2e3r4t5y6u7i8o9p0asdfghjk/T/vaults-of-the-test/vault");
    QDir().mkpath(QFileInfo(vaultFile).absolutePath());
    try {
        // First: every window of a process gives the badge its own number, and this test has to
        // hear only the one it makes, so no other window is open yet.
        testBadgeInWindow(temp);
        // Before the window test below starts its responder, which answers every text prompt: this
        // one has prompts of its own to answer.
        testIdentitiesPage(temp);
        testAddressLabel();
        testEllipsisLabel();
        testReaderHeader(temp);
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
        // Likewise the built-in contacts: the address book starts empty, but for their own test.
        const auto builtInContacts = bm::updates::defaultContacts();
        bm::updates::setDefaultContactsForTesting({});
        bm::Session session(temp.filePath("node"), true);
        QElapsedTimer clock;
        clock.start();
        session.unlockVault();
        require(session.mailboxOpen(), "mailbox unlock");
        std::cout << "Unlock plus mailbox open: " << clock.elapsed() << "ms\n";
        QCoreApplication::setApplicationVersion("9.8.7");
        bm::DesktopWindow window(session, temp.filePath("sounds"));
        window.show();
        auto brand = window.findChild<QLabel *>("brand");
        require(brand && brand->text().contains("ynotbit-9.8.7"),
                "the top bar names ynotbit with its version");
        {
            // The notification area: the icon's menu, and the window that hides into it.
            auto tray = window.trayIcon();
            require(tray, "the window has an icon for the notification area");
            const auto trayAction = [&](const char *name) {
                auto action = tray->menu()->findChild<QAction *>(name);
                require(action, "the tray menu has the item");
                return action;
            };
            auto folders = window.findChild<QListWidget *>("folders");
            const int folderBefore = folders->currentRow();
            int quits = 0;
            bm::DesktopWindow::setQuitHandlerForTesting([&] { ++quits; });
            // Show or Hide follows the window, and does what it says.
            require(window.isVisible() && trayAction("trayShowHideAction")->text() == "Hide ynotbit",
                    "a shown window is offered Hide");
            trayAction("trayShowHideAction")->trigger();
            require(!window.isVisible() && trayAction("trayShowHideAction")->text() == "Show ynotbit",
                    "Hide hides the window, and the item then offers Show");
            trayAction("trayShowHideAction")->trigger();
            require(window.isVisible() && trayAction("trayShowHideAction")->text() == "Hide ynotbit",
                    "Show brings it back");
            window.hide();
            require(trayAction("trayShowHideAction")->text() == "Show ynotbit",
                    "a window hidden by anything else is offered Show too");
            window.bringToFront();
            require(window.isVisible() && trayAction("trayShowHideAction")->text() == "Hide ynotbit",
                    "bringing the window forward shows a hidden one");
            window.showMinimized();
            require(window.isMinimized() && trayAction("trayShowHideAction")->text() == "Show ynotbit",
                    "a minimized window is offered Show");
            trayAction("trayShowHideAction")->trigger();
            require(window.isVisible() && !window.isMinimized(),
                    "Show brings a minimized window back");
            // Writing is on while a mailbox is open.
            require(trayAction("trayNewLetterAction")->isEnabled() &&
                        trayAction("trayNewChatAction")->isEnabled(),
                    "with a mailbox open, New letter and New chat are on");
            // New letter opens a composer, and not the window: the inbox stays out of the way.
            window.hide();
            trayAction("trayNewLetterAction")->trigger();
            auto composer = window.findChild<QDialog *>("composer");
            require(composer && composer->isVisible() && !window.isVisible(),
                    "New letter opens a composer without bringing the window up");
            composer->close();
            waitComposerGone(window);
            // New chat brings the window up, and asks who to write to.
            window.hide();
            bool asked = false, windowUp = false;
            QTimer answer;
            QObject::connect(&answer, &QTimer::timeout, [&] {
                for (auto dialog : window.findChildren<bm::NewChatDialog *>()) {
                    asked = true;
                    windowUp = window.isVisible();
                    dialog->reject();
                }
            });
            answer.start(20);
            trayAction("trayNewChatAction")->trigger();
            answer.stop();
            require(asked && windowUp, "New chat brings the window up and asks who to write to");
            // Settings brings the window up on its page.
            window.hide();
            trayAction("traySettingsAction")->trigger();
            auto settingsPane = window.findChild<QWidget *>("settingsPane");
            require(window.isVisible() && settingsPane && settingsPane->isVisible(),
                    "Settings brings the window up on the Settings page");
            folders->setCurrentRow(folderBefore);
            // Quit asks the app to end, from the icon and, where there is no application menu, the
            // window's own File menu.
            require(quits == 0, "nothing has quit yet");
            trayAction("trayQuitAction")->trigger();
            require(quits == 1, "Quit in the icon's menu quits");
#ifndef Q_OS_MACOS
            auto fileQuit = window.findChild<QAction *>("quitAction");
            require(fileQuit, "File has a Quit, where the application menu has none");
            fileQuit->trigger();
            require(quits == 2, "and it quits");
#endif
            // With the icon there, the app goes on without its window however the window went: a
            // letter closed last, with the window hidden, would end it otherwise.
            bm::TrayIcon::setAvailableForTesting(true);
            window.hide();
            require(!QApplication::quitOnLastWindowClosed(),
                    "with the icon there, hiding the window does not end the app with its last window");
            window.show();
            // Closing it hides it, and the app lives on.
            window.compose();
            composer = window.findChild<QDialog *>("composer");
            require(composer && composer->isVisible(), "(a letter is being written)");
            require(window.close(), "a window may be closed into the icon: the close is allowed");
            require(!window.isVisible() && !QApplication::quitOnLastWindowClosed(),
                    "closing hides the window, and the app does not end with it");
            require(composer->isVisible(), "a letter being written stays: it is a window of its own");
            require(trayAction("trayShowHideAction")->text() == "Show ynotbit",
                    "the icon then offers the window back");
            window.show();
            composer->close();
            waitComposerGone(window);
            // With none, there is no way back in, so closing is as it was: the letters close, the app ends.
            bm::TrayIcon::setAvailableForTesting(false);
            QWidget keepAlive; // lets the last-window rule be seen to hold, without ending this run
            keepAlive.show();
            window.compose();
            composer = window.findChild<QDialog *>("composer");
            require(composer && window.close() && !window.isVisible(),
                    "with no icon, the window closes");
            require(QApplication::quitOnLastWindowClosed(), "and with its last window the app ends");
            waitComposerGone(window);
            require(!window.findChild<QDialog *>("composer"), "taking a letter being written with it");
            keepAlive.close();
            bm::TrayIcon::setAvailableForTesting(std::nullopt);
            bm::DesktopWindow::setQuitHandlerForTesting({});
            window.show();
        }
        auto folders = window.findChild<QListWidget *>("folders");
        require(folders, "folders exist");
        // Chats owns its panel and must not leave mail controls visible behind it.
        auto chatsIcon = window.findChild<QToolButton *>("folderIcon_Chats");
        require(chatsIcon, "Chats navigation exists");
        chatsIcon->click();
        QTest::qWait(30);
        auto chatsPane = window.findChild<QWidget *>("chatsPane");
        require(chatsPane && chatsPane->isVisible(), "Chats opens its own panel");
        require(chatsIcon->y() < window.findChild<QToolButton *>("folderIcon_Inbox")->y(),
                "Chats precedes Inbox in the rail");
        auto sharedRail = window.findChild<QWidget *>("channelRail");
        require(!window.findChild<QWidget *>("listColumn")->isVisible() && sharedRail->isVisible(),
                "Chats reuses the visible channel rail and hides the mail list");
        require(!window.findChild<QLabel *>("chatPreviewBadge"), "Chats has no preview badge");
        auto toggleRail = window.findChild<QPushButton *>("channelRailToggle");
        // Chats, Channels and Subscriptions share the rail but not its collapse. Whatever an
        // earlier run left in the settings, each starts expanded.
        for (int folder : {4, 5, 11}) {
            folders->setCurrentRow(folder);
            if (toggleRail->text() == ">>>")
                toggleRail->click();
        }
        const int originalRailWidth = sharedRail->width();
        toggleRail->click();
        require(sharedRail->width() == 50, "Chats uses the existing collapse control");
        {
            // A reader may keep the chans compact beside an expanded chat list, and every
            // page comes back as it was left.
            const auto railOn = [&](int folder) {
                folders->setCurrentRow(folder);
                QCoreApplication::processEvents();
                return sharedRail->width();
            };
            require(railOn(4) == originalRailWidth && railOn(5) == originalRailWidth &&
                        toggleRail->text() == "<<<",
                    "collapsing Chats leaves Channels and Subscriptions expanded");
            railOn(4);
            toggleRail->click();
            require(railOn(5) == originalRailWidth,
                    "collapsing Channels leaves Subscriptions expanded");
            require(railOn(4) == 50 && toggleRail->text() == ">>>" && railOn(11) == 50,
                    "each page comes back as it was left");
            toggleRail->click(); // Chats, expanded
            require(railOn(11) == originalRailWidth && railOn(4) == 50,
                    "expanding Chats leaves Channels collapsed");
            // Each page's rail is kept for the next start. The rail is hidden while a
            // window has no mailbox, so its pages are told by the width they were given.
            const auto railsOf = [&](const QString &node) {
                bm::Session own(temp.filePath(node), true);
                bm::DesktopWindow reopened(own, temp.filePath("sounds-reopened"));
                auto reopenedFolders = reopened.findChild<QListWidget *>("folders");
                auto reopenedRail = reopened.findChild<QWidget *>("channelRail");
                QList<int> widths;
                for (int folder : {4, 5, 11}) {
                    reopenedFolders->setCurrentRow(folder);
                    widths << reopenedRail->maximumWidth();
                }
                return widths;
            };
            require(railsOf("rail-node") == QList<int>({50, originalRailWidth, originalRailWidth}),
                    "a window opened later has each page's rail as it was left");
            toggleRail->click(); // Channels, expanded again
            // One collapse for the whole rail was all that older versions kept: a reader
            // who had collapsed it finds all three pages collapsed, to change one by one.
            QSettings().remove("railCollapsed");
            QSettings().setValue("channelRailCollapsed", true);
            require(railsOf("rail-node") == QList<int>({50, 50, 50}),
                    "a rail collapsed before the pages had each their own is collapsed on all");
            QSettings().remove("channelRailCollapsed");
            railOn(11);
        }
        const auto chatAddress = bm::Protocol::identity("chat rail test").address;
        require(session.addContact(chatAddress, "Chat rail contact"), "Add contact for chat rail");
        QCoreApplication::processEvents();
        require(sharedRail->findChildren<QPushButton *>("channelChip").isEmpty(),
                "a contact is not a chat: the Chats rail lists chats only");
        window.startChat(personal, chatAddress);
        QCoreApplication::processEvents();
        QPushButton *chatChip = nullptr;
        for (auto chip : sharedRail->findChildren<QPushButton *>("channelChip"))
            if (chip->property("address").toString() == chatAddress)
                chatChip = chip;
        require(chatChip && qobject_cast<bm::ChatRow *>(chatChip),
                "A chat is a row of the shared rail, as a chan is a chip");
        require(window.findChild<QLabel *>("chatTitle")->text() == "Chat rail contact",
                "Starting a chat opens it");
        folders->setCurrentRow(4);
        require(sharedRail->isVisible() && sharedRail->width() == originalRailWidth,
                "Channels shows the same rail");
        chatsIcon->click();
        require(window.findChild<QLabel *>("chatTitle")->text() == "Chat rail contact",
                "Returning to Chats restores its selected conversation");
        // A conversation is redrawn only when it changed, and never moves a reader
        // who scrolled up into it; a theme change recolours it. Faults are collected,
        // so one run shows them all.
        QStringList chatFaults;
        const auto expect = [&](bool ok, const char *fault) {
            if (!ok)
                chatFaults << fault;
        };
        {
            bm::ChatsPane pane;
            pane.resize(640, 420);
            pane.show();
            pane.setConversation(personal, "Personal", chatAddress, "Pane contact", false);
            const auto chat = [](int n, const QString &state = "received") {
                return QVariantMap{{"id", "pane-" + QString::number(n)},
                                   {"body", "Message number " + QString::number(n)},
                                   {"outgoing", n % 2 == 0},
                                   {"seqno", n},
                                   {"received", 1700000000 + n},
                                   {"expires", 1700086400 + n},
                                   {"state", state}};
            };
            const auto count = [&](const char *name) {
                return pane.findChildren<QWidget *>(name).size();
            };
            QVariantList thread;
            for (int n = 1; n <= 40; ++n)
                thread << chat(n);
            pane.setMessages(thread);
            auto bar = pane.findChild<QScrollArea *>("chatMessagesScroll")->verticalScrollBar();
            // Messages are laid out after setMessages() returns, later on a busy machine: wait
            // for the bar to come to rest at the end, not for a fixed time.
            const auto atEnd = [&] { return bar->maximum() > 0 && bar->value() == bar->maximum(); };
            expect(QTest::qWaitFor(atEnd, 5000), "a conversation opens at its newest message");
            QPointer<QWidget> first = pane.findChildren<QWidget *>("incomingChatBubble").value(0);
            pane.setMessages(thread);
            expect(!first.isNull(), "a conversation that did not change is not redrawn");
            bar->setValue(bar->maximum() / 2);
            const int reading = bar->value(), shortRange = bar->maximum();
            thread << chat(41);
            pane.setMessages(thread);
            // Once the new message has lengthened the bar, the reader is still where they were.
            const bool longer = QTest::qWaitFor([&] { return bar->maximum() > shortRange; }, 5000);
            expect(longer && count("incomingChatBubble") == 21 && bar->value() == reading,
                   "a new message does not move a reader who scrolled up");
            bar->setValue(bar->maximum());
            const int longRange = bar->maximum();
            thread << chat(42);
            pane.setMessages(thread);
            const bool followed =
                QTest::qWaitFor([&] { return bar->maximum() > longRange && atEnd(); }, 5000);
            expect(followed && count("outgoingChatBubble") == 21,
                   "a reader at the newest message follows the new ones");
            thread[41] = chat(42, "acknowledged");
            pane.setMessages(thread);
            const auto ticks = pane.findChildren<QLabel *>("chatTick");
            expect(!ticks.isEmpty() && ticks.last()->property("tick").toString() == "delivered",
                   "a sent chat's new state shows");
            bar->setValue(0);
            pane.setConversation(personal, "Personal",
                                 bm::Protocol::identity("chat pane other").address, "Other contact",
                                 false);
            pane.setMessages(thread);
            expect(QTest::qWaitFor(atEnd, 5000), "another conversation opens at its newest message");
            pane.setDark(true);
            QTest::qWait(30);
            const auto outgoing = pane.findChildren<QWidget *>("outgoingChatBubble");
            const auto incoming = pane.findChildren<QWidget *>("incomingChatBubble");
            // What is drawn at a bubble's left edge, clear of its text and its rounded corners.
            const auto shade = [](QWidget *bubble) {
                return bubble->grab().toImage().pixelColor(4, bubble->height() / 2);
            };
            expect(!outgoing.isEmpty() && !incoming.isEmpty() &&
                       shade(outgoing.first()) == QColor("#174944") &&
                       shade(incoming.first()) == QColor("#30343c"),
                   "a theme change recolours the conversation shown");
        }
        // A long conversation is shown a window at a time: its newest messages first, older
        // ones as the reader scrolls up to them, and nothing moves under their eyes.
        {
            bm::ChatsPane pane;
            pane.resize(640, 420);
            pane.show();
            pane.setConversation(personal, "Personal", chatAddress, "Window contact", false);
            const auto line = [](int n, const QString &state = {}) {
                const bool mine = n % 3 == 0;
                return QVariantMap{{"id", "window-" + QString::number(n)},
                                   {"body", "window " + QString::number(n) + " end"},
                                   {"outgoing", mine},
                                   {"seqno", n},
                                   {"received", 1700000000 + n},
                                   {"expires", 1700086400 + n},
                                   {"state", !state.isEmpty() ? state
                                             : mine           ? "acknowledged"
                                                              : "received"}};
            };
            const auto lines = [&](int from, int to) { // [from, to), oldest first
                QVariantList list;
                for (int n = from; n < to; ++n)
                    list << line(n);
                return list;
            };
            auto scroll = pane.findChild<QScrollArea *>("chatMessagesScroll");
            auto bar = scroll->verticalScrollBar();
            QSignalSpy asked(&pane, &bm::ChatsPane::olderRequested);
            const auto shown = [&] { return pane.findChildren<QLabel *>("chatBody").size(); };
            const auto bodyOf = [&](int n) -> QLabel * {
                for (auto body : pane.findChildren<QLabel *>("chatBody"))
                    if (body->text().contains("window " + QString::number(n) + " end"))
                        return body;
                return nullptr;
            };
            const auto topOf = [&](QWidget *w) {
                return w->mapTo(scroll->viewport(), QPoint(0, 0)).y();
            };
            const auto atEnd = [&] { return bar->maximum() > 0 && bar->value() == bar->maximum(); };
            QVariantList thread = lines(300, 400); // the newest 100 of a long conversation
            pane.setMessages(thread, true);
            expect(QTest::qWaitFor(atEnd, 5000) && shown() == 100,
                   "a long conversation opens at its newest message, as a window of it");
            QTest::qWait(30);
            expect(asked.isEmpty(), "far from the top of the window, nothing older is asked for");
            bar->setValue(500);
            QTest::qWait(30);
            expect(asked.size() == 1, "within two screens of the top, older messages are asked for");
            bar->setValue(450);
            QTest::qWait(30);
            expect(asked.size() == 1, "and asked for once, until they come");
            QLabel *anchor = nullptr; // the first message wholly in view
            for (int n = 300; n < 400 && !anchor; ++n)
                if (auto body = bodyOf(n); body && topOf(body) >= 0)
                    anchor = body;
            QPointer<QLabel> kept = anchor;
            const int placed = anchor ? topOf(anchor) : 0;
            thread = lines(250, 300) + thread;
            pane.setMessages(thread, true);
            expect(shown() == 150 && kept && qAbs(topOf(anchor) - placed) <= 1,
                   "older messages come in above without moving what the reader is looking at");
            QTest::qWait(50);
            expect(kept && qAbs(topOf(anchor) - placed) <= 1 && asked.size() == 1,
                   "and the reader is not asked again while far from the new top");
            const int reading = bar->value();
            thread << line(400);
            pane.setMessages(thread, true);
            QTest::qWait(50);
            expect(shown() == 151 && kept && bar->value() == reading &&
                       qAbs(topOf(anchor) - placed) <= 1,
                   "a new message does not move a reader who scrolled up into older ones");
            thread[399 - 250] = line(399, "failed");
            pane.setMessages(thread, true);
            auto sent = bodyOf(399) ? bodyOf(399)->parentWidget()->findChild<QLabel *>("chatTick")
                                    : nullptr;
            expect(sent && sent->property("tick").toString() == "failed" && kept && shown() == 151 &&
                       bar->value() == reading,
                   "a message that moved on is redrawn alone, and the reader stays");
            bar->setValue(bar->maximum());
            thread << line(401);
            pane.setMessages(thread, true);
            expect(QTest::qWaitFor(atEnd, 5000) && shown() == 152,
                   "a reader at the newest message follows the new ones");
            pane.setMessages(thread, false);
            bar->setValue(0);
            QTest::qWait(30);
            expect(asked.size() == 1, "with nothing older left, nothing is asked for");
            pane.setConversation(personal, "Personal",
                                 bm::Protocol::identity("chat pane window other").address,
                                 "Other window contact", false);
            pane.setMessages(lines(0, 10), true);
            expect(QTest::qWaitFor(atEnd, 5000) && shown() == 10,
                   "another conversation opens at its newest message");
            QTest::qWait(30);
            expect(asked.size() == 2,
                   "a window whose newest end is already near its top asks for older ones at once");
        }
        // A message's label is as tall as a plain QLabel makes it, whatever the text and the
        // width: a line, a paragraph that wraps, a list with code and a link, other scripts, a
        // word longer than the bubble. Any other height clips the text or leaves a gap.
        {
            bm::ChatsPane pane;
            pane.resize(640, 420);
            pane.show();
            pane.setConversation(personal, "Personal", chatAddress, "Label contact", false);
            const QStringList texts = {
                "ok",
                "A line of some length, as wide as its text and no more.",
                "Just checking in about the **release**: the build for macOS is green, Windows is "
                "still waiting on the signing certificate. I will push the notes once CI finishes.",
                "Notes:\n\n1. Bump the version in `CMakeLists.txt`\n2. Dispatch CI\n3. Check "
                "[the notes](https://example.org/notes)\n\n```\ncmake --build build\nctest\n```\n"
                "Then publish.",
                "Ünïcödé, 日本語のメッセージ, и русский текст, 😀 emoji",
                "a_very_long_word_without_any_spaces_to_force_wrapping_anywhere_in_the_bubble_and_"
                "beyond_the_limit_of_its_width_so_that_it_cannot_fit_on_one_line_at_all",
                "> a quote\n\nand a reply under it"};
            QVariantList thread;
            for (int n = 0; n < texts.size(); ++n)
                thread << QVariantMap{{"id", "label-" + QString::number(n)},
                                      {"body", texts[n]},
                                      {"outgoing", n % 2 == 0},
                                      {"seqno", n + 1},
                                      {"received", 1700000000 + n},
                                      {"expires", 1700086400 + n},
                                      {"state", n % 2 == 0 ? "acknowledged" : "received"}};
            pane.setMessages(thread);
            int unequal = 0, checked = 0;
            for (int width : {640, 900, 520, 380}) {
                pane.resize(width, 420);
                QTest::qWait(80);
                for (auto body : pane.findChildren<QLabel *>("chatBody")) {
                    QLabel twin(body->text(), body->parentWidget());
                    twin.setTextFormat(Qt::RichText);
                    twin.setWordWrap(true);
                    twin.ensurePolished();
                    unequal += twin.heightForWidth(body->width()) != body->height();
                    ++checked;
                }
            }
            expect(checked == 4 * texts.size() && unequal == 0,
                   "a message's label is as tall as QLabel makes it, for any text and width");
        }
        // Enter sends, and Shift+Enter starts a new line; an empty composer ignores Enter.
        {
            bm::ChatsPane pane;
            pane.show();
            pane.setConversation(personal, "Personal", chatAddress, "Enter contact", false);
            auto composer = pane.findChild<QPlainTextEdit *>("chatComposer");
            QSignalSpy sent(&pane, &bm::ChatsPane::sendRequested);
            QTest::keyClick(composer, Qt::Key_Return);
            expect(sent.isEmpty() && composer->toPlainText().isEmpty(),
                   "Enter on an empty composer does nothing");
            QTest::keyClicks(composer, "first line");
            QTest::keyClick(composer, Qt::Key_Return, Qt::ShiftModifier);
            QTest::keyClicks(composer, "second line");
            expect(sent.isEmpty() && composer->toPlainText() == "first line\nsecond line",
                   "Shift+Enter starts a new line");
            QTest::keyClick(composer, Qt::Key_Enter);
            expect(sent.size() == 1 && sent.first().at(1).toString() == "first line\nsecond line" &&
                       composer->toPlainText() == "first line\nsecond line",
                   "Enter sends the message and adds no line break");
            expect(!composer->toolTip().isEmpty(), "the composer says what Enter does");
        }
        // A conversation is between one of my identities and a peer, and keeps that
        // identity: another one is another conversation. So the identity is only shown,
        // in the header and only when there are several, never chosen in the composer.
        // The composer is one card: a field that grows with its text up to six lines,
        // and a round send button. With no conversation there is none.
        {
            bm::ChatsPane pane;
            pane.resize(720, 520);
            pane.show();
            auto identity = pane.findChild<QFrame *>("chatIdentity");
            auto card = pane.findChild<QFrame *>("chatCard");
            auto composer = pane.findChild<QPlainTextEdit *>("chatComposer");
            auto send = pane.findChild<QPushButton *>("chatSendButton");
            expect(identity && card && composer && send,
                   "the header has an identity chip, the composer a card, a field, a send button");
            expect(pane.findChildren<QComboBox *>().isEmpty() &&
                       pane.findChildren<QMenu *>().isEmpty(),
                   "no dropdown or menu to change the identity of a conversation");
            if (identity && card && composer && send) {
                expect(!card->isVisible(), "with no conversation there is no composer");
                pane.setConversation(personal, "Personal", chatAddress, "Card contact", false);
                expect(card->isVisible() && pane.identity() == personal &&
                           pane.contact() == chatAddress,
                       "a conversation has a composer, and knows its identity and peer");
                expect(!identity->isVisible(), "with one identity it is not shown");
                pane.setConversation(personal, "Personal", chatAddress, "Card contact", true);
                auto name = identity->findChild<QLabel *>("chatIdentityName");
                expect(identity->isVisible() && name && name->text() == "Personal" &&
                           !identity->toolTip().isEmpty() &&
                           identity->findChildren<QAbstractButton *>().isEmpty(),
                       "with several the header names it, as text and not as a control");
                expect(send->text().isEmpty() && !send->icon().isNull() &&
                           !send->toolTip().isEmpty() && send->width() == send->height(),
                       "the send button is a round icon button with a tooltip");
                QTest::keyClicks(composer, "one line");
                QTest::qWait(30);
                const int oneLine = composer->height(), cardOne = card->height();
                expect(!composer->verticalScrollBar()->isVisible(),
                       "no scrollbar beside a line of text");
                for (int i = 0; i < 3; ++i) {
                    QTest::keyClick(composer, Qt::Key_Return, Qt::ShiftModifier);
                    QTest::keyClicks(composer, "and another");
                }
                QTest::qWait(30);
                expect(composer->height() > oneLine && card->height() > cardOne,
                       "the field grows with its text");
                expect(!composer->verticalScrollBar()->isVisible(),
                       "and has no scrollbar while the text fits");
                for (int i = 0; i < 12; ++i) {
                    QTest::keyClick(composer, Qt::Key_Return, Qt::ShiftModifier);
                    QTest::keyClicks(composer, "and more");
                }
                QTest::qWait(30);
                expect(composer->height() <= 6 * composer->fontMetrics().lineSpacing() + 8 &&
                           composer->verticalScrollBar()->maximum() > 0,
                       "up to six lines, then it scrolls");
                pane.clearComposer();
                QTest::qWait(30);
                expect(composer->height() == oneLine, "and shrinks back once the text is sent");
                QFocusEvent in(QEvent::FocusIn, Qt::OtherFocusReason);
                QApplication::sendEvent(composer, &in);
                expect(card->property("focused").toBool(), "the card shows where the focus is");
                QFocusEvent away(QEvent::FocusOut, Qt::OtherFocusReason);
                QApplication::sendEvent(composer, &away);
                expect(!card->property("focused").toBool(), "and stops showing it");
                const qint64 now = QDateTime::currentSecsSinceEpoch();
                const auto line = [&](int n, bool outgoing) {
                    return QVariantMap{{"id", "card-" + QString::number(n)},
                                       {"body", outgoing ? "Mine" : "Theirs"},
                                       {"outgoing", outgoing},
                                       {"seqno", n},
                                       {"received", now},
                                       {"expires", now + 86400},
                                       {"state", outgoing ? "acknowledged" : "received"}};
                };
                pane.setMessages(QVariantList{line(1, false), line(2, true)});
                QTest::qWait(30);
                auto theirs = pane.findChild<QWidget *>("incomingChatBubble");
                auto mine = pane.findChild<QWidget *>("outgoingChatBubble");
                auto avatar = pane.findChild<QLabel *>("chatAvatar");
                const auto left = [&](QWidget *w) { return w->mapTo(&pane, QPoint(0, 0)).x(); };
                const auto right = [&](QWidget *w) {
                    return w->mapTo(&pane, QPoint(w->width(), 0)).x();
                };
                expect(theirs && mine && avatar && left(avatar) == left(theirs) &&
                           left(theirs) == left(card) && right(mine) == right(card) &&
                           right(identity) == right(card),
                       "the header, the bubbles and the composer line up on both sides");
                pane.setConversation({}, {}, {}, {}, false);
                expect(!card->isVisible() && pane.contact().isEmpty(),
                       "no conversation, no composer again");
            }
        }
        // A sent chat shows how far it has got as a tick beside its time, a message
        // of today by its time alone and an older one by its date as well.
        {
            bm::ChatsPane pane;
            pane.resize(640, 420);
            pane.show();
            pane.setConversation(personal, "Personal", chatAddress, "Tick contact", false);
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            const auto message = [&](int n, bool outgoing, const QString &state, qint64 when = 0) {
                return QVariantMap{{"id", "tick-" + QString::number(n)},
                                   {"body", n == 7 ? QString("Hi! Are we still on for Thursday?")
                                                   : "Tick " + QString::number(n)},
                                   {"outgoing", outgoing},
                                   {"seqno", n},
                                   {"received", when ? when : now},
                                   {"expires", now + 86400},
                                   {"state", state}};
            };
            pane.setMessages(
                QVariantList{message(1, true, "queued"), message(2, true, "calculating_message"),
                             message(3, true, "awaiting_ack"), message(4, true, "acknowledged"),
                             message(5, true, "failed"), message(6, true, "expired"),
                             message(7, false, "received")});
            QStringList kinds;
            QSet<QString> tips;
            for (auto tick : pane.findChildren<QLabel *>("chatTick")) {
                kinds << tick->property("tick").toString();
                tips << tick->toolTip();
            }
            expect(kinds ==
                       QStringList{"pending", "pending", "sent", "delivered", "failed", "failed"},
                   "a sent chat shows how far it has got, a received one no tick");
            expect(tips.size() == 4 && !tips.contains(QString()), "each tick says what it means");
            // A short message is as wide as its text, not wrapped into a narrow box.
            QTest::qWait(50);
            bool wrapped = false;
            for (auto body : pane.findChildren<QLabel *>("chatBody"))
                wrapped =
                    wrapped || body->heightForWidth(body->width()) != body->heightForWidth(100000);
            expect(!wrapped, "a short message is not wrapped inside its bubble");
            auto times = pane.findChildren<QLabel *>("chatDetails");
            expect(times.size() == 7 &&
                       times.last()->text() ==
                           QLocale().toString(QDateTime::fromSecsSinceEpoch(now).time(),
                                              QLocale::ShortFormat),
                   "a message of today shows its time alone");
            const auto old = now - 3 * 86400;
            pane.setMessages(QVariantList{message(8, false, "received", old)});
            times = pane.findChildren<QLabel *>("chatDetails");
            expect(times.size() == 1 && times.last()->text() ==
                                            QLocale().toString(QDateTime::fromSecsSinceEpoch(old),
                                                               QLocale::ShortFormat),
                   "an older message shows its date as well");
        }
        // Chats are live: a chat that arrives shows in the open conversation, and a
        // stranger who wrote joins the rail, without a click. Links in a chat come
        // from a stranger: https only, after asking, as in a letter.
        {
            const auto stranger = bm::Protocol::identity("chat stranger").address;
            const auto expiry = QDateTime::currentSecsSinceEpoch() + 3600;
            const auto arrive = [&](const std::function<void(bm::Mailbox &)> &write) {
                vault.unlock(vaultFile, "test password");
                bm::Mailbox injected;
                injected.open(temp.filePath("mailbox"), vault.mailboxKey(key));
                write(injected);
                injected.close();
                vault.lock();
                emit session.changed(); // what the next tick does, without waiting for it
            };
            const auto bubbles = [&](const char *name) {
                return window.findChildren<QWidget *>(name);
            };
            const auto chipOf = [&](const QString &address) {
                QPushButton *found = nullptr;
                for (auto chip : sharedRail->findChildren<QPushButton *>("channelChip"))
                    if (chip->property("address").toString() == address)
                        found = chip;
                return found;
            };
            const auto row = [&](const QString &address) {
                return qobject_cast<bm::ChatRow *>(chipOf(address));
            };
            // The windows above took the activation; what is read is read in the active one.
            window.activateWindow();
            QTest::qWait(30);
            expect(window.isActiveWindow(), "the window is the active one");
            arrive([&](bm::Mailbox &m) {
                m.storeChat("live-1", chatAddress, personal, 1, "Hello **live**", expiry, false);
            });
            expect(bubbles("incomingChatBubble").size() == 1,
                   "a chat that arrives shows in the open conversation");
            expect(row(chatAddress) && row(chatAddress)->preview() == "Hello live" &&
                       row(chatAddress)->unread() == 0 && row(chatAddress)->received() > 0,
                   "the rail row of the open conversation shows its newest message, read");
            QPointer<QWidget> arrived = bubbles("incomingChatBubble").value(0);
            emit session.changed();
            expect(!arrived.isNull(), "the open conversation is not redrawn while nothing changes");
            arrive([&](bm::Mailbox &m) {
                m.storeChat("live-2", stranger, personal, 1,
                            "[a file](file:///Applications/Calculator.app) "
                            "[a site](https://example.org/chat)",
                            expiry, false);
            });
            expect(chipOf(stranger), "a stranger who wrote joins the rail");
            expect(row(stranger) && row(stranger)->unread() == 1 &&
                       row(stranger)->preview() == "a file a site",
                   "a conversation not open shows its newest message and what is unread");
            QTest::qWait(30); // the layout moves the rows
            expect(row(stranger) && row(chatAddress) && row(stranger)->y() < row(chatAddress)->y(),
                   "the conversation with the newest message comes first");
            expect(chatsIcon->property("unread").toInt() == 1,
                   "the Chats button counts what is unread, wherever the reader is");
            // Reopened, so the links are tested whether or not the page is live.
            window.findChild<QToolButton *>("folderIcon_Inbox")->click();
            chatsIcon->click();
            if (auto chip = chipOf(stranger))
                chip->click();
            QTest::qWait(30);
            expect(row(stranger) && row(stranger)->unread() == 0 &&
                       chatsIcon->property("unread").toInt() == 0 && session.chatUnread() == 0,
                   "opening a conversation reads it");
            toggleRail->click();
            expect(row(stranger) && row(stranger)->collapsed() && row(stranger)->unread() == 0,
                   "a collapsed rail still shows each conversation");
            toggleRail->click();
            expect(row(stranger) && !row(stranger)->collapsed(), "an expanded rail shows its rows");
            UrlRecorder recorder;
            auto &opened = recorder.opened;
            for (const char *scheme : {"https", "file"})
                QDesktopServices::setUrlHandler(scheme, &recorder, "record");
            QMessageBox::StandardButton answer = QMessageBox::No;
            bool asked = false;
            QTimer answering;
            QObject::connect(&answering, &QTimer::timeout, [&] {
                for (auto w : QApplication::topLevelWidgets())
                    if (auto box = qobject_cast<QMessageBox *>(w); box && box->isVisible()) {
                        asked = true;
                        box->button(answer)->click();
                    }
            });
            answering.start(10);
            QLabel *linked = nullptr;
            for (auto label : window.findChildren<QLabel *>("chatBody"))
                if (label->text().contains("Calculator"))
                    linked = label;
            expect(linked && !linked->openExternalLinks(), "a chat's links do not open themselves");
            if (linked) {
                emit linked->linkActivated("file:///Applications/Calculator.app");
                expect(opened.isEmpty() && !asked, "a chat's file link is ignored, unasked");
                emit linked->linkActivated("https://example.org/chat");
                expect(asked && opened.isEmpty(), "a chat's https link asks, and No opens nothing");
                asked = false;
                answer = QMessageBox::Yes;
                emit linked->linkActivated("https://example.org/chat");
                expect(asked && opened == QStringList{"https://example.org/chat"},
                       "a chat's https link opens after Yes");
            }
            if (auto reader = window.findChild<QTextBrowser *>("readerBody")) {
                answer = QMessageBox::Yes;
                opened.clear();
                asked = false;
                emit reader->anchorClicked(QUrl("https://example.org/letter"));
                expect(asked && opened == QStringList{"https://example.org/letter"},
                       "a letter's https link asks first and opens after Yes");
                opened.clear();
                asked = false;
                emit reader->anchorClicked(QUrl("file:///Applications/Calculator.app"));
                expect(opened.isEmpty() && !asked, "a letter's file link is ignored, unasked");
            }
            // A chat is read when it is looked at: with the window active.
            {
                QWidget elsewhere;
                elsewhere.show();
                elsewhere.activateWindow();
                QTest::qWait(50);
                if (!window.isActiveWindow()) { // a platform that cannot tell skips this
                    arrive([&](bm::Mailbox &m) {
                        m.storeChat("live-3", stranger, personal, 2, "while away", expiry, false);
                    });
                    expect(row(stranger) && row(stranger)->unread() == 1,
                           "a chat is not read while the window is not the active one");
                    window.activateWindow();
                    QTest::qWait(50);
                    emit session.changed();
                    expect(row(stranger) && row(stranger)->unread() == 0,
                           "it is read when the window is active again");
                }
            }
            // New chat: who to, and which identity, chosen once, when the chat starts.
            {
                bm::NewChatDialog dialog(session, false);
                dialog.show();
                auto field = dialog.findChild<QLineEdit *>("chatPeerField");
                auto box = dialog.findChild<QComboBox *>("chatIdentityBox");
                auto start = dialog.findChild<QPushButton *>("startChatButton");
                auto problem = dialog.findChild<QLabel *>("chatProblem");
                auto picker = dialog.findChild<QPushButton *>("chatContactsButton");
                const bool found = field && box && start && problem && picker;
                expect(found, "the new chat dialog has an address, an identity, a problem line, "
                              "a contacts button and a start button");
                if (found) {
                    expect(!start->isEnabled(), "an empty address starts nothing");
                    QTest::keyClicks(field, "nope");
                    expect(!start->isEnabled() && problem->isVisibleTo(&dialog) &&
                               !problem->text().isEmpty(),
                           "an invalid address is said, and not started");
                    field->setText(personal);
                    expect(!start->isEnabled() && problem->isVisibleTo(&dialog),
                           "one of your own identities is not someone to chat with");
                    const auto fresh = bm::Protocol::identity("new chat peer").address;
                    field->setText(fresh);
                    expect(start->isEnabled() && !problem->isVisibleTo(&dialog) &&
                               dialog.peer() == fresh && dialog.identity() == personal,
                           "a valid address starts a chat, from the one identity");
                    expect(!box->isVisibleTo(&dialog), "with one identity there is nothing to ask");
                    QAction *named = nullptr;
                    if (picker->menu())
                        for (auto action : picker->menu()->actions())
                            if (action->text() == "Chat rail contact")
                                named = action;
                    expect(named, "the contacts button lists the address book");
                    if (named)
                        named->trigger();
                    expect(dialog.peer() == chatAddress, "choosing a contact fills in its address");
                }
            }
            // A chat is started from the rail's button: open at once, listed first while it
            // is empty, kept when the page is left, and the same row once messages come.
            {
                const auto fresh = bm::Protocol::identity("started chat peer").address;
                QTimer driver;
                QObject::connect(&driver, &QTimer::timeout, [&] {
                    for (auto dialog : window.findChildren<bm::NewChatDialog *>())
                        if (dialog->isVisible()) {
                            dialog->findChild<QLineEdit *>("chatPeerField")->setText(fresh);
                            dialog->findChild<QPushButton *>("startChatButton")->click();
                        }
                });
                driver.start(10);
                window.findChild<QPushButton *>("joinOrCreateChannelButton")->click();
                driver.stop();
                QTest::qWait(30);
                auto pane = window.findChild<bm::ChatsPane *>("chatsPane");
                expect(pane && pane->contact() == fresh && pane->identity() == personal,
                       "New chat in the rail asks, then opens the chat");
                expect(row(fresh) && row(fresh)->preview().isEmpty() && row(stranger) &&
                           row(fresh)->y() < row(stranger)->y(),
                       "an empty chat is listed, first");
                window.findChild<QToolButton *>("folderIcon_Inbox")->click();
                chatsIcon->click();
                QTest::qWait(30);
                expect(row(fresh), "it stays when the page is left and opened again");
                arrive([&](bm::Mailbox &m) {
                    m.storeChat("started-1", fresh, personal, 7, "first words", expiry, false);
                });
                QTest::qWait(30);
                int rowsFor = 0;
                for (auto chip : sharedRail->findChildren<QPushButton *>("channelChip"))
                    if (chip->property("address").toString() == fresh)
                        ++rowsFor;
                expect(rowsFor == 1 && row(fresh) && row(fresh)->preview() == "first words",
                       "a started chat that gets a message is the same row, not another");
            }
            // A long conversation opens as a window of its newest messages, and a page of older
            // ones comes each time the reader scrolls up to the top of what is shown.
            {
                const auto longPeer = bm::Protocol::identity("long chat peer").address;
                arrive([&](bm::Mailbox &m) {
                    bm::Mailbox::Batch batch(m);
                    for (int n = 1; n <= 250; ++n)
                        m.storeChat("long-" + QString::number(n), longPeer, personal, n,
                                    "long chat message " + QString::number(n) + " end", expiry,
                                    false);
                    batch.commit();
                });
                window.findChild<QToolButton *>("folderIcon_Inbox")->click();
                chatsIcon->click();
                if (auto chip = chipOf(longPeer))
                    chip->click();
                QTest::qWait(50);
                auto thread = window.findChild<QScrollArea *>("chatMessagesScroll");
                auto bar = thread->verticalScrollBar();
                const auto bodiesNow = [&] { return window.findChildren<QLabel *>("chatBody"); };
                const auto has = [&](int n) {
                    for (auto body : bodiesNow())
                        if (body->text().contains("long chat message " + QString::number(n) + " end"))
                            return true;
                    return false;
                };
                const int window = bm::kChatWindow, page = bm::kChatPage;
                expect(bodiesNow().size() == window && has(250) && has(250 - window + 1) &&
                           !has(250 - window),
                       "a long conversation opens as a window of its newest messages");
                // A message's label is as tall as a plain QLabel makes it, in this window's style:
                // anything else clips its text, or leaves a gap under it.
                int unequal = 0, wider = 0;
                for (auto body : bodiesNow()) {
                    QLabel twin(body->text(), body->parentWidget());
                    twin.setTextFormat(Qt::RichText);
                    twin.setWordWrap(true);
                    twin.ensurePolished();
                    unequal += twin.heightForWidth(body->width()) != body->height();
                    wider += body->width() < body->minimumWidth();
                }
                expect(unequal == 0 && wider == 0,
                       "a message's label is as tall as QLabel makes it, as wide as its minimum");
                bar->setValue(0);
                QTest::qWait(100);
                expect(bodiesNow().size() == window + page && has(250) &&
                           has(250 - window - page + 1) && !has(250 - window - page),
                       "scrolled up to the top, it brings in a page of older messages");
                for (int more = 0; more < 12; ++more) {
                    bar->setValue(0);
                    QTest::qWait(100);
                }
                expect(bodiesNow().size() == 250 && has(1) && has(250),
                       "and again, until it has all of them");
                bar->setValue(0);
                QTest::qWait(100);
                expect(bodiesNow().size() == 250, "with all of them shown, nothing more is asked for");
                if (auto chip = chipOf(chatAddress))
                    chip->click();
                QTest::qWait(50);
                if (auto chip = chipOf(longPeer))
                    chip->click();
                QTest::qWait(50);
                expect(bodiesNow().size() == window && has(250) && !has(250 - window),
                       "a conversation opened again is a window of its newest messages again");
            }
            // Two identities, two chats: one peer under each identity is a chat of its own,
            // with its own row and thread, and a row says which identity it is under.
            {
                auto work = bm::Protocol::identity("chat second identity");
                work.label = "Work";
                const auto identitiesBefore = session.identities().size();
                expect(session.addIdentity(work), "a second identity joins the vault");
                arrive([&](bm::Mailbox &m) {
                    m.storeChat("two-1", chatAddress, personal, 101, "to Personal", expiry, false);
                    m.storeChat("two-2", chatAddress, work.address, 102, "to Work", expiry, false);
                });
                QTest::qWait(30);
                bm::ChatRow *underPersonal = nullptr, *underWork = nullptr;
                int sharing = 0;
                for (auto chip : sharedRail->findChildren<QPushButton *>("channelChip"))
                    if (auto r = qobject_cast<bm::ChatRow *>(chip);
                        r && r->property("address").toString() == chatAddress) {
                        ++sharing;
                        if (r->via() == personal)
                            underPersonal = r;
                        if (r->via() == work.address)
                            underWork = r;
                    }
                expect(sharing == 2 && underPersonal && underWork &&
                           underPersonal->preview() == "to Personal" &&
                           underWork->preview() == "to Work",
                       "one peer under two identities is two chats, each row saying which");
                auto pane = window.findChild<bm::ChatsPane *>("chatsPane");
                auto chip = window.findChild<QFrame *>("chatIdentity");
                const auto bodiesShown = [&] {
                    QStringList texts;
                    for (auto label : window.findChildren<QLabel *>("chatBody"))
                        texts << label->text();
                    return texts.join(" ");
                };
                if (underWork)
                    underWork->click();
                QTest::qWait(30);
                auto name = chip ? chip->findChild<QLabel *>("chatIdentityName") : nullptr;
                expect(pane && name && pane->identity() == work.address &&
                           pane->contact() == chatAddress && chip->isVisible() &&
                           name->text() == "Work" && bodiesShown().contains("to Work") &&
                           !bodiesShown().contains("to Personal"),
                       "a chat shows its own thread, under its own identity");
                if (underPersonal)
                    underPersonal->click();
                QTest::qWait(30);
                expect(pane && pane->identity() == personal &&
                           bodiesShown().contains("to Personal") &&
                           !bodiesShown().contains("to Work"),
                       "and the other chat its own");
                {
                    bm::NewChatDialog dialog(session, false);
                    dialog.show();
                    auto box = dialog.findChild<QComboBox *>("chatIdentityBox");
                    expect(box && box->isVisibleTo(&dialog) && box->count() == 2,
                           "with several identities a new chat asks which, once");
                    if (box)
                        box->setCurrentIndex(box->findData(work.address));
                    if (auto field = dialog.findChild<QLineEdit *>("chatPeerField"))
                        field->setText(bm::Protocol::identity("new chat under work").address);
                    expect(dialog.identity() == work.address, "and the chat starts under it");
                }
                answer = QMessageBox::Yes;
                session.deleteIdentity(work.address);
                QTest::qWait(30);
                expect(session.identities().size() == identitiesBefore,
                       "the extra identity is deleted again");
            }
            answering.stop();
            QDesktopServices::unsetUrlHandler("https");
            QDesktopServices::unsetUrlHandler("file");
        }
        const auto chatFaultText = chatFaults.join("; ").toStdString();
        require(chatFaults.isEmpty(), chatFaultText.c_str());
        session.removeContact(chatAddress);
        QCoreApplication::processEvents();
        require(window.findChild<QLabel *>("chatTitle")->text() == chatAddress,
                "A contact removed leaves the chat, now named by its address");
        window.findChild<QToolButton *>("folderIcon_Inbox")->click();
        require(!chatsPane->isVisible() && window.findChild<QWidget *>("listColumn")->isVisible(),
                "Returning to Inbox restores the mail list");
        folders->setCurrentRow(4);
        QTest::qWait(50);
        footprint("Mailbox open");
        auto outboxIcon = window.findChild<QToolButton *>("folderIcon_Outbox");
        require(session.messageCount("Outbox", "") == 0 && outboxIcon && !outboxIcon->isVisible(),
                "with nothing queued the Outbox stays out of the rail");
        // The buttons beside an address (copy it, add it to contacts) are drawn
        // like the app's other tool buttons, in every place: a 22 px box with a
        // 14 px icon, bordered in the theme's border colour, light or dark.
        const auto near = [](QColor a, QColor b) {
            return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) +
                       qAbs(a.blue() - b.blue()) <=
                   24;
        };
        const auto copyEdge = [](QToolButton *b) {
            return b->grab().toImage().pixelColor(0, b->height() / 2);
        };
        const auto copyStyled = [&](QToolButton *b) {
            return b && b->size() == QSize(22, 22) && b->iconSize() == QSize(14, 14) &&
                   (near(copyEdge(b), QColor("#dbe3e8")) || near(copyEdge(b), QColor("#354553")));
        };
        // A card's icon buttons sit side by side in one bordered group (drawn as
        // a feed card's actions: compared with one below).
        const auto inOnePill = [&](const QList<QToolButton *> &buttons) {
            auto pill = buttons.first() ? buttons.first()->parentWidget() : nullptr;
            if (!pill)
                return false;
            for (int i = 0; i < buttons.size(); ++i)
                if (!buttons[i] || buttons[i]->parentWidget() != pill ||
                    buttons[i]->y() != buttons.first()->y() ||
                    (i > 0 && buttons[i]->x() <= buttons[i - 1]->x()))
                    return false;
            return pill->grab().toImage().pixelColor(0, pill->height() / 2).alpha() == 255;
        };
        // Ink in an 80 px icon: opaque pixels in all, and the widest opaque run
        // of any row in its bottom fifth (a line under the drawing).
        const auto ink = [](const QIcon &icon) {
            const auto img =
                icon.pixmap(QSize(80, 80)).toImage().convertToFormat(QImage::Format_ARGB32);
            int all = 0, underline = 0;
            for (int y = 0; y < img.height(); ++y) {
                int row = 0;
                for (int x = 0; x < img.width(); ++x)
                    if (qAlpha(img.pixel(x, y)) >= 200)
                        ++row;
                all += row;
                if (y >= img.height() * 4 / 5)
                    underline = qMax(underline, row);
            }
            return std::pair{all, underline};
        };
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
            {
                // Switching chans moves the selection without rebuilding the
                // rail: rebuilt, it painted empty for a moment, a blink.
                struct ChipPaints : QObject {
                    QWidget *holder = nullptr;
                    int bare = 0; // paints that found the rail without its chips
                    bool eventFilter(QObject *watched, QEvent *event) override {
                        if (watched == holder && event->type() == QEvent::Paint) {
                            int shown = 0;
                            for (auto chip : holder->findChildren<QPushButton *>("channelChip"))
                                shown += chip->isVisible();
                            bare += shown == 0;
                        }
                        return false;
                    }
                } paints;
                QList<QPointer<QPushButton>> kept;
                QPushButton *active = nullptr, *other = nullptr;
                for (auto chip : chips) {
                    kept << chip;
                    (chip->isChecked() ? active : other) = chip;
                }
                paints.holder = active->parentWidget();
                paints.holder->installEventFilter(&paints);
                other->click();
                QTest::qWait(50);
                require(paints.bare == 0, "switching chans never paints the rail without its chips");
                bool same = true;
                for (const auto &chip : kept)
                    same = same && chip;
                require(same, "switching chans keeps the rail's chips; only the selection moves");
                require(other->isChecked() && !active->isChecked(),
                        "a click on a chan selects it, and only it");
                active->click();
                QTest::qWait(50);
                active->click();
                QTest::qWait(50);
                paints.holder->removeEventFilter(&paints);
                require(active->isChecked() && !other->isChecked(),
                        "a click on the selected chan keeps it selected");
                require(paints.bare == 0, "and the rail never paints without them");
            }
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
            const int folderBefore = folders->currentRow();
            auto openSettings = [&] {
                window.findChild<QAction *>("settingsAction")->trigger();
                auto settings = window.findChild<QWidget *>("settingsPane");
                require(settings && settings->isVisible(),
                        "Settings opens as a page of the window");
                return settings;
            };
            auto themeTo = [&](const char *mode) {
                auto combo = openSettings()->findChild<QComboBox *>("themeCombo");
                combo->setCurrentIndex(combo->findData(QString(mode)));
            };
            themeTo("light");
            {
                auto language = openSettings()->findChild<QComboBox *>("languageCombo");
                require(language && language->count() == bm::languages().size() + 1,
                        "Settings lists System default and every language");
                for (auto action : window.menuBar()->actions())
                    require(action->text() != "Appearance" && action->text() != "Language",
                            "theme and language live in Settings, not in the menu bar");
            }
            {
                // The Notifications page lists the sound themes, the built-in one
                // first, with a switch and a preview for each event.
                auto settings = openSettings();
                auto themes = settings->findChild<QListWidget *>("soundThemesList");
                require(themes && themes->count() == 1 &&
                            themes->item(0)->data(Qt::UserRole).toString() == "retro" &&
                            themes->item(0)->text().contains("Retro"),
                        "Settings lists the built-in sound theme");
                require(settings->findChild<QCheckBox *>("soundEnabledCheck") &&
                            settings->findChild<QCheckBox *>("soundEvent_new-chat") &&
                            settings->findChild<QCheckBox *>("soundEvent_new-mail") &&
                            settings->findChild<QPushButton *>("soundPreview_new-chat") &&
                            settings->findChild<QPushButton *>("addSoundThemeButton"),
                        "the sound switch, both events and the add-theme button are all there");
                // The built-in theme was unpacked from the compiled-in zip, so both
                // of its sounds are on disk and the notifier can find them.
                auto &sounds = window.soundNotifier();
                require(!bm::soundFileFor(sounds.currentTheme(), "new-chat").isEmpty() &&
                            !bm::soundFileFor(sounds.currentTheme(), "new-mail").isEmpty(),
                        "the built-in theme unpacks both of its sounds");
                // The shipped sounds must be in a format this output device
                // renders. Qt does not resample a sound effect, so a 22050 Hz
                // file loads Ready and then plays into silence.
                for (const auto &event : bm::soundEvents()) {
                    QFile file(bm::soundFileFor(sounds.currentTheme(), event.id));
                    QString why;
                    require(file.open(QIODevice::ReadOnly) &&
                                bm::soundFileIsPlayable(file.readAll(), event.file,
                                                        sounds.audioLimits(), &why),
                            "the built-in theme's sounds are playable on this output device");
                }
                auto enabled = settings->findChild<QCheckBox *>("soundEnabledCheck");
                enabled->setChecked(false);
                require(!sounds.enabled(), "the sound switch is remembered");
                enabled->setChecked(true);
                require(sounds.enabled(), "turning sounds back on is remembered");
                auto mail = settings->findChild<QCheckBox *>("soundEvent_new-mail");
                mail->setChecked(false);
                require(!sounds.eventEnabled("new-mail"), "an event can be silenced on its own");
                mail->setChecked(true);
                require(sounds.eventEnabled("new-mail"), "and turned back on");
                // The Play button must hand the event's file to a sound effect that
                // loads: this is the whole path a real arrival takes.
                require(sounds.findChildren<QSoundEffect *>().isEmpty(),
                        "no sound effect exists before anything is played");
                settings->findChild<QPushButton *>("soundPreview_new-chat")->click();
                const auto effects = sounds.findChildren<QSoundEffect *>();
                require(effects.size() == 1,
                        "the preview button builds exactly one sound effect");
                require(effects.first()->source().toLocalFile().endsWith("/new-chat.wav"),
                        "the preview button hands the event's own file to the effect");
                // A machine with no sound output (a CI runner) can't take a sound to Ready.
                if (!QMediaDevices::audioOutputs().isEmpty())
                    require(QTest::qWaitFor(
                                [&] { return effects.first()->status() == QSoundEffect::Ready; }, 5000),
                            "the preview's sound loads ready to play");
                settings->findChild<QPushButton *>("soundPreview_new-mail")->click();
                require(sounds.findChildren<QSoundEffect *>().size() == 2,
                        "the other event loads its own sound effect");
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
            auto headerCopy = window.findChild<QToolButton *>("feedHeaderCopy");
            require(copyStyled(headerCopy) && near(copyEdge(headerCopy), QColor("#dbe3e8")),
                    "the feed's copy-address button is drawn like the other tool buttons");
            require(copyStyled(window.findChild<QToolButton *>("addSenderContact")) &&
                        copyStyled(window.findChild<QToolButton *>("addRecipientContact")),
                    "the reader's add-to-contacts buttons are drawn like the other tool buttons");
            themeTo("dark");
            require(iconOf("density_compact") != lightDensity,
                    "density icons recolour on a theme switch");
            require(iconOf("filter_unread") != lightFilter,
                    "filter icons recolour on a theme switch");
            require(near(copyEdge(headerCopy), QColor("#354553")),
                    "a copy-address button's border follows the theme");
            themeTo("system");
            folders->setCurrentRow(folderBefore);
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
            auto poppedStripe = popped->findChild<QWidget *>("letterKindStripe");
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
        waitComposerGone(window);
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
            const int folderBefore = folders->currentRow();
            window.findChild<QAction *>("settingsAction")->trigger();
            auto settingsPane = window.findChild<QWidget *>("settingsPane");
            require(settingsPane, "Settings opens as a page of the window");
            auto toggle = settingsPane->findChild<QCheckBox *>("updateNoticesCheck");
            require(banner && !banner->isHidden() &&
                        window.findChild<QLabel *>("updateLabel")->text().contains("0.6.0"),
                    "a newer announced version shows the update banner");
            require(toggle && toggle->isEnabled() && toggle->isChecked(),
                    "update notices are on by default");
            toggle->click();
            require(banner->isHidden() && !session.updateNotices(),
                    "turning notices off hides the banner");
            toggle->click();
            require(!banner->isHidden(), "turning notices on again shows it");
            window.findChild<QPushButton *>("updateDismissButton")->click();
            require(banner->isHidden(), "a dismissed version is not shown again");
            QCoreApplication::setApplicationVersion("0.6.0");
            session.clearError();
            require(banner->isHidden(), "the running version is not announced as new");
            bm::updates::setPublisherAddressForTesting({});
            session.clearError();
            require(!toggle->isEnabled(), "a build without a publisher has nothing to toggle");
            folders->setCurrentRow(folderBefore);
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
            require(bm::hexDump("ABCDEFGHIJKLMNOPQ\x01") ==
                        "00000000  41 42 43 44 45 46 47 48  49 4a 4b 4c 4d 4e 4f 50  "
                        "|ABCDEFGHIJKLMNOP|\n"
                        "00000010  51 01" +
                            QString(45, ' ') + "|Q.|\n",
                    "a dump line: offset, sixteen bytes split eight and eight, the printable "
                    "ones; a short last line keeps the columns");
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
            // ynotbit's signature means the letter came from its Markdown composer.
            window.selectMessage(session.saveLetter({}, address, address, "Empty",
                                                    "-- sent by y*notbit*", "direct"));
            require(mode("view_markdown")->isChecked(),
                    "a letter holding only ynotbit's signature opens in markdown mode");
            window.selectMessage(session.saveLetter({}, address, address, "Short",
                                                    "See you at 5.\n\n-- sent by y*notbit*",
                                                    "direct"));
            require(mode("view_markdown")->isChecked(),
                    "plain words above ynotbit's signature open in markdown mode too");
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
            auto popViews = popout->findChild<QWidget *>("viewSwitch");
            require(popViews, "the pop-out window has its own view switch");
            auto popMode = [&](const char *id) { return popViews->findChild<QToolButton *>(id); };
            auto popBody = popout->findChild<QTextBrowser *>("readerBody");
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
        require(popped->findChild<QTextBrowser *>("readerBody")->toPlainText().contains(
                    "A readable list"),
                "separate window renders the same markdown body");
        // Subject, details and body scroll as one: the scrollbar is the reader's,
        // always shown, so text never re-wraps when it appears.
        const auto readerScroll = [](QWidget *body) {
            QScrollArea *scroll = nullptr;
            for (auto w = body->parentWidget(); w && !scroll; w = w->parentWidget())
                scroll = qobject_cast<QScrollArea *>(w);
            return scroll;
        };
        auto scroll = readerScroll(reader);
        auto poppedReaderBody = popped->findChild<QTextBrowser *>("readerBody");
        auto poppedScroll = readerScroll(poppedReaderBody);
        require(scroll && scroll->objectName() == "readerScroll" &&
                    scroll->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOn &&
                    reader->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
                "main window reader always shows its scrollbar; the body has none of its own");
        require(poppedScroll && poppedScroll != scroll &&
                    poppedScroll->objectName() == "readerScroll" &&
                    poppedScroll->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOn &&
                    poppedReaderBody->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
                "separate window reader always shows its scrollbar; the body has none of its own");
        require(window.findChild<QToolBar *>("actionsToolbar")->mapTo(&window, QPoint()).y() <
                    window.findChild<QTextEdit *>("subject")->mapTo(&window, QPoint()).y(),
                "main window toolbar renders above the subject");
        require(popped->findChild<QToolBar *>("actionsToolbar")->mapTo(popped, QPoint()).y() <
                    popped->findChild<QTextEdit *>("subject")
                        ->mapTo(popped, QPoint())
                        .y(),
                "separate window toolbar renders above the subject");
        popped->close();
        {
            // A long letter: scrolled down, the body gets the whole pane, while
            // the toolbar stays.
            QStringList lines;
            for (int i = 1; i <= 300; ++i)
                lines << QString("Line %1 of a long letter.").arg(i);
            const auto longLetter =
                session.saveLetter({}, address, address, "A long letter", lines.join('\n'), "direct");
            const auto otherLong = session.saveLetter({}, address, address, "Another long letter",
                                                      lines.join("\n\n"), "direct");
            // Finishes laying out the letter shown: a long one is laid out in
            // steps after it first appears, and the body grows as they report.
            const auto settle = [&] {
                const int full = qCeil(reader->document()->size().height()); // the rest, now
                QElapsedTimer limit;
                limit.start();
                while (reader->minimumHeight() < full && limit.elapsed() < 5000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QCoreApplication::processEvents();
            };
            window.selectMessage(longLetter);
            settle();
            auto bar = scroll->verticalScrollBar();
            auto viewport = scroll->viewport();
            auto subject = window.findChild<QTextEdit *>("subject");
            auto toolbar = window.findChild<QToolBar *>("actionsToolbar");
            const int toolbarY = toolbar->mapTo(&window, QPoint()).y();
            require(bar->maximum() > 0 && reader->verticalScrollBar()->maximum() == 0,
                    "a long letter scrolls the reader, not the body on its own");
            require(bar->value() == 0 && subject->mapTo(viewport, QPoint()).y() >= 0,
                    "a letter opens at the top, its subject in view");
            bar->setValue(bar->maximum());
            require(subject->mapTo(viewport, QPoint(0, subject->height())).y() <= 0,
                    "scrolled down, the subject is out of view");
            require(reader->mapTo(viewport, QPoint(0, reader->height())).y() <= viewport->height(),
                    "and the end of the body is in view");
            require(toolbar->mapTo(&window, QPoint()).y() == toolbarY,
                    "the toolbar stays put while the letter scrolls");
            window.selectMessage(otherLong);
            settle();
            require(bar->maximum() > 0 && bar->value() == 0,
                    "the next letter opens at the top, not where the last one was left");
            bar->setValue(bar->maximum());
            window.findChild<QWidget *>("viewSwitch")->findChild<QToolButton *>("view_text")->click();
            QCoreApplication::processEvents();
            require(bar->value() == 0, "and so does another view of the same letter");
            window.selectMessage(acknowledged); // "A delivered letter."
            QCoreApplication::processEvents();
            require(bar->value() == 0 && bar->maximum() == 0, "a short letter opens unscrolled");
            require(reader->mapTo(viewport, QPoint(0, reader->height())).y() == viewport->height(),
                    "and its body fills the rest of the pane");

            window.selectMessage(longLetter);
            settle();
            const auto visible = reader->visibleRegion().boundingRect();
            QTest::wheelEvent(window.windowHandle(), reader->mapTo(&window, visible.center()),
                              QPoint(0, -120));
            QCoreApplication::processEvents();
            require(bar->value() > 0, "the wheel over the body scrolls the whole reader");

            bar->setValue(0);
            QTest::keyClick(reader, Qt::Key_PageDown);
            require(bar->value() == bar->pageStep(), "Page Down in the body pages the reader");
            QTest::keyClick(reader, Qt::Key_End);
            require(bar->value() == bar->maximum(), "End goes to the end of the letter");
            QTest::keyClick(reader, Qt::Key_Home);
            require(bar->value() == 0, "Home goes back to the top");
            QTest::keyClick(reader, Qt::Key_Space);
            require(bar->value() == bar->pageStep(), "Space pages too");

            // Selecting past the bottom edge scrolls on, the selection following.
            bar->setValue(0);
            auto bodyPort = reader->viewport();
            const QPoint start = bodyPort->mapFrom(viewport, QPoint(20, viewport->height() - 40));
            const QPoint below = bodyPort->mapFrom(viewport, QPoint(20, viewport->height() + 30));
            QTest::mousePress(bodyPort, Qt::LeftButton, {}, start);
            QMouseEvent drag(QEvent::MouseMove, below, bodyPort->mapToGlobal(below), Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(bodyPort, &drag);
            const int firstEnd = reader->textCursor().position();
            QTest::qWait(300);
            require(bar->value() > 0 && reader->textCursor().position() > firstEnd,
                    "dragging a selection past the bottom scrolls the reader and extends the selection");
            QTest::mouseRelease(bodyPort, Qt::LeftButton, {}, below);
            const int stopped = bar->value();
            QTest::qWait(150);
            require(bar->value() == stopped, "releasing the button stops the scrolling");

            // Leaving a hex letter must not lay its dump out again in the next
            // letter's font and wrapping: the body is as tall as its text, so
            // that is all of it, seconds for a big dump.
            QByteArray bytes(64 * 1024, 0);
            QRandomGenerator random(7);
            for (auto &c : bytes)
                c = char(random.bounded(256));
            window.selectMessage(session.saveLetter({}, address, address, "Noise",
                                                    QString::fromLatin1(bytes), "direct"));
            const int shownAt = reader->minimumHeight();
            settle();
            require(window.findChild<QWidget *>("viewSwitch")
                        ->findChild<QToolButton *>("view_hex")
                        ->isChecked(),
                    "big noise opens in hex (test sanity)");
            require(shownAt < reader->minimumHeight() &&
                        reader->verticalScrollBar()->maximum() == 0,
                    "a big dump is shown before all of it is laid out, then the body holds all of it");
            qreal tallest = 0;
            const auto watch = QObject::connect(
                reader->document()->documentLayout(),
                &QAbstractTextDocumentLayout::documentSizeChanged,
                [&tallest](const QSizeF &size) { tallest = qMax(tallest, size.height()); });
            window.selectMessage(acknowledged); // "A delivered letter."
            QObject::disconnect(watch);
            require(tallest <= 2 * reader->document()->size().height(),
                    "leaving a hex letter does not lay its dump out again");
        }
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
        auto subjectScroll = longPopped->findChild<QTextEdit *>("subject");
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
        // Qt measures a button in the font of its plain state: a bold :default
        // style would cut the text of every default button, and of any dialog
        // button while it has the focus. Default buttons are bold by their own font.
        QStringList cutDefaults;
        static const QRegularExpression stateFont(R"(QPushButton[^{}]*:[a-z-]+[^{}]*\{[^}]*font)");
        if (stateFont.match(window.styleSheet()).hasMatch())
            cutDefaults << "the style sheet changes a button's font with its state";
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
            // A letter's own window has the same button, and the same dialog.
            {
                for (auto action : window.findChildren<QAction *>())
                    if (action->text() == "Open in new window")
                        action->trigger();
                auto popout = window.findChildren<QDialog *>("messageWindow").last();
                auto popAdd = popout->findChild<QToolButton *>("addRecipientContact");
                require(popAdd && popAdd->isVisible(),
                        "a letter's own window offers to add an unknown recipient");
                QTimer::singleShot(30, &window, [&] {
                    auto dialog = window.findChild<QDialog *>("contactDialog");
                    require(dialog->findChild<QLineEdit *>("contactNameField")->text() ==
                                    bm::addressTitle(alice) &&
                                dialog->findChild<QLabel *>("contactNameHint")->isVisible(),
                            "from a letter's own window the dialog offers the name too");
                    dialog->findChild<QPushButton *>("contactCancelButton")->click();
                });
                popAdd->click();
                require(!session.isContact(alice), "cancelling the dialog adds no contact");
                popout->close();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
            QTimer::singleShot(30, &window, [&] {
                auto dialog = window.findChild<QDialog *>("contactDialog");
                require(dialog && dialog->findChild<QLineEdit *>("contactAddressField")->text() == alice,
                        "the add dialog arrives with the address filled in");
                auto name = dialog->findChild<QLineEdit *>("contactNameField");
                require(name->text() == bm::addressTitle(alice) &&
                            name->selectedText() == name->text() &&
                            dialog->findChild<QLabel *>("contactNameHint")->isVisible(),
                        "the reader's add button brings a dialog offering the address's name");
                auto save = dialog->findChild<QPushButton *>("contactSaveButton");
                require(save->isEnabled(), "a valid new address can be saved straight away");
                if (!boldAndWhole(save))
                    cutDefaults << "the contact dialog's Save";
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
            window.findChild<QToolButton *>("addContactButton")->click();
            // The Add contact dialog offers the name made from a new address (a coolname, in
            // title case), and keeps it only until a name of your own is typed.
            {
                const auto dora = bm::Protocol::identity("Dora for the name offer").address;
                const auto erin = bm::Protocol::identity("Erin for the name offer").address;
                const auto doraName = bm::addressTitle(dora), erinName = bm::addressTitle(erin);
                require(doraName.split(' ').size() >= 3 && doraName.at(0).isUpper() &&
                            doraName != erinName,
                        "test sanity: two addresses, two names of three words or more");
                const auto addDialog = [&](const std::function<void(QDialog *)> &body) {
                    QTimer::singleShot(30, &window, [&, body] {
                        auto dialog = window.findChild<QDialog *>("contactDialog");
                        require(dialog, "the Add contact dialog opens");
                        body(dialog);
                    });
                    window.findChild<QToolButton *>("addContactButton")->click();
                };
                QApplication::clipboard()->clear();
                addDialog([&](QDialog *dialog) {
                    auto name = dialog->findChild<QLineEdit *>("contactNameField");
                    auto addressField = dialog->findChild<QLineEdit *>("contactAddressField");
                    auto hint = dialog->findChild<QLabel *>("contactNameHint");
                    require(hint && name->text().isEmpty() && !hint->isVisible(),
                            "with no address there is no name to offer");
                    addressField->setText(dora);
                    require(name->text() == doraName && hint->isVisible(),
                            "a new address brings the name made from it, marked as a suggestion");                    addressField->setText(erin);
                    require(name->text() == erinName && hint->isVisible(),
                            "another address brings its own name");
                    addressField->setText("BM-garbage");
                    require(name->text().isEmpty() && !hint->isVisible(),
                            "an address that cannot be a contact takes the suggestion back");
                    addressField->setText(alice);
                    require(name->text().isEmpty() && !hint->isVisible(),
                            "an address already in the book gets no suggestion: saving would "
                            "rename it");
                    addressField->setText(dora);
                    name->setText("Dora");
                    require(!hint->isVisible(), "a name of your own is not marked as a suggestion");
                    addressField->setText(erin);
                    require(name->text() == "Dora" && !hint->isVisible(),
                            "a name of your own stays when the address changes");
                    dialog->findChild<QPushButton *>("contactCancelButton")->click();
                });
                addDialog([&](QDialog *dialog) {
                    dialog->findChild<QLineEdit *>("contactAddressField")->setText(dora);
                    dialog->findChild<QPushButton *>("contactSaveButton")->click();
                });
                require(session.nameFor(dora) == doraName,
                        "saving keeps the offered name as the contact's name");
                session.removeContact(dora);
                QCoreApplication::processEvents();
                QApplication::clipboard()->setText(erin);
                addDialog([&](QDialog *dialog) {
                    auto name = dialog->findChild<QLineEdit *>("contactNameField");
                    require(dialog->findChild<QLineEdit *>("contactAddressField")->text() == erin &&
                                name->text() == erinName && name->selectedText() == erinName,
                            "an address on the clipboard arrives with its name offered, selected");
                    dialog->findChild<QPushButton *>("contactCancelButton")->click();
                });
                QApplication::clipboard()->clear();
            }
            // The Contacts page.
            folders->setCurrentRow(9);
            QCoreApplication::processEvents();
            require(window.findChild<QWidget *>("contactsPane")->isVisible() &&
                        !window.findChild<QWidget *>("listColumn")->isVisible() &&
                        !window.findChild<QWidget *>("identitiesPane")->isVisible(),
                    "Contacts is a whole-pane page");
            auto contactTree = window.findChild<QTreeView *>("contactsTree");
            require(contactTree, "the address book is a tree view");
            const auto contactRows = [&] { return contactTree->model()->rowCount(); };
            const auto contactCell = [&](int row, int column) {
                return contactTree->model()->index(row, column);
            };
            const auto contactText = [&](int row, int column) {
                return contactCell(row, column).data().toString();
            };
            const auto rowOf = [&](const QString &contact) {
                for (int row = 0; row < contactRows(); ++row)
                    if (contactText(row, 0) == contact)
                        return row;
                return -1;
            };
            // The local names (third column), top to bottom.
            const auto contactNames = [&] {
                QStringList names;
                for (int row = 0; row < contactRows(); ++row)
                    names << contactText(row, 2);
                return names;
            };
            const auto clickContact = [&](int row, int column) {
                QTest::mouseClick(contactTree->viewport(), Qt::LeftButton, {},
                                  contactTree->visualRect(contactCell(row, column)).center());
            };
            QStringList columns;
            for (int column = 0; column < contactTree->model()->columnCount(); ++column)
                columns << contactTree->model()->headerData(column, Qt::Horizontal).toString();
            require(columns == QStringList({"Address", "Nickname", "Local name"}),
                    "the address book has Address, Nickname and Local name columns");
            require(contactRows() == 1 && contactText(0, 0) == alice &&
                        contactText(0, 1).isEmpty() && contactText(0, 2) == "Alice",
                    "a contact is listed by address, with its local name and no nickname yet");
            // The buttons for the selected contact sit in the toolbar, where Add contact is.
            auto addButton = window.findChild<QToolButton *>("addContactButton");
            auto write = window.findChild<QToolButton *>("writeToContactButton");
            auto chat = window.findChild<QToolButton *>("chatWithContactButton");
            auto copy = window.findChild<QToolButton *>("copyContactAddressButton");
            auto rename = window.findChild<QToolButton *>("renameContactButton");
            auto remove = window.findChild<QToolButton *>("deleteContactButton");
            require(addButton && write && chat && copy && rename && remove,
                    "the toolbar has Write, Chat, Copy address, Rename, Delete and Add contact");
            const QList<QWidget *> rowButtons{write, chat, copy, rename, remove};
            const auto topOf = [&](QWidget *w) { return w->mapTo(&window, QPoint()).y(); };
            for (auto button : rowButtons)
                require(topOf(button) < topOf(contactTree) &&
                            topOf(button) < topOf(addButton) + addButton->height() &&
                            topOf(button) + button->height() > topOf(addButton),
                        "a contact's buttons sit in Add contact's toolbar, above the tree");
            require(addButton->isEnabled(), "Add contact is on with nothing selected");
            for (auto button : rowButtons)
                require(!button->isEnabled(), "with no row selected a contact's buttons are off");
            require(inOnePill({write, chat, copy, rename, remove}),
                    "Write, Chat, Copy address, Rename and Delete sit in one pill");
            require(write->toolTip() == "Write a letter" && chat->toolTip() == "Chat" &&
                        copy->toolTip() == "Copy address" &&
                        rename->toolTip() == "Rename contact" &&
                        remove->toolTip() == "Delete contact",
                    "the pill's buttons say what they act on");
            require(ink(rename->icon()).second >= 32,
                    "a contact's Rename shows a pencil over a line of text");
            // Three contacts: identicons, the address book's order, and sorting by name.
            const auto bob = bm::Protocol::identity("Bob for the address book").address;
            const auto carol = bm::Protocol::identity("Carol for the address book").address;
            require(session.addContact(bob, "bob") && session.addContact(carol, "Carol"),
                    "add two more contacts");
            QCoreApplication::processEvents();
            require(contactRows() == 3, "new contacts are listed at once");
            const auto ratio = contactTree->devicePixelRatioF();
            const auto side = contactTree->iconSize().width();
            for (int row = 0; row < contactRows(); ++row) {
                const auto icon = contactCell(row, 0).data(Qt::DecorationRole).value<QIcon>();
                require(icon.pixmap(contactTree->iconSize(), ratio).toImage() ==
                            bm::identiconPixmap(contactText(row, 0), qRound(side * ratio)).toImage(),
                        "each address is shown with its own identicon");
            }
            // The address column follows the tree's text size: a model's font would keep its
            // own, and addressFont() alone has the platform's fixed-font size, 10 px beside text
            // of 13 on macOS.
            {
                const auto addressWidth = [&] {
                    QCoreApplication::processEvents();
                    contactTree->doItemsLayout(); // the column fits its contents
                    return contactTree->header()->sectionSize(0);
                };
                const auto usual = addressWidth();
                contactTree->setStyleSheet("QTreeView{font-size:24px;}");
                require(addressWidth() > usual + 100, "an address grows with the text of the tree");
                contactTree->setStyleSheet({});
                require(addressWidth() == usual, "and shrinks back with it");
            }
            {
                // And it is in the address font (fixed width), not the font of the other
                // columns: a name that is an address too. Measured as the delegate sizes a cell,
                // against a plain delegate given each font.
                QStyleOptionViewItem option;
                option.initFrom(contactTree);
                option.widget = contactTree;
                option.font = contactTree->font();
                option.fontMetrics = QFontMetrics(option.font);
                option.decorationSize = contactTree->iconSize();
                auto mono = option;
                mono.font.setFamilies(bm::addressFont().families());
                mono.fontMetrics = QFontMetrics(mono.font);
                const auto shown = [&](const QModelIndex &cell) {
                    return contactTree->itemDelegate()->sizeHint(option, cell).width();
                };
                const auto plainIn = [&](const QStyleOptionViewItem &font, const QModelIndex &cell) {
                    return QStyledItemDelegate().sizeHint(font, cell).width();
                };
                require(shown(contactCell(0, 0)) == plainIn(mono, contactCell(0, 0)),
                        "an address is in the address font, at the size of the tree's text");
                require(shown(contactCell(0, 2)) == plainIn(option, contactCell(0, 2)),
                        "a name is in the tree's own font");
                const auto dave = bm::Protocol::identity("Dave without a name").address;
                require(session.addContact(dave, ""), "add a contact without a name");
                QCoreApplication::processEvents();
                const auto row = rowOf(dave);
                require(row >= 0 && contactText(row, 2) == dave &&
                            shown(contactCell(row, 2)) == plainIn(mono, contactCell(row, 2)),
                        "a name that is an address is in the address font too");
                session.removeContact(dave);
                QCoreApplication::processEvents();
            }
            QStringList book, shown;
            for (const auto &c : session.contacts())
                book << c.toMap()["address"].toString();
            for (int row = 0; row < contactRows(); ++row)
                shown << contactText(row, 0);
            require(shown == book,
                    "the rows keep the address book's order until a header is clicked");
            const auto header = contactTree->header();
            const auto clickHeader = [&](int column) {
                QTest::mouseClick(header->viewport(), Qt::LeftButton, {},
                                  QPoint(header->sectionViewportPosition(column) + 8,
                                         header->height() / 2));
            };
            clickHeader(2);
            require(contactNames() == QStringList({"Alice", "bob", "Carol"}),
                    "a click on Local name sorts the rows by name, ignoring case");
            clickHeader(2);
            require(contactNames() == QStringList({"Carol", "bob", "Alice"}),
                    "a second click reverses the order");
            session.removeContact(bob);
            session.removeContact(carol);
            QCoreApplication::processEvents();
            require(contactRows() == 1 && contactText(0, 0) == alice,
                    "the extra contacts are gone again");
            // Selecting a row, by any of its cells, turns the buttons on; Copy address copies.
            clickContact(0, 2);
            for (auto button : rowButtons)
                require(button->isEnabled(), "selecting a row turns a contact's buttons on");
            QApplication::clipboard()->clear();
            copy->click();
            require(QApplication::clipboard()->text() == alice,
                    "Copy address copies the selected contact's address");
            auto filter = window.findChild<QLineEdit *>("contactsFilter");
            auto empty = window.findChild<QLabel *>("contactsEmpty");
            require(empty && !empty->isVisible() && contactTree->isVisible(),
                    "with contacts listed there is no empty note");
            filter->setText("nobody-matches");
            require(contactRows() == 0 && empty->isVisible() && !contactTree->isVisible(),
                    "a filter with no match says so");
            for (auto button : rowButtons)
                require(!button->isEnabled(), "a selection the filter hides turns the buttons off");
            filter->setText(alice.mid(5, 6));
            require(contactRows() == 1, "the filter matches addresses too");
            filter->setText("aLICE");
            require(contactRows() == 1, "the filter matches names, ignoring case");
            filter->clear();
            // A nickname (nickbot's name for an address) is in the second column, and found by
            // the filter.
            session.setNickname(alice, "@alice");
            QCoreApplication::processEvents();
            require(contactText(0, 0) == alice && contactText(0, 1) == "@alice",
                    "a contact's nickname is in the Nickname column");
            filter->setText("@ALI");
            require(contactRows() == 1, "the filter matches nicknames, ignoring case");
            filter->setText("@nobody");
            require(contactRows() == 0, "and only what matches");
            filter->clear();
            session.setNickname(alice, "");
            QCoreApplication::processEvents();
            require(contactText(0, 1).isEmpty(), "an empty nickname takes the nickname away");
            session.setNickname("BM-not-an-address", "@x");
            require(!session.error().isEmpty() && session.nickname("BM-not-an-address").isEmpty(),
                    "a nickname is only for a valid address");
            session.clearError();
            // Write opens the composer to the contact, with the address book wired in: from its
            // button, and from a double-click on the row.
            const auto composerToAlice = [&] {
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
            };
            clickContact(0, 2);
            QTimer::singleShot(30, &window, composerToAlice);
            write->click();
            waitComposerGone(window);
            QTimer::singleShot(30, &window, composerToAlice);
            QTest::mouseDClick(contactTree->viewport(), Qt::LeftButton, {},
                               contactTree->visualRect(contactCell(0, 2)).center());
            waitComposerGone(window);
            // A theme switch recolours the toolbar's icons, and the page keeps its selection.
            {
                const auto themeCombo = [&] {
                    window.findChild<QAction *>("settingsAction")->trigger();
                    return window.findChild<QWidget *>("settingsPane")
                        ->findChild<QComboBox *>("themeCombo");
                };
                const auto before = themeCombo()->currentData().toString();
                const auto themeTo = [&](const QString &mode) {
                    auto combo = themeCombo();
                    combo->setCurrentIndex(combo->findData(mode));
                    folders->setCurrentRow(9);
                    QCoreApplication::processEvents();
                };
                const QList<QToolButton *> toolbar{
                    write,
                    chat,
                    copy,
                    rename,
                    remove,
                    window.findChild<QToolButton *>("findPeopleButton"),
                    window.findChild<QToolButton *>("checkNicknamesButton"),
                    addButton};
                themeTo("light");
                QList<QImage> light;
                for (auto b : toolbar)
                    light << b->icon().pixmap(18, 18).toImage();
                const auto writePill = write->parentWidget()->styleSheet(),
                           toolsPill = addButton->parentWidget()->styleSheet();
                themeTo("dark");
                for (int i = 0; i < toolbar.size(); ++i)
                    require(toolbar[i]->icon().pixmap(18, 18).toImage() != light[i],
                            "a theme switch recolours the toolbar's icons");
                require(write->parentWidget()->styleSheet() != writePill &&
                            addButton->parentWidget()->styleSheet() != toolsPill,
                        "and both pills' borders");
                themeTo(before);
            }
            require(write->isEnabled(), "the selected row stays selected when the page refreshes");
            // Rename and Delete act on the selected row, so a second contact goes below the first.
            require(session.addContact(bob, "Bob"), "add a second contact");
            QCoreApplication::processEvents();
            contactTree->sortByColumn(2, Qt::AscendingOrder);
            require(contactNames() == QStringList({"Alice", "Bob"}), "Bob is the second row");
            clickContact(rowOf(bob), 2);
            QTimer::singleShot(30, &window, [&] {
                auto dialog = window.findChild<QDialog *>("contactDialog");
                auto addressField = dialog->findChild<QLineEdit *>("contactAddressField");
                require(addressField->isReadOnly() && addressField->text() == bob,
                        "renaming keeps the address");
                require(dialog->findChild<QLineEdit *>("contactNameField")->text() == "Bob" &&
                            !dialog->findChild<QLabel *>("contactNameHint")->isVisible(),
                        "the dialog starts with the selected contact's name, and no suggestion");
                dialog->findChild<QLineEdit *>("contactNameField")->setText("Bobby");
                dialog->findChild<QPushButton *>("contactSaveButton")->click();
            });
            rename->click();
            QTest::qWait(20);
            require(session.nameFor(bob) == "Bobby" && session.nameFor(alice) == "Alice" &&
                        contactText(rowOf(bob), 2) == "Bobby",
                    "rename updates the selected contact in the book and in the row");
            require(contactTree->selectionModel()->isRowSelected(rowOf(bob), QModelIndex()) &&
                        rename->isEnabled(),
                    "a renamed contact stays selected");
            // The confirmation is a message box on the window; answer it once it shows.
            const auto answer = [&](QMessageBox::StandardButton choice) {
                QTimer timer;
                QObject::connect(&timer, &QTimer::timeout, &window, [&] {
                    for (auto box : window.findChildren<QMessageBox *>())
                        if (box->isVisible())
                            box->button(choice)->click();
                });
                timer.start(10);
                remove->click();
                timer.stop();
                QTest::qWait(20);
            };
            answer(QMessageBox::No);
            require(contactRows() == 2 && session.isContact(bob), "declining keeps the contact");
            answer(QMessageBox::Yes);
            require(contactRows() == 1 && contactText(0, 0) == alice && !session.isContact(bob),
                    "delete removes the selected contact and no other");
            clickContact(0, 2);
            answer(QMessageBox::Yes);
            require(contactRows() == 0 && session.contacts().isEmpty() &&
                        session.nameFor(alice).isEmpty(),
                    "delete removes the contact");
            for (auto button : rowButtons)
                require(!button->isEnabled(),
                        "with the contact gone nothing is selected, and the buttons are off");
            require(empty->isVisible() && !contactTree->isVisible(),
                    "an empty address book says so");
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
            composeAndWait(window);
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
            composeAndWait(window);
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
            composeAndWait(window, {{"hash", bigDraft}});
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
            composeAndWait(window, session.message(threaded), true);
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
            composeAndWait(window, session.message(notes), true);
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
            composeAndWait(window, {{"hash", replyId}});
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
        auto poppedBar =
            readerScroll(bodyPopped->findChild<QTextBrowser *>("readerBody"))->verticalScrollBar();
        require(poppedBar->maximum() > 0, "long body is actually scrollable (test sanity)");
        require(poppedBar->value() == 0,
                "separate window opens scrolled to the beginning of the letter, not the end");
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
        require(dPopped->findChild<QTextEdit *>("subject")->toPlainText() == expectedSubject,
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
        waitComposerGone(window);
        const auto existingDraft = session.saveLetter({}, address, address, "Existing draft",
                                                       "Already written", "direct");
        QTimer::singleShot(30, &window, [&] {
            auto dialog = window.findChild<QDialog *>("composer");
            require(!dialog->findChild<QTextEdit *>("bodyField")->toPlainText().contains("ynotbit"),
                    "reopening an existing draft does not re-inject the signature");
            dialog->reject();
        });
        composeAndWait(window, {{"hash", existingDraft}});
        folders->setCurrentRow(1);
        QCoreApplication::processEvents();
        const auto draftToOpen =
            session.saveLetter({}, address, address, "Draft to reopen", "Unfinished", "direct");
        // compose() does not block: the composer a double-click opens is there once it returns.
        // It is closed here, not from a timer. Closing only schedules a composer's deletion, and
        // the next timer (for the composer opened after this one) found this one still there
        // whenever the thread was busy long enough for both timers to be due in the same pass.
        for (int row = 0; row < list->model()->rowCount(); ++row) {
            auto index = list->model()->index(row, 0);
            if (index.data(Qt::UserRole + 1).toString() != draftToOpen)
                continue;
            emit list->doubleClicked(index);
            break;
        }
        QCoreApplication::processEvents();
        {
            require(!window.findChild<QDialog *>("messageWindow"),
                    "double-clicking a draft does not open the read-only viewer");
            auto draftComposer = window.findChild<QDialog *>("composer");
            require(draftComposer, "double-clicking a draft opens it in the composer instead");
            require(draftComposer->findChild<QLineEdit *>("subjectField")->text() ==
                        "Draft to reopen",
                    "the composer opens with the draft's own content, not a blank letter");
            draftComposer->findChild<QPushButton *>("discardButton")->click();
            waitComposerGone(window);
        }
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
            if (!boldAndWhole(dialog->findChild<QPushButton *>("sendButton")))
                cutDefaults << "the composer's Send";
            for (const auto &cut : cutDefaults)
                std::cerr << qPrintable(cut) << '\n';
            require(cutDefaults.isEmpty(),
                    "default buttons show all of their bold text; no button's font changes with "
                    "its state");
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
        composeAndWait(window);
        require(session.messageCount("Outbox", "") == 1,
                "composer sends through persistent outbox");
        {
            // The Outbox is in the rail while a letter waits in it; emptied while
            // open, it stays until the reader leaves.
            require(outboxIcon->isVisible(), "a queued letter brings the Outbox into the rail");
            const auto queued = session.messageHashes("Outbox").value(0);
            const int folderBefore = folders->currentRow();
            outboxIcon->click();
            session.moveLetter(queued, "Archive");
            require(session.messageCount("Outbox", "") == 0 && outboxIcon->isVisible(),
                    "the open Outbox stays in the rail when it empties");
            window.findChild<QToolButton *>("folderIcon_Inbox")->click();
            require(!outboxIcon->isVisible(), "leaving the empty Outbox takes it out of the rail");
            session.restoreLetter(queued);
            require(session.messageCount("Outbox", "") == 1 && outboxIcon->isVisible(),
                    "a letter back in the Outbox brings it back");
            folders->setCurrentRow(folderBefore);
        }
        const auto draftsBeforeNeverSaved = session.messageCount("Drafts", "");
        QTimer::singleShot(30, &window, [&] {
            window.findChild<QDialog *>("composer")
                ->findChild<QPushButton *>("discardButton")
                ->click();
        });
        composeAndWait(window);
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
        composeAndWait(window, {{"hash", toDiscard}});
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
            auto identitiesTree = window.findChild<QTreeView *>("identitiesTree");
            require(identitiesTree && identitiesTree->model()->rowCount() == 2,
                    "one row per identity");
            const auto identityRow = [&](const QString &addr) {
                for (int row = 0; row < identitiesTree->model()->rowCount(); ++row)
                    if (identitiesTree->model()->index(row, 0).data().toString() == addr)
                        return row;
                return -1;
            };
            const auto identityCell = [&](const QString &addr, int column) {
                return identitiesTree->model()->index(identityRow(addr), column);
            };
            const auto selectIdentity = [&](int row) {
                identitiesTree->setCurrentIndex(identitiesTree->model()->index(row, 0));
            };
            require(identityRow(address) >= 0 && identityRow(emptyChannel) >= 0,
                    "both identities are listed");
            require(
                identityCell(address, 2).data(bm::DesktopWindow::kIdentityDefaultRole).toBool() &&
                    !identityCell(emptyChannel, 2)
                         .data(bm::DesktopWindow::kIdentityDefaultRole)
                         .toBool(),
                "the default identity is marked");
            require(
                identityCell(emptyChannel, 2).data(bm::DesktopWindow::kIdentityChanRole).toBool() &&
                    !identityCell(address, 2).data(bm::DesktopWindow::kIdentityChanRole).toBool(),
                "the chan identity is marked as one");
            auto setDefault = window.findChild<QToolButton *>("setDefaultIdentityButton");
            require(setDefault && !setDefault->isEnabled(),
                    "with the default identity selected Set as default has nothing to do");
            auto identiconFor = [&](const QString &addr) -> QImage {
                return identityCell(addr, 0)
                    .data(Qt::DecorationRole)
                    .value<QIcon>()
                    .pixmap(identitiesTree->iconSize())
                    .toImage();
            };
            auto personalIcon = identiconFor(address);
            auto chanIcon = identiconFor(emptyChannel);
            require(!personalIcon.isNull() && !chanIcon.isNull(), "identicon pixmaps render");
            require(personalIcon.size() == identitiesTree->iconSize(),
                    "identicon scaled to the table's icon size");
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

            {
                auto copy = window.findChild<QToolButton *>("copyAddressButton");
                auto qr = window.findChild<QToolButton *>("showQrButton");
                auto rename = window.findChild<QToolButton *>("renameIdentityButton");
                auto remove = window.findChild<QToolButton *>("deleteIdentityButton");
                require(inOnePill({copy, qr, setDefault, rename, remove}),
                        "an identity's Copy address, QR, Set as default, Rename and Delete sit in "
                        "one pill");
                require(rename->toolTip() == "Rename identity" &&
                            remove->toolTip() == "Delete identity",
                        "the identities' buttons say what they act on");
                const auto [renameInk, renameLine] = ink(rename->icon());
                const auto [editInk, editLine] =
                    ink(window.findChild<QAction *>("editAction")->icon());
                require(renameLine >= 32 && editLine == 0,
                        "Rename shows a pencil over a line of text; Edit / Send, the pencil alone");
                require(editInk >= 450, "Edit / Send's pencil is drawn whole, not a bare stroke");
            }
            QApplication::clipboard()->clear();
            window.findChild<QToolButton *>("copyAddressButton")->click();
            require(QApplication::clipboard()->text() == address,
                    "the copy button copies the selected identity's address");

            selectIdentity(identityRow(emptyChannel));
            require(setDefault->isEnabled(), "another identity can be made the default");
            setDefault->click();
            QTest::qWait(20);
            bool channelIsDefault = false;
            for (auto v : session.identities())
                if (v.toMap()["address"].toString() == emptyChannel)
                    channelIsDefault = v.toMap()["default"].toBool();
            require(channelIsDefault, "Set as default promotes the chosen identity");
            require(identiconFor(address) == personalIcon,
                    "the same address renders the identical identicon after the table is drawn "
                    "again");

            // The personal identity goes: it is not the default now.
            selectIdentity(identityRow(address));
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
            selectIdentity(0);
            window.findChild<QToolButton *>("showQrButton")->click();
            require(qrFound, "the QR button opens a visible QR dialog");

            // New identity opens the new vault dialog, for an identity alone.
            bool identityDialog = false;
            const auto createIdentity = [&] {
                identityDialog = false;
                QTimer::singleShot(30, &window, [&] {
                    if (auto d = window.findChild<QDialog *>("newIdentityDialog");
                        d && d->isVisible()) {
                        identityDialog = true;
                        d->findChild<QPushButton *>("newVaultCreateButton")->click();
                    }
                });
            };
            auto beforeAdd = session.identities().size();
            createIdentity();
            window.findChild<QToolButton *>("newIdentityButton")->click();
            QTest::qWait(50);
            require(identityDialog && session.identities().size() == beforeAdd + 1,
                    "New identity adds the identity its dialog shows");

            selectIdentity(0);
            window.findChild<QToolButton *>("renameIdentityButton")->click();
            QTest::qWait(50);
            bool renamed = false;
            // The first row may be a chan, whose name keeps a '#'.
            for (int row = 0; row < identitiesTree->model()->rowCount(); ++row) {
                const auto name = identitiesTree->model()->index(row, 2).data().toString();
                renamed = renamed || name == "test password" || name == "#test password";
            }
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
            auto cards = [&] { return identitiesTree->model()->rowCount(); };
            auto cardFor = [&](const QString &addr) { return identityRow(addr) >= 0; };
            auto shownCards = cards();
            createIdentity();
            menuItem("Create identity…")->trigger();
            QTest::qWait(50);
            require(identityDialog && cards() == shownCards + 1,
                    "Identity > Create identity… shows the new row on the open Identities page");
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
            // The chan dialog is not a password prompt, so the password responder steps aside.
            responder.stop();
            // Answers a menu item's text prompts in turn.
            std::function<void(QStringList)> answer = [&](QStringList replies) {
                onDialog((QInputDialog *)nullptr, [&, replies](QInputDialog *d) mutable {
                    d->setTextValue(replies.takeFirst());
                    if (!replies.isEmpty())
                        answer(replies);
                    d->accept();
                });
            };
            onDialog((bm::JoinChannelDialog *)nullptr, [&](bm::JoinChannelDialog *d) {
                auto join = d->findChild<QPushButton *>("joinChannelAcceptButton");
                require(!join->isEnabled(), "Join waits for a name");
                // The dialog is as wide as the fields showing, so each kind of chan has its
                // fields as wide as an address (and the style sheet's 24 px) and the dialog
                // keeps its width from one kind to the other.
                const int addressWidth =
                    QFontMetrics(bm::addressFont()).horizontalAdvance(imported) + 24;
                const auto showAnAddress = [&](std::initializer_list<const char *> names) {
                    for (auto name : names)
                        if (d->findChild<QLineEdit *>(name)->width() < addressWidth)
                            return false;
                    return true;
                };
                const int publicWidth = d->width();
                require(showAnAddress({"publicChannelName", "channelAddressField"}),
                        "a public chan's fields are as wide as an address");
                d->findChild<QLineEdit *>("publicChannelName")->setText("widgets menu chan");
                require(d->name() == "#widgets menu chan" && d->phrase() == "widgets menu chan",
                        "a public chan's phrase is its name, listed with a '#'");
                d->findChild<QRadioButton *>("secretChannelButton")->setChecked(true);
                QCoreApplication::processEvents(); // the fixed-size layout resizes the dialog
                require(d->width() >= publicWidth &&
                            showAnAddress({"secretChannelName", "secretChannelPhrase",
                                           "channelAddressField"}),
                        "a secret chan's dialog is no narrower than a public one's, its fields "
                        "as wide as an address");
                auto phrase = d->findChild<QLineEdit *>("secretChannelPhrase");
                require(phrase->echoMode() == QLineEdit::Password && !join->isEnabled(),
                        "a secret chan's phrase is typed hidden, and a name is needed");
                d->findChild<QLineEdit *>("secretChannelName")->setText("hideout");
                phrase->setText("widgets secret chan phrase");
                require(join->isEnabled() && d->name() == "#hideout" &&
                            d->phrase() == "widgets secret chan phrase",
                        "a secret chan: its own '#' name, and the hidden phrase");
                join->click();
            });
            shownCards = cards();
            menuItem("Join or create chan…")->trigger();
            QTest::qWait(50);
            responder.start(10);
            require(session.error().isEmpty() && cards() == shownCards + 1,
                    "Identity > Join or create chan… shows the chan on the open Identities page");
            bool listed = false;
            for (const auto &value : session.identities())
                listed = listed || value.toMap()["label"].toString() == "#hideout";
            require(listed, "the secret chan is listed under its name");

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
            {
                // A card's pill is the feed's action group: buttons of the same
                // size, the same border round them and dividers between them.
                auto feedButton = feedCard("bc-1")->findChild<QToolButton *>("feedReply");
                auto feedLast = feedCard("bc-1")->findChild<QToolButton *>("feedTrash");
                auto feedGroup = feedButton->parentWidget();
                const auto pixel = [](QWidget *w, int x, int y) {
                    return w->grab().toImage().pixelColor(x, y);
                };
                // One column beside the long post: the divider is below.
                const auto divider =
                    pixel(feedButton, feedButton->width() / 2, feedButton->height() - 1);
                const auto border = pixel(feedGroup, 0, feedGroup->height() / 2);
                // A divider adds its pixel to a button, below it in the feed's
                // column, right of it in a card's row; the last button has none.
                const auto likeFeed = [&](QToolButton *b, QToolButton *last) {
                    return b && last && b->size() == feedButton->size().transposed() &&
                           last->size() == feedLast->size() &&
                           b->iconSize() == feedButton->iconSize() &&
                           pixel(b, b->width() - 1, b->height() / 2) == divider &&
                           pixel(b->parentWidget(), 0, b->parentWidget()->height() / 2) == border;
                };
                require(divider.alpha() == 255 && border.alpha() == 255,
                        "test sanity: the feed's group has a border and dividers");
                require(likeFeed(window.findChild<QToolButton *>("renameIdentityButton"),
                                 window.findChild<QToolButton *>("deleteIdentityButton")),
                        "the identities toolbar's pill is drawn as the feed's action group");
                folders->setCurrentRow(9);
                QCoreApplication::processEvents();
                require(likeFeed(window.findChild<QToolButton *>("copyContactAddressButton"),
                                 window.findChild<QToolButton *>("deleteContactButton")),
                        "the contacts toolbar's pill is drawn as the feed's action group");
                folders->setCurrentRow(5);
                QTest::qWait(30);
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
            // Reply privately: a personal letter to the sender, quoting the post. compose() does
            // not block: the composer the click opens is there once the click returns, so it is
            // looked at and closed here, not from a timer that a busy thread may not run in time.
            feedCard("bc-2")->findChild<QToolButton *>("feedReply")->click();
            auto replyComposer = window.findChild<QDialog *>("composer");
            const bool replied =
                replyComposer &&
                replyComposer->findChild<QLineEdit *>("recipientField")->text() == other &&
                replyComposer->findChild<QPushButton *>("modePrivateButton")->isChecked();
            if (replyComposer)
                replyComposer->reject();
            waitComposerGone(window);
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
            // Locked, the rail stays for Settings: the folders need a mailbox,
            // Settings does not.
            auto gear = window.findChild<QToolButton *>("folderIcon_Settings");
            auto welcome = window.findChild<QWidget *>("welcomeStack");
            require(gear && gear->isVisible() && gear->isEnabled(),
                    "the lock screen keeps the rail, with Settings in it");
            require(!window.findChild<QToolButton *>("folderIcon_Inbox")->isEnabled() &&
                        !window.findChild<QToolButton *>("folderIcon_Contacts")->isEnabled() &&
                        !window.findChild<QPushButton *>("writeButton")->isEnabled(),
                    "locked, the rail's folders and Write are off");
            require(!window.findChild<QLineEdit *>("vaultRepeatField"),
                    "the lock screen only unlocks: vaults are made in their own dialog");
            for (const auto &open :
                 {std::function<void()>(
                      [&] { window.findChild<QPushButton *>("createVaultButton")->click(); }),
                  std::function<void()>(
                      [&] { window.findChild<QAction *>("createVaultAction")->trigger(); })}) {
                bool opened = false;
                QTimer::singleShot(30, &window, [&] {
                    if (auto d = window.findChild<QDialog *>("newVaultDialog")) {
                        opened = d->isVisible();
                        d->reject();
                    }
                });
                open();
                require(opened, "Create a new vault… and File > New vault… open the new vault "
                                "dialog");
            }
            require(!window.findChild<QAction *>("createIdentityAction")->isEnabled(),
                    "Identity > Create identity… waits for an open vault");
            const int shown = folders->currentRow();
            folders->setCurrentRow(2);
            require(!outboxIcon->isVisible(),
                    "locked, the Outbox is out of the rail, even as the folder last shown");
            folders->setCurrentRow(shown);
            const int folderBefore = folders->currentRow();
            gear->click();
            auto settings = window.findChild<QWidget *>("settingsPane");
            require(settings && settings->isVisible() && !welcome->isVisible(),
                    "the gear opens Settings in place of the lock screen");
            gear->click();
            require(!settings->isVisible() && welcome->isVisible() &&
                        folders->currentRow() == folderBefore,
                    "the gear again goes back to the lock screen");
            gear->click();
            session.beginVaultUnlock();
            QCoreApplication::processEvents();
            require(!settings->isVisible() && welcome->isVisible(),
                    "a vault asking for its password leaves Settings for the lock screen");
        }
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
        composeAndWait(window, {{"hash", formatted}});
        require(!session.mailboxOpen(), "locking closes composer and mailbox");
        require(!chatsIcon->isEnabled() && !chatsPane->isVisible(),
                "Locking hides Chats and disables its navigation");
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
            require(window.findChild<QToolButton *>("folderIcon_Settings")->isVisible() &&
                        window.findChild<QToolButton *>("folderIcon_Settings")->isEnabled() &&
                        !window.findChild<QToolButton *>("folderIcon_Inbox")->isEnabled(),
                    "with no mailbox open the rail keeps only Settings working");
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
            const int folderBefore = folders->currentRow();
            open->trigger();
            auto settings = window.findChild<QWidget *>("settingsPane");
            require(settings && settings->isVisible() && !settings->isWindow(),
                    "Settings opens as a page of the window, not a window of its own");
            require(!window.findChild<QWidget *>("listColumn")->isVisible() &&
                        !window.findChild<QWidget *>("contactsPane")->isVisible(),
                    "Settings takes the whole pane, like Contacts");
            auto gear = window.findChild<QToolButton *>("folderIcon_Settings");
            auto contactsIcon = window.findChild<QToolButton *>("folderIcon_Contacts");
            auto rail = gear ? gear->parentWidget()->layout() : nullptr;
            require(gear && gear->isChecked() &&
                        rail->indexOf(gear) == rail->indexOf(contactsIcon) + 1,
                    "the rail's gear, right after Contacts, shows Settings is open");
            {
                const auto img = gear->icon()
                                     .pixmap(gear->iconSize())
                                     .toImage()
                                     .convertToFormat(QImage::Format_ARGB32);
                int solid = 0;
                for (int y = 0; y < img.height(); ++y)
                    for (int x = 0; x < img.width(); ++x)
                        if (qAlpha(img.pixel(x, y)) >= 200)
                            ++solid;
                require(solid > 0 && img != contactsIcon->icon()
                                                .pixmap(gear->iconSize())
                                                .toImage()
                                                .convertToFormat(QImage::Format_ARGB32),
                        "the gear has an icon of its own");
            }
            open->trigger();
            require(window.findChildren<QWidget *>("settingsPane").size() == 1 &&
                        settings->isVisible(),
                    "opening Settings again keeps the one page");
            window.findChild<QToolButton *>("folderIcon_Inbox")->click();
            require(!settings->isVisible() && folders->currentRow() == 0 && !gear->isChecked(),
                    "another folder leaves Settings");
            gear->click();
            gear->click();
            require(settings->isVisible(), "with a mailbox open the gear keeps Settings open");
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
            folders->setCurrentRow(folderBefore);
        }
        {
            const int folderBefore = folders->currentRow();
            window.findChild<QAction *>("settingsAction")->trigger();
            auto settings = window.findChild<QWidget *>("settingsPane");
            auto density = settings->findChild<QComboBox *>("densityCombo");
            density->setCurrentIndex(density->findData(QString("compact")));
            require(window.findChild<QToolButton *>("density_compact")->isChecked(),
                    "density chosen in Settings moves the list's density switch");
            window.findChild<QToolButton *>("density_comfortable")->click();
            require(density->currentData().toString() == "comfortable",
                    "the list's density switch moves the Settings choice");
            require(settings->findChild<QCheckBox *>("gpuCheck") &&
                        settings->findChild<QLabel *>("cpuWorkers")->text().contains(
                            QString::number(bm::ProofOfWork::workerCount())),
                    "the Sending page shows the GPU switch and CPU workers");
            require(settings->findChild<QPushButton *>("changePasswordButton")->isEnabled() ==
                        session.unlocked(),
                    "changing the vault password needs an unlocked vault");
            folders->setCurrentRow(folderBefore);
        }
        {
            bm::Session n(temp.filePath("listen-node"), true);
            auto noListen = [&] { return n.nodeArguments().contains("-i"); };
            require(n.listenEnabled() && n.upnpEnabled() && !noListen(),
                    "by default the node listens and UPnP is on");
            n.setListenEnabled(false);
            require(noListen(), "listening off passes -i to the node");
            n.setListenEnabled(true);
            n.setProxy("127.0.0.1:9050");
            require(noListen(), "with a proxy the node never listens");
            n.setProxy("");
            require(!noListen(), "without the proxy it listens again");
        }
        {
            // A service asks for more peers and a port of its own in its node's settings;
            // a desktop asks for none of it and gets the node's defaults.
            const auto root = temp.filePath("service-node");
            QDir().mkpath(root);
            {
                QSettings config(root + "/desktop.ini", QSettings::IniFormat);
                config.setValue("maxOutgoing", 16);
                config.setValue("maxIncoming", 32);
                config.setValue("listenPort", 8555);
            }
            bm::Session service(root, true);
            const auto args = " " + service.nodeArguments().join(' ') + " ";
            require(args.contains(" -o 16 ") && args.contains(" -n 32 ") &&
                        args.contains(" -p 8555 "),
                    "a service's node gets the peer limits and the port it was set to");
            bm::Session desktop(temp.filePath("desktop-node"), true);
            const auto plain = " " + desktop.nodeArguments().join(' ') + " ";
            require(!plain.contains(" -o ") && !plain.contains(" -n ") && !plain.contains(" -p "),
                    "a desktop's node asks for none of it");
        }
        {
            const int folderBefore = folders->currentRow();
            window.findChild<QAction *>("settingsAction")->trigger();
            auto settings = window.findChild<QWidget *>("settingsPane");
            session.setProxy("127.0.0.1:9050");
            require(!settings->findChild<QCheckBox *>("listenCheck")->isEnabled() &&
                        !settings->findChild<QCheckBox *>("upnpCheck")->isEnabled() &&
                        !settings->findChild<QLabel *>("proxyNote")->isHidden(),
                    "a proxy disables listening and UPnP and says why");
            session.setProxy("");
            require(settings->findChild<QCheckBox *>("listenCheck")->isEnabled() &&
                        settings->findChild<QLabel *>("proxyNote")->isHidden(),
                    "without a proxy listening can be switched again");
            folders->setCurrentRow(folderBefore);
        }
        require(bm::Session::wantsPortMapping(true, true, true, {}) &&
                    !bm::Session::wantsPortMapping(false, true, true, {}) &&
                    !bm::Session::wantsPortMapping(true, false, true, {}) &&
                    !bm::Session::wantsPortMapping(true, true, false, {}) &&
                    !bm::Session::wantsPortMapping(true, true, true, "127.0.0.1:9050"),
                "UPnP runs only online, listening, switched on and without a proxy");
        {
            // A vault made with the identity it starts with.
            bm::Session fresh(temp.filePath("made-vault-node"), true);
            const auto made =
                bm::phraseIdentity("ynotbit test phrase", "Mysterious Poised Shellfish", 60);
            const auto path = temp.filePath("made.bmvault");
            require(!fresh.createVault(path, "one password", "another one", made) &&
                        fresh.error().contains("match") && !QFile::exists(path),
                    "mismatched passwords make no vault");
            require(!fresh.createVault(path, "", "", made) && !QFile::exists(path),
                    "an empty password makes no vault");
            require(fresh.createVault(path, "long test password", "long test password", made) &&
                        fresh.unlocked() && QFile::exists(path),
                    "a vault is made, and open");
            const auto identities = fresh.identities();
            require(identities.size() == 1 &&
                        identities[0].toMap()["address"].toString() == made.address &&
                        identities[0].toMap()["label"].toString() ==
                            "Mysterious Poised Shellfish" &&
                        identities[0].toMap()["default"].toBool(),
                    "its one identity is the one shown, with its nickname, as default");
            require(!fresh.addIdentity(made) && fresh.error().contains("already"),
                    "an identity is in a vault once");
            require(fresh.addIdentity(bm::Protocol::identity("Second Me")) &&
                        fresh.identities().size() == 2,
                    "another identity joins the open vault");
            require(!bm::Session(temp.filePath("no-vault-node"), true).addIdentity(made),
                    "without an open vault no identity is added");
            require(!fresh.createVault(temp.filePath("nowhere/second.bmvault"), "pw", "pw", made) &&
                        fresh.unlocked() && fresh.vaultPath() == path,
                    "a vault that cannot be made leaves the open one open");
            const auto second = temp.filePath("second.bmvault");
            require(fresh.createVault(second, "pw", "pw", made) && fresh.unlocked() &&
                        fresh.vaultPath() == second,
                    "a vault made while another is open takes its place");
            fresh.lock();
            require(!fresh.createVault(path, "pw", "pw", made) &&
                        fresh.error().contains("existing vaults"),
                    "an existing vault file is never overwritten");
#ifndef Q_OS_WIN // there QFile::link makes a shortcut beside the name, not a link to lead through
            const auto link = temp.filePath("dangling.bmvault");
            require(QFile::link(temp.filePath("nowhere/vault.bmvault"), link),
                    "make a link to nowhere");
            require(!fresh.createVault(link, "pw", "pw", made) && QFileInfo(link).isSymLink(),
                    "a failed create leaves what was at the path");
#endif
            auto forged = bm::phraseIdentity("ynotbit test phrase", "x", 61);
            forged.address = made.address;
            const auto half = temp.filePath("half.bmvault");
            require(!fresh.createVault(half, "pw", "pw", forged) && !QFile::exists(half) &&
                        !QFile::exists(half + ".lock"),
                    "a vault left half made is removed, its lock too");
        }
        {
            // A mailbox from before the address book was filled: its Sent folder is the only
            // record of whom the user wrote to, and its Inbox of who wrote to them. Opening it
            // adds them once, each named from its address.
            const auto vaultFile = temp.filePath("sent-vault.bmvault");
            const auto mailboxFile = temp.filePath("sent-mailbox.bmmail");
            const auto laterFile = temp.filePath("sent-later.bmmail");
            const auto dora = bm::Protocol::identity("Dora written to").address;
            const auto erin = bm::Protocol::identity("Erin written to").address;
            const auto frank = bm::Protocol::identity("Frank who wrote").address;
            const auto gina = bm::Protocol::identity("Gina in the archive").address;
            const auto hana = bm::Protocol::identity("Hana written to later").address;
            const auto ivan = bm::Protocol::identity("Ivan who wrote twice").address;
            const auto jack = bm::Protocol::identity("Jack on a channel").address;
            const auto kim = bm::Protocol::identity("Kim broadcasting").address;
            const auto lena = bm::Protocol::identity("Lena in the trash").address;
            const auto mona = bm::Protocol::identity("Mona in the archive").address;
            const auto nora = bm::Protocol::identity("Nora written to earlier").address;
            const auto owen = bm::Protocol::identity("Owen who wrote later").address;
            const auto pia = bm::Protocol::identity("Pia who wrote later still").address;
            bm::Vault made;
            made.create(vaultFile, "test password");
            const auto mine = made.addIdentity("Mine");
            const auto chan = made.addChannel("sent recipients chan", "Sent chan");
            {
                const auto key = made.addMailboxKey();
                bm::Mailbox box;
                box.create(mailboxFile, key, made.mailboxKey(key));
                box.store("s1", mine, dora, "Hello Dora", "hi", 1, "Sent");
                box.store("s2", mine, dora, "Dora again", "hi", 2, "Sent");
                box.store("s3", mine, erin, "Hello Erin", "hi", 3, "Sent");
                box.store("s4", mine, mine, "A note to self", "hi", 4, "Sent");
                box.store("s5", mine, chan, "To my chan", "hi", 5, "Sent");
                box.store("s6", mine, "BM-not-an-address", "Lost", "hi", 6, "Sent");
                box.store("a1", mine, gina, "Archived", "hi", 7, "Archive");
                box.store("i1", frank, mine, "From Frank", "hi", 8, "Inbox");
                box.store("i2", ivan, mine, "From Ivan", "hi", 10, "Inbox");
                box.store("i3", ivan, mine, "Ivan again", "hi", 11, "Inbox");
                box.store("i4", mine, mine, "A note to self", "hi", 12, "Inbox");
                box.store("i5", chan, mine, "From my chan", "hi", 13, "Inbox");
                box.store("i6", "BM-not-an-address", mine, "Lost", "hi", 14, "Inbox");
                box.store("i7", erin, mine, "From Erin", "hi", 15, "Inbox");
                box.store("c1", jack, chan, "On a channel", "hi", 16, "Channels");
                box.store("b1", kim, kim, "A broadcast", "hi", 17, "Broadcasts");
                box.store("t1", lena, mine, "Deleted", "hi", 18, "Trash");
                box.store("a2", mona, mine, "Kept", "hi", 19, "Archive");
                box.saveContact(erin, "Erin E.");
            }
            {
                // An earlier release put the Sent recipients of this one in its book; its Inbox
                // senders it never did.
                const auto key = made.addMailboxKey();
                bm::Mailbox box;
                box.create(laterFile, key, made.mailboxKey(key));
                box.setSetting("migrated:sent-recipients", "1");
                box.store("s1", mine, nora, "Hello Nora", "hi", 1, "Sent");
                box.store("i1", owen, mine, "From Owen", "hi", 2, "Inbox");
            }
            made.lock();
            QDir().mkpath(temp.filePath("sent-node"));
            {
                QSettings config(temp.filePath("sent-node/desktop.ini"), QSettings::IniFormat);
                config.setValue("vault", vaultFile);
                config.setValue("mailbox", mailboxFile);
            }
            bm::Session legacy(temp.filePath("sent-node"), true);
            legacy.beginVaultUnlock();
            legacy.submitVaultPassword("test password");
            require(legacy.unlocked() && legacy.mailboxOpen(), "the mailbox from before opens");
            const auto book = [&] {
                QMap<QString, QString> names;
                for (const auto &c : legacy.contacts())
                    names[c.toMap()["address"].toString()] = c.toMap()["label"].toString();
                return names;
            };
            require(book() == QMap<QString, QString>({{dora, bm::addressTitle(dora)},
                                                      {erin, "Erin E."},
                                                      {frank, bm::addressTitle(frank)},
                                                      {ivan, bm::addressTitle(ivan)}}),
                    "opening adds each address a letter was sent to and each one that wrote to "
                    "an identity, once, named from the address; no own address, chan or bad "
                    "address, no sender outside the Inbox (channel, broadcast, trash, archive), "
                    "and a name already given stays");
            legacy.removeContact(dora);
            legacy.removeContact(frank);
            legacy.openMailboxAt(mailboxFile);
            require(legacy.mailboxOpen() && !legacy.isContact(dora) && !legacy.isContact(frank) &&
                        legacy.isContact(erin),
                    "a contact deleted later is not added again: each migration runs once");
            legacy.openMailboxAt(laterFile);
            require(legacy.mailboxOpen() &&
                        book() == QMap<QString, QString>({{owen, bm::addressTitle(owen)}}),
                    "a mailbox that had its Sent recipients added earlier gets its Inbox senders "
                    "now, and only those: each migration has a flag of its own");
            // A mailbox made now has nothing to catch up on: whom it writes to later stays
            // out of the book until the user adds them.
            const auto madeNow = temp.filePath("sent-made-now.bmmail");
            auto answer = new QTimer(&window);
            QObject::connect(answer, &QTimer::timeout, &window, [&, answer] {
                if (auto d = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                    answer->stop();
                    chooseFile(d, madeNow);
                }
            });
            answer->start(10);
            legacy.createMailbox();
            answer->deleteLater();
            require(legacy.mailboxOpen() && legacy.mailPath() == madeNow,
                    "a new mailbox is made, and open");
            legacy.closeMailbox();
            {
                bm::Vault peek;
                peek.unlock(vaultFile, "test password");
                bm::Mailbox box;
                for (const auto &id : peek.mailboxIds()) {
                    try {
                        box.open(madeNow, peek.mailboxKey(id));
                        break;
                    } catch (...) {
                    }
                }
                require(box.isOpen(), "the new mailbox opens with one of the vault's keys");
                box.store("n1", mine, hana, "Hello Hana", "hi", 9, "Sent");
                box.store("n2", pia, mine, "Hello from Pia", "hi", 10, "Inbox");
            }
            legacy.openMailboxAt(madeNow);
            require(legacy.mailboxOpen() && legacy.contacts().isEmpty(),
                    "a mailbox made now is not migrated when it is next opened: neither whom it "
                    "wrote to nor who wrote to it later");
        }
        {
            // The echo bot is every mailbox's first contact, once: /echobot by nickname and by
            // name. The default nickbot is the second, /nickbot by nickname and by name.
            const QString echo = "BM-87echoAkzstiHuPGSN8PH88m4WPm4attre8";
            require(builtInContacts.size() == 2 && builtInContacts[0].address == echo &&
                        builtInContacts[0].nickname == "/echobot" &&
                        builtInContacts[0].label == "/echobot",
                    "the first built-in contact is the echo bot, /echobot by nickname and by name");
            require(!builtInNickbots.isEmpty() &&
                        builtInContacts[1].address == builtInNickbots.first().address &&
                        builtInContacts[1].nickname == "/nickbot" &&
                        builtInContacts[1].label == "/nickbot",
                    "the second is the default nickbot, /nickbot by nickname and by name");
            const QList<bm::updates::DefaultContact> echoOnly{builtInContacts[0]};
            const auto vaultFile = temp.filePath("echo-vault.bmvault");
            const auto first = temp.filePath("echo-first.bmmail");
            const auto second = temp.filePath("echo-second.bmmail");
            bm::Vault made;
            made.create(vaultFile, "test password");
            const auto mine = made.addIdentity("Mine");
            for (const auto &path : {first, second}) {
                const auto key = made.addMailboxKey();
                bm::Mailbox box;
                box.create(path, key, made.mailboxKey(key));
                // A letter went to the bot: the migration of Sent must not name it first.
                box.store("s1", mine, echo, "Hello bot", "hi", 1, "Sent");
                if (path == second)
                    box.saveContact(echo, "Echo"); // already a contact, under a name of its own
            }
            made.lock();
            QDir().mkpath(temp.filePath("echo-node"));
            {
                QSettings config(temp.filePath("echo-node/desktop.ini"), QSettings::IniFormat);
                config.setValue("vault", vaultFile);
                config.setValue("mailbox", first);
            }
            bm::updates::setDefaultContactsForTesting(echoOnly);
            bm::Session starter(temp.filePath("echo-node"), true);
            starter.beginVaultUnlock();
            starter.submitVaultPassword("test password");
            const auto bot = [&] { // the echo bot's name and nickname, or nothing
                for (const auto &c : starter.contacts())
                    if (c.toMap()["address"].toString() == echo)
                        return QStringList{c.toMap()["label"].toString(),
                                           c.toMap()["nickname"].toString()};
                return QStringList();
            };
            require(starter.mailboxOpen() && starter.contacts().size() == 1 &&
                        bot() == QStringList({"/echobot", "/echobot"}),
                    "an old mailbox opens with the echo bot as a contact, /echobot by nickname "
                    "and by name, though a letter went to it");
            starter.removeContact(echo);
            starter.openMailboxAt(first);
            require(starter.mailboxOpen() && !starter.isContact(echo),
                    "a deleted echo bot is not added again: the migration runs once");
            starter.openMailboxAt(second);
            require(bot() == QStringList({"Echo", "/echobot"}),
                    "a contact that was there keeps its name, and gets the nickname");
            // A mailbox made now starts with it too.
            const auto createAt = [&](const QString &path) {
                auto answer = new QTimer(&window);
                QObject::connect(answer, &QTimer::timeout, &window, [&, answer, path] {
                    if (auto d = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                        answer->stop();
                        chooseFile(d, path);
                    }
                });
                answer->start(10);
                starter.createMailbox();
                answer->deleteLater();
            };
            const auto madeNow = temp.filePath("echo-made-now.bmmail");
            createAt(madeNow);
            require(starter.mailboxOpen() && starter.mailPath() == madeNow &&
                        bot() == QStringList({"/echobot", "/echobot"}),
                    "a mailbox made now has the echo bot from the start");
            bm::updates::setDefaultContactsForTesting({});
            // And the built-in subscriptions, the Bitmessage digest: from the start, not at its
            // next opening; and a mailbox that was opened since does not get it back once it is
            // gone.
            const auto digest = bm::updates::digestAddress();
            bm::updates::setDefaultSubscriptionsForTesting({{digest, "Bitmessage digest"}});
            const auto subscribed = [&] {
                QStringList addresses;
                for (const auto &s : starter.subscriptions())
                    addresses << s.toMap()["address"].toString();
                return addresses;
            };
            const auto madeSubscribed = temp.filePath("digest-made-now.bmmail");
            createAt(madeSubscribed);
            require(starter.mailboxOpen() && starter.mailPath() == madeSubscribed &&
                        subscribed() == QStringList({digest}),
                    "a mailbox made now is subscribed to the Bitmessage digest from the start");
            starter.unsubscribe(digest);
            starter.openMailboxAt(madeSubscribed);
            require(starter.mailboxOpen() && subscribed().isEmpty(),
                    "unsubscribing sticks: opening the mailbox again does not add the digest back");
            bm::updates::setDefaultSubscriptionsForTesting({});
        }
        {
            // The new vault dialog's look, under the window's style sheet: every
            // field reaches the right edge, also where forms keep fields at their
            // size hint (macOS); nothing is squeezed or cut as rows appear, in
            // English or Russian; rows stay put while the phrase is typed.
            struct FieldsAtHint : QProxyStyle {
                int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                              QStyleHintReturn *data) const override {
                    if (hint == SH_FormLayoutFieldGrowthPolicy)
                        return QFormLayout::FieldsStayAtSizeHint;
                    return QProxyStyle::styleHint(hint, option, widget, data);
                }
            } fieldsAtHint;
            bm::Session looks(temp.filePath("looks-node"), true);
            bm::DesktopWindow lookWindow(looks, temp.filePath("sounds-looks"));
            lookWindow.show();
            QStringList faults;
            const auto inspect = [&](QDialog *d, const QString &when) {
                QCoreApplication::processEvents();
                for (auto w : d->findChildren<QWidget *>()) {
                    // A field's own buttons are placed by the field, not the dialog.
                    if (!w->isVisible() || qobject_cast<QLineEdit *>(w->parentWidget()))
                        continue;
                    const auto name = when + ": " +
                                      (w->objectName().isEmpty() ? w->metaObject()->className()
                                                                 : w->objectName());
                    auto label = qobject_cast<QLabel *>(w);
                    if (label && label->wordWrap() &&
                        label->height() < label->heightForWidth(label->width()))
                        faults << name + " cut";
                    if ((qobject_cast<QAbstractButton *>(w) &&
                         (w->width() < w->sizeHint().width() ||
                          w->height() < w->sizeHint().height())) ||
                        (qobject_cast<QLineEdit *>(w) && w->height() < w->sizeHint().height()))
                        faults << name + " squeezed";
                }
                const int edge = d->width() - d->layout()->contentsMargins().right();
                for (auto name : {"nicknameField", "phraseField", "vaultFileButton",
                                  "newVaultPasswordField", "newVaultRepeatField"})
                    if (auto w = d->findChild<QWidget *>(name);
                        w->isVisible() && w->mapTo(d, QPoint(w->width(), 0)).x() < edge - 1)
                        faults << when + ": " + name + " short of the right edge";
                if (!boldAndWhole(d->findChild<QPushButton *>("newVaultCreateButton")))
                    faults << when + ": Create's text cut";
            };
            const auto look = [&](const QString &language) {
                QTranslator translator;
                if (language != "en")
                    require(translator.load(":/i18n/ynotbit_" + language) &&
                                QCoreApplication::installTranslator(&translator),
                            "load a translation");
                QTimer::singleShot(30, &lookWindow, [&] {
                    auto d = lookWindow.findChild<QDialog *>("newVaultDialog");
                    if (!d)
                        return;
                    d->setStyle(&fieldsAtHint);
                    inspect(d, language + " random");
                    auto file = d->findChild<QLineEdit *>("vaultFileField");
                    auto nickname = d->findChild<QLineEdit *>("nicknameField");
                    nickname->clear();
                    QTest::keyClicks(nickname, "Wonderful Unpredictable Wombat");
                    // The style sheet's padding and frame, and the field's margins: 24 px. The
                    // dialog asks for 600 px of path at most: in a font large enough to need
                    // more (the offscreen platform on Windows) the path does not show whole.
                    const int pathWidth =
                        QFontMetrics(file->font()).horizontalAdvance(file->text());
                    if (std::min(pathWidth + 24, 600) > file->width())
                        faults << language +
                                      QString(": a long nickname's whole path does not show: %1 "
                                              "needs %2 px, the field is %3 (at least %4) in a "
                                              "dialog %5 wide on a screen %6 wide")
                                          .arg(file->text())
                                          .arg(pathWidth + 24)
                                          .arg(file->width())
                                          .arg(file->minimumWidth())
                                          .arg(d->width())
                                          .arg(d->screen()->availableGeometry().width());
                    auto form = qobject_cast<QFormLayout *>(d->layout());
                    auto passwordLabel = qobject_cast<QLabel *>(
                        form->labelForField(d->findChild<QLineEdit *>("newVaultPasswordField")));
                    if (language == "en" &&
                        (!passwordLabel || passwordLabel->text() != "Vault password"))
                        faults << "the password is not called the vault password";
                    d->findChild<QRadioButton *>("phraseKeysButton")->click();
                    if (d->findChild<QAbstractButton *>("anotherKeysButton")->isVisible())
                        faults << language + ": another random address is offered for a phrase";
                    auto phrase = d->findChild<QLineEdit *>("phraseField");
                    if (auto eye = d->findChild<QAction *>("showPhraseAction")) {
                        eye->trigger();
                        if (phrase->echoMode() != QLineEdit::Normal)
                            faults << language + ": the eye does not show the phrase";
                        eye->trigger();
                    } else {
                        faults << language + ": no eye in the phrase field";
                    }
                    QTest::keyClicks(phrase, "ynotbit test phrase");
                    QTest::qWait(400);
                    if (d->findChild<QLabel *>("rebuildNote")->isVisible())
                        faults << language + ": a key pair is noted for the phrase's first address";
                    inspect(d, language + " phrase");
                    const auto row = file->mapTo(d, QPoint());
                    const auto size = d->size();
                    QTest::keyClicks(phrase, "!");
                    QCoreApplication::processEvents();
                    if (file->mapTo(d, QPoint()) != row || d->size() != size)
                        faults << language + ": rows move while the phrase is typed";
                    QTest::keyClick(phrase, Qt::Key_Backspace);
                    QTest::qWait(400);
                    auto shorter = d->findChild<QPushButton *>("shorterButton");
                    const int width = shorter->width();
                    shorter->click();
                    QTest::qWait(1300);
                    if (shorter->width() != width)
                        faults << language + ": the search button changes width";
                    inspect(d, language + " search");
                    shorter->click();
                    QTest::keyClicks(d->findChild<QLineEdit *>("newVaultPasswordField"),
                                     "long test password");
                    auto repeat = d->findChild<QLineEdit *>("newVaultRepeatField");
                    auto problem = d->findChild<QLabel *>("passwordProblem");
                    QTest::keyClicks(repeat, "long test passwor");
                    if (problem->isVisible())
                        faults << language + ": a mismatch is said before the repeat is typed out";
                    QTest::keyClicks(repeat, "x");
                    if (!problem->isVisible())
                        faults << language + ": a typed-out mismatch is not said";
                    inspect(d, language + " mismatch");
                    d->reject();
                });
                lookWindow.findChild<QPushButton *>("createVaultButton")->click();
                QCoreApplication::removeTranslator(&translator);
            };
            look("en");
            look("ru");
            for (const auto &fault : faults)
                std::cerr << qPrintable(fault) << '\n';
            require(faults.isEmpty(),
                    "the new vault dialog fills its width and squeezes nothing, in English and "
                    "Russian");
        }
        {
            // The new vault dialog: an address first, then a nickname and a file
            // named after it; the vault around that address.
            const QString documents = temp.filePath("documents");
            QDir().mkpath(documents);
            bm::Session fresh(temp.filePath("dialog-node"), true);
            auto dialog = std::make_unique<bm::NewVaultDialog>(fresh, documents, false);
            dialog->show();
            auto nickname = dialog->findChild<QLineEdit *>("nicknameField");
            auto address = dialog->findChild<QLabel *>("addressLabel");
            auto file = dialog->findChild<QLineEdit *>("vaultFileField");
            auto password = dialog->findChild<QLineEdit *>("newVaultPasswordField");
            auto repeat = dialog->findChild<QLineEdit *>("newVaultRepeatField");
            auto phrase = dialog->findChild<QLineEdit *>("phraseField");
            auto shorter = dialog->findChild<QPushButton *>("shorterButton");
            auto create = dialog->findChild<QPushButton *>("newVaultCreateButton");
            const auto in = [&](const QString &name) { return QDir(documents).filePath(name); };
            const auto first = address->text();
            require(first.startsWith("BM-") &&
                        nickname->text() == bm::NewVaultDialog::nicknameFor(first) &&
                        file->text() ==
                            in(bm::NewVaultDialog::slugFor(nickname->text()) + ".bmvault"),
                    "the dialog opens with random keys, their nickname, and a file named after it");
            require(
                bm::NewVaultDialog::nicknameFor("BM-5oSrHVwbsQYPKmePHfqQJZr6eibiqj9") ==
                        "Imperial Conscious Grebe" &&
                    bm::NewVaultDialog::nicknameFor("BM-%1%2") == "Noble Llama of Discourse" &&
                    bm::NewVaultDialog::slugFor("Noble Llama of Discourse") ==
                        "noble-llama-of-discourse" &&
                    bm::NewVaultDialog::slugFor("  Alice's  Work!") == "alice-s-work" &&
                    bm::NewVaultDialog::slugFor("Ёжик в тумане") == "ёжик-в-тумане" &&
                    bm::NewVaultDialog::slugFor("!!!") == "vault",
                "nicknames are title case; file names are their slug, letters of any script kept");
            dialog->findChild<QAbstractButton *>("anotherKeysButton")->click();
            require(address->text() != first &&
                        nickname->text() == bm::NewVaultDialog::nicknameFor(address->text()),
                    "Another makes new keys, and renames");
            QFile taken(in("taken.bmvault"));
            require(taken.open(QIODevice::WriteOnly), "make a file to step around");
            taken.close();
            nickname->clear();
            QTest::keyClicks(nickname, "Taken");
            require(file->text() == in("taken-2.bmvault"),
                    "the nickname names the file, numbered past one that exists");
            file->clear();
            QTest::keyClicks(file, in("mine.bmvault"));
            QTest::keyClicks(nickname, " More");
            require(file->text() == in("mine.bmvault"), "a file the user chose stays put");
            QTimer::singleShot(50, dialog.get(), [&] {
                for (auto w : QApplication::topLevelWidgets())
                    if (auto d = qobject_cast<QFileDialog *>(w))
                        chooseFile(d, in("picked"));
            });
            dialog->findChild<QAbstractButton *>("vaultFileButton")->click();
            require(file->text() == in("picked.bmvault"), "… picks the file, with its suffix");
            require(!create->isEnabled(), "no password, no vault");
            QTest::keyClicks(password, "long test password");
            QTest::keyClicks(repeat, "long test passwor");
            require(!create->isEnabled() &&
                        !dialog->findChild<QLabel *>("passwordProblem")->isVisibleTo(dialog.get()),
                    "a repeat not yet typed out blocks Create, unremarked");
            QTest::keyClicks(repeat, "x");
            require(!create->isEnabled() &&
                        dialog->findChild<QLabel *>("passwordProblem")->isVisibleTo(dialog.get()),
                    "a typed-out mismatch is said, and blocks Create");
            QTest::keyClick(repeat, Qt::Key_Backspace);
            QTest::keyClicks(repeat, "d");
            require(create->isEnabled(), "matching passwords and a new file allow Create");
            file->clear();
            QTest::keyClicks(file, taken.fileName());
            require(!create->isEnabled() &&
                        dialog->findChild<QLabel *>("vaultFileProblem")->isVisibleTo(dialog.get()),
                    "an existing file is refused");
            dialog->findChild<QRadioButton *>("phraseKeysButton")->click();
            QTest::keyClicks(phrase, "ynotbit test phrase");
            // The dialog works out an address 300 ms after the phrase stops changing, on the
            // window's own thread: wait for what it shows, not for a time a slow machine
            // overruns.
            require(QTest::qWaitFor(
                        [&] { return address->text() == "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN"; },
                        10000) &&
                        !dialog->findChild<QLabel *>("rebuildNote")->isVisibleTo(dialog.get()),
                    "a phrase gives its first address, which the phrase alone rebuilds");
            shorter->click();
            require(shorter->text() == "Stop search", "the search runs");
            QTest::keyClicks(phrase, "!");
            const auto edited = bm::phraseIdentity("ynotbit test phrase!", {},
                                                   *bm::firstPair("ynotbit test phrase!", 1))
                                    .address;
            require(QTest::qWaitFor(
                        [&] {
                            return shorter->text() == "Find a shorter address" &&
                                   address->text() == edited;
                        },
                        10000),
                    "editing the phrase stops the search; the address is the new phrase's");
            QTest::keyClick(phrase, Qt::Key_Backspace);
            require(QTest::qWaitFor(
                        [&] { return address->text() == "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN"; },
                        10000),
                    "the phrase gives its first address again");
            shorter->click();
            require(QTest::qWaitFor(
                        [&] { return address->text() == "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6"; },
                        60000),
                    "the search finds the phrase's shorter address");
            shorter->click();
            require(address->text() == "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6" &&
                        dialog->findChild<QLabel *>("rebuildNote")->isVisibleTo(dialog.get()) &&
                        dialog->findChild<QLabel *>("rebuildNote")->text().contains("27122") &&
                        shorter->text() == "Find a shorter address",
                    "the search finds the shorter address, noting its key pair; Stop search "
                    "keeps it");
            nickname->clear();
            QTest::keyClicks(nickname, QString(300, 'n'));
            file->clear();
            QTest::keyClicks(file, in(QString(300, 'n') + ".bmvault"));
            create->click();
            require(dialog->isVisible() &&
                        dialog->findChild<QLabel *>("newVaultError")->isVisibleTo(dialog.get()) &&
                        !fresh.unlocked() &&
                        address->text() == "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6",
                    "a vault that cannot be written leaves the dialog open, keys kept, saying why");
            require(dialog->findChild<QLabel *>("newVaultError")->text().startsWith("Cannot write"),
                    "...and says it cannot write there, not that the vault is in use");
            nickname->clear();
            QTest::keyClicks(nickname, "Shorter Me");
            file->clear();
            QTest::keyClicks(file, in("created.bmvault"));
            create->click();
            require(dialog->result() == QDialog::Accepted && fresh.unlocked() &&
                        QFile::exists(in("created.bmvault")),
                    "Create makes the vault, and closes");
            const auto identities = fresh.identities();
            require(identities.size() == 1 &&
                        identities[0].toMap()["address"].toString() ==
                            "BM-NC3eQa6kLxUsWNGFCqAbAJd733dhXkH6" &&
                        identities[0].toMap()["label"].toString() == "Shorter Me",
                    "the vault's identity is the address shown, with its nickname");
            require(phrase->text().isEmpty() && password->text().isEmpty() &&
                        repeat->text().isEmpty(),
                    "closing clears the phrase and the passwords");
            // Cancelled mid-search: the threads stop at once.
            fresh.lock();
            dialog = std::make_unique<bm::NewVaultDialog>(fresh, documents, false);
            dialog->show();
            dialog->findChild<QPushButton *>("shorterButton")->click();
            QTest::qWait(100);
            QElapsedTimer closing;
            closing.start();
            dialog->reject();
            dialog.reset();
            require(closing.elapsed() < 2000, "cancelling while the search runs stops it at once");
        }
        {
            // The same dialog makes an identity alone, in the open vault: no file,
            // no password.
            bm::Session holder(temp.filePath("identity-dialog-node"), true);
            require(holder.createVault(temp.filePath("identities.bmvault"), "pw", "pw",
                                       bm::phraseIdentity("ynotbit test phrase", "First", 60)),
                    "make a vault to add to");
            bm::NewVaultDialog dialog(holder, {}, false, nullptr, bm::NewVaultDialog::IdentityOnly);
            dialog.show();
            auto create = dialog.findChild<QPushButton *>("newVaultCreateButton");
            require(
                dialog.objectName() == "newIdentityDialog" &&
                    dialog.windowTitle() == "New identity" &&
                    !dialog.findChild<QLineEdit *>("vaultFileField")->isVisibleTo(&dialog) &&
                    !dialog.findChild<QLineEdit *>("newVaultPasswordField")->isVisibleTo(&dialog) &&
                    !dialog.findChild<QLineEdit *>("newVaultRepeatField")->isVisibleTo(&dialog) &&
                    create->isEnabled() && create->text() == "Create identity",
                "for an identity alone the dialog asks no file and no password");
            const int width = dialog.width();
            dialog.findChild<QRadioButton *>("phraseKeysButton")->click();
            QCoreApplication::processEvents();
            require(dialog.width() == width,
                    "the dialog keeps its width as the keys' source changes");
            QTest::keyClicks(dialog.findChild<QLineEdit *>("phraseField"), "ynotbit test phrase");
            require(QTest::qWaitFor(
                        [&] {
                            return !create->isEnabled() &&
                                   dialog.findChild<QLabel *>("addressTaken")->isVisibleTo(&dialog);
                        },
                        10000),
                    "an address already in the vault is said, and not made again");
            dialog.findChild<QRadioButton *>("randomKeysButton")->click();
            auto nickname = dialog.findChild<QLineEdit *>("nicknameField");
            nickname->clear();
            QTest::keyClicks(nickname, "Work");
            const auto address = dialog.findChild<QLabel *>("addressLabel")->text();
            create->click();
            bool added = false;
            for (const auto &identity : holder.identities())
                added = added || (identity.toMap()["address"].toString() == address &&
                                  identity.toMap()["label"].toString() == "Work");
            require(dialog.result() == QDialog::Accepted && holder.identities().size() == 2 &&
                        added,
                    "Create identity adds the address shown, with its nickname, to the open vault");
        }
        {
            // Enter right after editing the phrase never saves the old phrase's keys.
            bm::Session typed(temp.filePath("typed-node"), true);
            bm::NewVaultDialog dialog(typed, temp.path(), false);
            dialog.show();
            dialog.findChild<QRadioButton *>("phraseKeysButton")->click();
            auto phrase = dialog.findChild<QLineEdit *>("phraseField");
            auto file = dialog.findChild<QLineEdit *>("vaultFileField");
            file->clear();
            QTest::keyClicks(file, temp.filePath("typed.bmvault"));
            QTest::keyClicks(phrase, "ynotbit test phrase");
            auto shown = dialog.findChild<QLabel *>("addressLabel");
            require(QTest::qWaitFor(
                        [&] { return shown->text() == "BM-2cVdr5sKJWPUGP9MtqdVAn4NHM3TScMMzN"; },
                        10000),
                    "the phrase's keys are there before they are edited");
            QTest::keyClicks(dialog.findChild<QLineEdit *>("newVaultPasswordField"),
                             "long test password");
            QTest::keyClicks(dialog.findChild<QLineEdit *>("newVaultRepeatField"),
                             "long test password");
            QTest::keyClicks(phrase, "!");
            QTest::keyClick(phrase, Qt::Key_Return);
            require(!typed.unlocked(), "Enter right after editing the phrase saves nothing");
            const auto edited = bm::phraseIdentity("ynotbit test phrase!", {},
                                                   *bm::firstPair("ynotbit test phrase!", 1))
                                    .address;
            require(QTest::qWaitFor([&] { return shown->text() == edited; }, 10000),
                    "the address is the edited phrase's");
        }
        {
            // New vault… with a vault open asks nothing; Cancel keeps it open.
            bool asked = false, opened = false;
            QTimer::singleShot(30, &window, [&] {
                if (auto box = window.findChild<QMessageBox *>(); box && box->isVisible()) {
                    asked = true;
                    box->button(QMessageBox::No)->click();
                }
                if (auto d = window.findChild<QDialog *>("newVaultDialog"); d && d->isVisible()) {
                    opened = true;
                    d->reject();
                }
            });
            window.findChild<QAction *>("createVaultAction")->trigger();
            require(!asked && opened && session.unlocked(),
                    "New vault… with a vault open asks nothing; Cancel keeps the vault open");
        }
        {
            // A vault made from a window's lock screen is the one it offers next,
            // even when the lock screen offered another before.
            bm::Session own(temp.filePath("window-vault-node"), true);
            bm::DesktopWindow second(own, temp.filePath("sounds-second"));
            second.show();
            bool asked = false;
            // Makes a vault in the dialog the named button or action opens,
            // answering Yes to any question asked before it.
            const auto makeVault = [&](const QString &path, const QString &opener) {
                std::function<void()> fill = [&, path] {
                    if (auto box = second.findChild<QMessageBox *>(); box && box->isVisible()) {
                        asked = true;
                        box->button(QMessageBox::Yes)->click();
                    }
                    auto d = second.findChild<QDialog *>("newVaultDialog");
                    if (!d || !d->isVisible()) {
                        QTimer::singleShot(30, &second, fill);
                        return;
                    }
                    auto file = d->findChild<QLineEdit *>("vaultFileField");
                    file->clear();
                    QTest::keyClicks(file, path);
                    QTest::keyClicks(d->findChild<QLineEdit *>("newVaultPasswordField"),
                                     "long test password");
                    QTest::keyClicks(d->findChild<QLineEdit *>("newVaultRepeatField"),
                                     "long test password");
                    d->findChild<QPushButton *>("newVaultCreateButton")->click();
                };
                QTimer::singleShot(30, &second, fill);
                if (auto action = second.findChild<QAction *>(opener))
                    action->trigger();
                else
                    second.findChild<QPushButton *>(opener)->click();
                const bool madeIt =
                    own.unlocked() && own.vaultPath() == path && QFile::exists(path);
                own.lock();
                QCoreApplication::processEvents();
                return madeIt;
            };
            const auto lockedName = [&] {
                return second.findChild<QLabel *>("lockedVaultName")->text();
            };
            const auto made = temp.filePath("made-in-window.bmvault");
            require(makeVault(made, "createVaultButton") &&
                        lockedName() == "made-in-window.bmvault",
                    "the lock screen's Create a new vault… makes the vault, offered when locked");
            const auto another = temp.filePath("another-in-window.bmvault");
            require(makeVault(another, "createVaultButton") &&
                        lockedName() == "another-in-window.bmvault",
                    "a second vault made there is the one offered next, not the first");
            own.choosePendingVault(another);
            own.submitVaultPassword("long test password");
            require(own.unlocked(), "test sanity: the vault opens again");
            const auto third = temp.filePath("third-in-window.bmvault");
            require(makeVault(third, "createVaultAction") && !asked &&
                        lockedName() == "third-in-window.bmvault",
                    "New vault… with a vault open asks nothing; the new vault takes its place");
        }
        testFileMenu(temp);
        testMarkAllRead(temp);
        testLettersContextMenu(temp);
        testUnreadTotal(temp);
        testRailBadgeWidget();
        testRailBadges(temp);
        testPyBitmessageImport(temp);
        testPyBitmessageOffer(temp);
        testPyBitmessageImportAddsPeople(temp);
        testNickbots(temp);
        testLaneBelongsToTheVault(temp);
        testNickbotsPage(temp);
        testNicknameDialog(temp);
        testIdentityIcons();
        testCheckNicknames(temp);
        testContactChats(temp);
        testFindPeople(temp);
        testNewIdentityRegisters(temp);
        require(session.portMapper().state() == bm::PortMapper::State::Off,
                "an offline session never talks to the router");
        {
            // The node's object count and size come from its status.json, and
            // the last values stay shown while it isn't running.
            QFile status(temp.filePath("node/status.json"));
            require(status.open(QIODevice::WriteOnly), "write a node status");
            status.write(R"({"objects":42,"object_bytes":1048576,"established_incoming":2,"time":0})");
            status.close();
            QTest::qWait(900);
            require(session.objectCount() == 42 && session.cacheBytes() == 1048576,
                    "the object count and size come from the node's status.json");
            require(session.incomingConnections() == 2,
                    "incoming connections come from the node's status.json");
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
#include "widgets_tests.moc"
