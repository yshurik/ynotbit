// The notification-area icon: its glyph, the colour each platform gets, its menu, and the second
// launch that wakes the running window. (The window's side of it, closing into the tray, is in
// widgets_tests.)
#include "single_instance.h"
#include "tray_icon.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLocalSocket>
#include <QMenu>
#include <QSignalSpy>
#include <QSystemTrayIcon>
#include <QTest>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
// How many separate stretches of ink run down the glyph's middle column: the top edge of the
// envelope, the shield (in two, when it has a keyhole), the bottom edge.
static int inkRuns(const QImage &image) {
    int runs = 0;
    bool inside = false;
    for (int y = 0; y < image.height(); ++y) {
        const bool ink = image.pixelColor(image.width() / 2, y).alpha() >= 128;
        runs += ink && !inside;
        inside = ink;
    }
    return runs;
}
static void testGlyph() {
    const QColor color("#3a7bd5");
    for (int size : bm::kTraySizes) {
        const auto image = bm::trayGlyphImage(size, color);
        require(image.width() == size && image.height() == size,
                "the glyph is as many pixels square as asked");
        const auto premultiplied = image.convertedTo(QImage::Format_ARGB32_Premultiplied);
        int solid = 0, left = size, right = -1, top = size, bottom = -1;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const auto pixel = image.pixelColor(x, y);
                solid += pixel.alpha() >= 128;
                if (pixel.alpha() >= 64) {
                    left = qMin(left, x);
                    right = qMax(right, x);
                    top = qMin(top, y);
                    bottom = qMax(bottom, y);
                }
                // One colour: every pixel, at the edges too, is the colour asked, only more or less
                // of it (as stored: the colour times its alpha).
                const QRgb raw = reinterpret_cast<const QRgb *>(premultiplied.constScanLine(y))[x];
                const int alpha = qAlpha(raw);
                require(qAbs(qRed(raw) - color.red() * alpha / 255) <= 2 &&
                            qAbs(qGreen(raw) - color.green() * alpha / 255) <= 2 &&
                            qAbs(qBlue(raw) - color.blue() * alpha / 255) <= 2,
                        "the glyph has no colour but the one asked");
                // Left and right are the same, so the mark is centred, not just about so.
                require(qAbs(pixel.alpha() - image.pixelColor(size - 1 - x, y).alpha()) <= 16,
                        "the glyph is symmetric");
            }
        const double ink = double(solid) / (size * size);
        require(ink > 0.12 && ink < 0.60, "the glyph is neither empty nor a block");
        require(left >= 1 && top >= 1 && right <= size - 2 && bottom <= size - 2,
                "the glyph keeps a pixel clear all round, so no bar clips it");
        require(right - left + 1 >= size * 0.85 && bottom - top + 1 >= size * 0.55,
                "the glyph fills its square");
        // The keyhole is for sizes where it can be told from a blot.
        require(inkRuns(image) == (size >= 28 ? 4 : 3),
                "the shield has a keyhole from 28 pixels up, and none below");
    }
    // Any colour: the glyph does not care which.
    for (const auto &other : {QColor(Qt::black), QColor(Qt::white), QColor("#c1cdd7")}) {
        const auto image = bm::trayGlyphImage(32, other);
        bool seen = false;
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                if (image.pixelColor(x, y).alpha() == 255) {
                    seen = true;
                    require(image.pixelColor(x, y).rgb() == other.rgb(), "the colour asked is the ink");
                }
        require(seen, "the glyph has solid ink");
    }
    // The icon offers all of them.
    const auto icon = bm::trayGlyph(color);
    require(!icon.isNull(), "the tray icon exists");
    for (int size : bm::kTraySizes)
        require(icon.availableSizes().contains(QSize(size, size)),
                "the tray icon offers every size");
    require(icon.pixmap(QSize(44, 44)).toImage() == bm::trayGlyphImage(44, color).convertedTo(
                                                         icon.pixmap(QSize(44, 44)).toImage().format()),
            "the icon's picture of a size is the glyph drawn at that size");
}
static void testTone() {
    using bm::TrayPlatform;
    // macOS hands the colour to the menu bar, whatever else is said.
    for (bool light : {false, true}) {
        const auto tone = bm::trayTone(TrayPlatform::Mac, {light, "GNOME", true});
        require(tone.mask && tone.color == QColor(Qt::black),
                "macOS gets a black template image, which the menu bar tints");
    }
    // Windows: the taskbar's own theme, not the apps'.
    const auto onLight = bm::trayTone(TrayPlatform::Windows, {true, {}, true});
    const auto onDark = bm::trayTone(TrayPlatform::Windows, {false, {}, false});
    require(!onLight.mask && onLight.color.lightness() < 64, "dark on a light Windows taskbar");
    require(!onDark.mask && onDark.color.lightness() > 220, "light on a dark Windows taskbar");
    // Linux: GNOME's bar is dark whatever the apps wear; elsewhere the system's colour scheme is the
    // best guess (not the app's own theme, which the window may have set to anything).
    for (const char *desktop : {"GNOME", "ubuntu:GNOME", "Unity:Unity7:ubuntu"}) {
        const auto tone = bm::trayTone(TrayPlatform::Linux, {false, desktop, false});
        require(!tone.mask && tone.color.lightness() > 220,
                "light on a GNOME panel, even under a light scheme");
    }
    for (const char *desktop : {"KDE", "X-Cinnamon", "XFCE", ""}) {
        const auto dark = bm::trayTone(TrayPlatform::Linux, {false, desktop, true});
        const auto light = bm::trayTone(TrayPlatform::Linux, {false, desktop, false});
        require(!dark.mask && dark.color.lightness() > 220 && !light.mask &&
                    light.color.lightness() < 64,
                "elsewhere the glyph is light under a dark scheme and dark under a light one");
    }
    // This machine.
#if defined(Q_OS_MACOS)
    require(bm::thisPlatform() == TrayPlatform::Mac, "this is macOS");
#elif defined(Q_OS_WIN)
    require(bm::thisPlatform() == TrayPlatform::Windows, "this is Windows");
#else
    require(bm::thisPlatform() == TrayPlatform::Linux, "this is Linux or another desktop Unix");
#endif
}
static bm::TrayIcon::Labels labels() {
    return {"Show ynotbit", "Hide ynotbit", "New letter", "New chat", "Settings…", "Quit"};
}
static void testMenu() {
    bm::TrayIcon tray(labels());
    QStringList names, texts;
    for (auto action : tray.menu()->actions()) {
        names << (action->isSeparator() ? "-" : action->objectName());
        if (!action->isSeparator())
            texts << action->text();
        require(action->isSeparator() || action->menuRole() == QAction::NoRole,
                "no item moves to the application menu, whatever its words");
    }
    require(names == QStringList{"trayShowHideAction", "-", "trayNewLetterAction",
                                 "trayNewChatAction", "traySettingsAction", "-", "trayQuitAction"},
            "the menu is Show/Hide, New letter, New chat, Settings, Quit");
    require(texts == QStringList{"Show ynotbit", "New letter", "New chat", "Settings…", "Quit"},
            "the items carry the words they were given");
    require(tray.icon()->contextMenu() == tray.menu() && tray.icon()->toolTip() == "ynotbit",
            "the icon opens the menu and is named");
    const auto tone = bm::trayTone(bm::thisPlatform(), bm::currentTrayEnvironment());
    require(!tray.icon()->icon().isNull() && tray.icon()->icon().isMask() == tone.mask,
            "the icon is the glyph, a template image where the platform tints it");
    // Show or Hide, by what the window is.
    auto showHide = tray.menu()->findChild<QAction *>("trayShowHideAction");
    tray.setWindowShown(true);
    require(showHide->text() == "Hide ynotbit", "a shown window is offered Hide");
    tray.setWindowShown(false);
    require(showHide->text() == "Show ynotbit", "a hidden window is offered Show");
    // Writing needs a mailbox; the rest does not.
    const auto enabled = [&](const char *name) {
        return tray.menu()->findChild<QAction *>(name)->isEnabled();
    };
    tray.setMailboxOpen(false);
    require(!enabled("trayNewLetterAction") && !enabled("trayNewChatAction") &&
                enabled("trayShowHideAction") && enabled("traySettingsAction") &&
                enabled("trayQuitAction"),
            "without a mailbox, only writing is off");
    tray.setMailboxOpen(true);
    require(enabled("trayNewLetterAction") && enabled("trayNewChatAction"),
            "with a mailbox, writing is on");
    // Each item says its own request, once.
    struct Item {
        const char *action;
        const char *signal;
    };
    for (const auto &item : {Item{"trayShowHideAction", SIGNAL(showHideRequested())},
                             Item{"trayNewLetterAction", SIGNAL(newLetterRequested())},
                             Item{"trayNewChatAction", SIGNAL(newChatRequested())},
                             Item{"traySettingsAction", SIGNAL(settingsRequested())},
                             Item{"trayQuitAction", SIGNAL(quitRequested())}}) {
        QList<QSignalSpy *> spies;
        for (const char *signal :
             {SIGNAL(showHideRequested()), SIGNAL(newLetterRequested()), SIGNAL(newChatRequested()),
              SIGNAL(settingsRequested()), SIGNAL(quitRequested()), SIGNAL(showRequested())})
            spies << new QSignalSpy(&tray, signal);
        tray.menu()->findChild<QAction *>(item.action)->trigger();
        for (auto spy : spies) {
            const bool mine = QByteArray(spy->signal().constData()) == QByteArray(item.signal + 1);
            require(spy->count() == (mine ? 1 : 0), "an item says its own request and no other");
        }
        qDeleteAll(spies);
    }
    // A click on the icon (Windows, Linux) brings the window forward, and never hides it: a double
    // click is two clicks, and would otherwise show it and hide it again.
    QSignalSpy shown(&tray, &bm::TrayIcon::showRequested), toggled(&tray, &bm::TrayIcon::showHideRequested);
    for (auto reason : {QSystemTrayIcon::Trigger, QSystemTrayIcon::DoubleClick})
        emit tray.icon()->activated(reason);
    require(shown.count() == 2 && toggled.count() == 0, "a click on the icon shows the window");
    for (auto reason : {QSystemTrayIcon::MiddleClick, QSystemTrayIcon::Context})
        emit tray.icon()->activated(reason);
    require(shown.count() == 2, "a middle click or the menu's own click does not");
}
static void testActive() {
    bm::TrayIcon tray(labels());
    bm::TrayIcon::setAvailableForTesting(true);
    require(tray.active(), "with a tray there, the icon is showing");
    bm::TrayIcon::setAvailableForTesting(false);
    require(!tray.active(), "with no tray there, it is not");
    bm::TrayIcon::setAvailableForTesting(std::nullopt);
    // With none forced, the platform's own answer is the one: the offscreen platform of a test
    // run has no tray, and says so.
    require(tray.active() == QSystemTrayIcon::isSystemTrayAvailable(),
            "the platform's own answer is used when none is forced");
}
static void testStartupRemove() {
    const auto pieces = bm::startupRemoveMessage("gnome-shell/x/1-2-host_TIME3");
    require(!pieces.isEmpty() && pieces.first().startsWith("remove: ID=\""),
            "the message says remove, for the id");
    QByteArray all;
    for (const auto &piece : pieces) {
        require(piece.size() == 20, "each piece fills a client message's 20 bytes");
        all += piece;
    }
    require(all.startsWith("remove: ID=\"gnome-shell/x/1-2-host_TIME3\"") &&
                all.indexOf('\0') == all.indexOf('"', 12) + 1,
            "the id is in quotes and the message ends with a NUL");
    require(QByteArray(bm::startupRemoveMessage("a\"b\\c").first()).contains("a\\\"b\\\\c"),
            "a quote or backslash in the id is escaped");
    bm::finishStartupSequence(); // nothing to end, and no X11 here: it must do nothing
}
static void testSingleInstance() {
    using bm::SingleInstance;
    const auto key = SingleInstance::keyFor("/tmp/some/node/folder");
    require(key.startsWith("ynotbit-") && key.size() <= 40 && !key.contains('/') &&
                !key.contains('\\'),
            "the socket's name is short and is not a path: macOS allows 104 bytes in all");
    require(key == SingleInstance::keyFor("/tmp/some/node/folder") &&
                key == SingleInstance::keyFor("/tmp/some/other/../node/folder/") &&
                key != SingleInstance::keyFor("/tmp/some/node/other"),
            "a folder has one name however it is spelled, and another folder another");
    // Each run its own name, so a run does not meet the last one's socket.
    const auto name = "ynotbit-test-" + QString::number(QCoreApplication::applicationPid());
    require(!SingleInstance::wake(name), "with no instance listening, there is no one to wake");
    {
        SingleInstance instance(name);
        require(instance.listening(), "an instance listens");
        QSignalSpy woken(&instance, &SingleInstance::woken);
        require(SingleInstance::wake(name), "a second launch finds the first");
        require(woken.wait(2000) && woken.count() == 1, "and the first is told to show its window");
        require(SingleInstance::wake(name) && woken.wait(2000) && woken.count() == 2,
                "as often as it asks");
        // Whatever else is said at the socket does not show the window.
        QLocalSocket other;
        other.connectToServer(name);
        require(other.waitForConnected(1000), "the socket takes a connection");
        other.write("hello\n");
        other.waitForBytesWritten(1000);
        QTest::qWait(200);
        require(woken.count() == 2, "only a request to show does");
    }
    require(!SingleInstance::wake(name), "a closed instance is not there to wake");
#ifndef Q_OS_WIN
    // A socket file left by a run that crashed: the next one takes its place, and a launch that
    // finds only the file knows there is no instance.
    const QString stale = QDir::tempPath() + "/" + name;
    require(QFile(stale).open(QIODevice::WriteOnly), "(a leftover file)");
    require(!SingleInstance::wake(name), "a leftover file is not an instance");
    SingleInstance instance(name);
    require(instance.listening(), "a leftover file does not keep the next instance from listening");
    QFile::remove(stale);
#endif
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        testGlyph();
        testTone();
        testMenu();
        testActive();
        testStartupRemove();
        testSingleInstance();
        std::cout << "PASS: glyph, tone, menu, tray availability, second launch\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
