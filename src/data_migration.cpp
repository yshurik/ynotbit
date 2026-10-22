#include "data_migration.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
namespace bm {
namespace {
// Removes `dir` if nothing is in it.
void dropIfEmpty(const QString &dir) {
    if (QDir(dir).isEmpty())
        QDir().rmdir(dir);
}
} // namespace
QString adoptLegacyFolder(const QString &oldDir, const QString &newDir) {
    if (QDir::cleanPath(oldDir) == QDir::cleanPath(newDir) || !QFileInfo(oldDir).isDir())
        return newDir;
    if (QFileInfo(newDir).exists()) {
        if (!QDir(newDir).isEmpty())
            return newDir; // already in use: the old one is not ours to merge in
        QDir().rmdir(newDir);
    }
    if (!QDir().mkpath(QFileInfo(newDir).absolutePath()) || !QDir().rename(oldDir, newDir))
        return oldDir;
    dropIfEmpty(QFileInfo(oldDir).absolutePath());
    return newDir;
}
bool adoptLegacySettings(const QString &oldOrganization, const QString &oldApplication) {
    QSettings old(oldOrganization, oldApplication);
    if (old.allKeys().isEmpty())
        return false;
    QSettings now;
    if (!now.allKeys().isEmpty())
        return false;
    for (const auto &key : old.allKeys())
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
