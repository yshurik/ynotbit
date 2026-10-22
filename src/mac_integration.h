#pragma once
#include <functional>
// What only macOS has: the app asleep behind other windows, and a Dock icon to click.
namespace bm::mac {
#ifdef __APPLE__
// Brings the app in front of the others. A window shown from the notification area's menu is not
// in front until the app is.
void activateApp();
// Runs `handler` when the Dock icon is clicked, or the app opened again, while it runs: the
// window closed into the notification area is wanted back. Call it with the event loop running,
// as the system's own handler for this is set up when it starts, and this one takes its place.
void onReopen(std::function<void()> handler);
// Stops Qt from reporting a click on the menu-bar icon (QSystemTrayIcon::activated). Qt reports it
// by asking the app's last event for its click count, and macOS 27 gives the icon its clicks
// without a mouse event: the count is refused with an exception, and the app aborts (Qt 6.8 to
// 6.10; Qt's dev branch has the fix). The icon opens its menu on a click and nothing else is wanted
// of it. Call it where the icon is made; where Qt's handlers are not loaded (the offscreen platform
// of the tests) it does nothing.
void silenceStatusItemClicks();
#else
inline void activateApp() {}
inline void onReopen(std::function<void()>) {}
inline void silenceStatusItemClicks() {}
#endif
} // namespace bm::mac
