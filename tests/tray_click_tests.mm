// A click on the menu-bar icon, on the real Cocoa platform and in the real menu bar (the tray test
// runs offscreen, which has no menu bar to click in). Qt's own handler of the click asks the
// app's last event for its click count, and macOS 27 hands the click over without a mouse event:
// the app aborted. The events here are put in the app's hands the way the event loop would.
// Needs a window server; without one it is skipped (the exit code 77 ctest reads as that).
#import <AppKit/AppKit.h>
#include "tray_icon.h"
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSignalSpy>
#include <QSystemTrayIcon>
#include <QTest>
#include <QTimer>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
constexpr int kSkipped = 77;
// The menu the click opened, as the system said it began tracking one.
NSMenu *gOpenMenu = nil;

void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
NSButton *buttonIn(NSView *view) {
    if ([view isKindOfClass:[NSButton class]])
        return (NSButton *)view;
    for (NSView *sub in view.subviews)
        if (NSButton *button = buttonIn(sub))
            return button;
    return nil;
}
// The icon's button in the menu bar: the one this process has.
NSButton *statusButton() {
    for (NSWindow *window in NSApp.windows)
        if ([NSStringFromClass([window class]) containsString:@"StatusBar"] && window.contentView)
            if (NSButton *button = buttonIn(window.contentView))
                return button;
    return nil;
}
// Makes `event` the last one the app took from its queue, which is what NSApp.currentEvent is.
void lastEventIs(NSEvent *event) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        [NSApp postEvent:event atStart:YES];
        QCoreApplication::processEvents();
        if (NSApp.currentEvent.type == event.type)
            return;
    }
    throw std::runtime_error("(the event could not be made the app's last)");
}
NSEvent *notAMouseEvent() {
    return [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                         modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:1
                                 data1:0 data2:0];
}
NSEvent *aMouseEvent() {
    return [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:NSZeroPoint modifierFlags:0
                             timestamp:0 windowNumber:0 context:nil eventNumber:0 clickCount:1
                              pressure:0];
}
// Clicks the icon as the menu bar does. A menu it opens is open for a moment, `inside` runs on it,
// and it is shut again. Gives the menu, or nil if the click opened none.
NSMenu *click(NSButton *button, const std::function<void(NSMenu *)> &inside = {}) {
    gOpenMenu = nil;
    id observer = [NSNotificationCenter.defaultCenter
        addObserverForName:NSMenuDidBeginTrackingNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification *notice) { gOpenMenu = notice.object; }];
    // The menu's own loop runs the timer; nothing here may throw into it.
    QTimer shut;
    shut.setSingleShot(true);
    QObject::connect(&shut, &QTimer::timeout, [&] {
        if (!gOpenMenu)
            return;
        if (inside)
            inside(gOpenMenu);
        [gOpenMenu cancelTracking];
    });
    shut.start(150);
    [button performClick:nil]; // comes back when the menu is shut
    [NSNotificationCenter.defaultCenter removeObserver:observer];
    return gOpenMenu;
}
} // namespace

int main(int argc, char **argv) {
    // No login session (a job with no desktop): there is no window server for Cocoa to start with.
    if (CFDictionaryRef session = CGSessionCopyCurrentDictionary()) {
        CFRelease(session);
    } else {
        std::cout << "SKIP: no login session, so no window server\n";
        return kSkipped;
    }
    QApplication app(argc, argv);
    if (QGuiApplication::platformName() != "cocoa") {
        std::cout << "SKIP: needs the Cocoa platform and its menu bar\n";
        return kSkipped;
    }
    // One scenario by name, to watch it alone; none for all of them.
    const std::string only = argc > 1 ? argv[1] : "";
    const auto wanted = [&](const char *name) { return only.empty() || only == name; };
    try {
        bm::TrayIcon tray({"Show ynotbit", "Hide ynotbit", "New letter", "New chat", "Settings…", "Quit"});
        tray.setMailboxOpen(true);
        NSButton *button = nil;
        for (int waited = 0; waited < 40 && !button; ++waited) {
            QTest::qWait(50);
            button = statusButton();
        }
        if (!button) {
            std::cout << "SKIP: no menu bar to put the icon in\n";
            return kSkipped;
        }
        // Does a click here open a menu at all? (Under a mouse event: nothing throws.)
        lastEventIs(aMouseEvent());
        if (!click(button)) {
            std::cout << "SKIP: the menu bar does not open menus here\n";
            return kSkipped;
        }
        QSignalSpy showRequested(&tray, &bm::TrayIcon::showRequested);

        // The click macOS 27 makes: the app's last event is not a mouse event.
        if (wanted("non-mouse")) {
            lastEventIs(notAMouseEvent());
            require(click(button) != nil, "a click opens the menu, whatever the app saw last");
            require(showRequested.count() == 0, "and it does not ask for the window");
        }
        // The click macOS made until then. Opening the menu is not asking for the window: that is
        // for Show, in the menu (Qt reports every click on the item, and the window would rise
        // each time the menu is opened).
        if (wanted("mouse")) {
            lastEventIs(aMouseEvent());
            require(click(button) != nil, "a click opens the menu");
            require(showRequested.count() == 0, "opening the menu does not ask for the window");
        }
        // Without a menu the click is the button's own action, which Qt answers the same way.
        if (wanted("no-menu")) {
            tray.icon()->setContextMenu(nullptr);
            lastEventIs(notAMouseEvent());
            require(click(button) == nil, "(with no menu, a click opens none)");
            require(showRequested.count() == 0, "a click on a bare icon does not abort or ask");
            tray.icon()->setContextMenu(tray.menu());
        }
        // And the item chosen in the menu opened by such a click does what it says.
        if (wanted("choose")) {
            QSignalSpy newLetter(&tray, &bm::TrayIcon::newLetterRequested);
            lastEventIs(notAMouseEvent());
            NSMenu *menu = click(button, [](NSMenu *open) {
                if (NSMenuItem *item = [open itemWithTitle:@"New letter"])
                    [open performActionForItemAtIndex:[open indexOfItem:item]];
            });
            require(menu != nil, "a click opens the menu");
            require([menu itemWithTitle:@"New letter"] != nil, "the menu has its items");
            require(newLetter.count() == 1, "an item chosen in the menu makes its request");
        }
        std::cout << "PASS: a click on the menu-bar icon opens its menu, whatever the app saw last\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
