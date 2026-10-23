#pragma once
#include <QtGlobal>
#include <functional>
namespace bm {
// The number on the app's icon: in the Dock on macOS, a taskbar overlay on Windows, the launcher's
// count on Linux where the shell shows one. Qt does each (QGuiApplication::setBadgeNumber, since
// 6.5); this keeps what it is given down to what has changed, and the user's choice to have it.
class AppBadge {
  public:
    // The number the window has now. It is shown while the badge is on, and given to the system
    // only when what is shown changes.
    static void set(int count);
    // The number taken off (the app is ending); the next set() shows it again.
    static void clear();
    static bool enabled();
    // On or off, kept in the settings. Off, the number is taken off and stays off.
    static void setEnabled(bool on);
    // Where a number goes instead of the system, for tests.
    static void setSinkForTesting(std::function<void(qint64)> sink);
};
} // namespace bm
