#pragma once
#include <QString>
#include <QStringList>
namespace bm {
// The launcher entry and icons a Linux desktop wants to name the app and draw its icon in the dock
// and the Activities view. A package or the AppImage's integration brings its own; a copy run from
// a build folder or an AppImage nobody integrated does not, and the shell then shows a generic icon.
// The entry text for a program started with `exec`.
QString desktopEntryText(const QString &exec);
// Writes ynotbit.desktop and the icons under `dataHome` (~/.local/share) when no entry is there
// already: not when one is installed in `systemDirs` (/usr/share, ...), and not over a file that
// is not ours (it has no X-Ynotbit-Generated line), which the user made or changed. An entry of
// ours is brought up to date, for the program having moved. Returns whether it wrote anything.
bool ensureDesktopEntry(const QString &dataHome, const QStringList &systemDirs, const QString &exec);
} // namespace bm
