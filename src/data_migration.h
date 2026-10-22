#pragma once
#include <QString>
namespace bm {
// The app's data used to live under the application identifier "NotbitDesktop/Notbit Desktop" and
// now lives under "ynotbit". These carry what an earlier version left to the new places.
//
// Moves the folder `oldDir` to `newDir` when `oldDir` exists and `newDir` holds nothing yet, and
// leaves no empty parent behind. Returns the folder to use: `newDir`, or `oldDir` when it could not
// be moved, as an app that cannot find its keys is worse than one that keeps its old folder.
QString adoptLegacyFolder(const QString &oldDir, const QString &newDir);
// Copies the settings stored under (oldOrganization, oldApplication) to the ones the application
// now has, when it has none yet, and clears the old ones. Returns whether it copied any.
bool adoptLegacySettings(const QString &oldOrganization, const QString &oldApplication);
} // namespace bm
