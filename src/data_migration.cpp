#include "data_migration.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
namespace bm {
namespace {
// Removes `dir` if nothing is in it.
void dropIfEmpty(const QString &dir) {
    if (QDir(dir).isEmpty())
        QDir().rmdir(dir);
}
// The keys a QSettings holds of its own. On macOS it also lists the system-wide
// defaults, so those are taken off: they are never the app's to move or to count.
QStringList ownKeys(const QSettings &settings) {
    static const QStringList shared =
        QSettings("ynotbit-no-such-organization", "no-such-application").allKeys();
    auto keys = settings.allKeys();
    for (const auto &key : shared)
        keys.removeAll(key);
    return keys;
}
} // namespace
QString adoptLegacyFolder(const QString &oldDir, const QString &newDir) {
    if (QDir::cleanPath(oldDir) == QDir::cleanPath(newDir) || !QFileInfo(oldDir).isDir())
        return newDir;
    if (QFileInfo(newDir).exists()) {
        if (!QDir(newDir).isEmpty()) {
            // Something small made the new folder first (a sound theme, say). Only a new
            // folder with no node of its own gets the old entries, one by one, never over one.
            if (QFileInfo(newDir + "/node").exists() || !QFileInfo(oldDir + "/node").isDir())
                return newDir;
            for (const auto &name : QDir(oldDir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot))
                if (!QFileInfo::exists(newDir + "/" + name))
                    QDir().rename(oldDir + "/" + name, newDir + "/" + name);
            if (QFileInfo(oldDir + "/node").exists())
                return oldDir;
            dropIfEmpty(oldDir);
            dropIfEmpty(QFileInfo(oldDir).absolutePath());
            return newDir;
        }
        QDir().rmdir(newDir);
    }
    if (!QDir().mkpath(QFileInfo(newDir).absolutePath()) || !QDir().rename(oldDir, newDir))
        return oldDir;
    dropIfEmpty(QFileInfo(oldDir).absolutePath());
    return newDir;
}
bool adoptLegacySettings(const QString &oldOrganization, const QString &oldApplication) {
    QSettings old(oldOrganization, oldApplication);
    const auto oldKeys = ownKeys(old);
    if (oldKeys.isEmpty())
        return false;
    QSettings now;
    if (!ownKeys(now).isEmpty())
        return false;
    for (const auto &key : oldKeys)
        now.setValue(key, old.value(key));
    now.sync();
    if (now.status() != QSettings::NoError)
        return false;
    const auto file = old.fileName();
    old.clear();
    old.sync();
    if (QFileInfo(file).isFile()) { // a file; on Windows it is a registry key and this is not
        QFile::remove(file);
        dropIfEmpty(QFileInfo(file).absolutePath());
    }
    return true;
}
} // namespace bm
