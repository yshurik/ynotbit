#include "desktop_entry.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
namespace bm {
namespace {
constexpr auto kMarker = "X-Ynotbit-Generated=true";
constexpr int kIconSizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256, 512};
// A path as the Exec key takes it: in double quotes when it has anything a launcher would split
// on, with what is special inside quotes escaped; "%" starts a field code, so it is doubled.
QString execPath(QString path) {
    path.replace('%', "%%");
    if (!path.contains(QRegularExpression("[\\s\"'\\\\<>~|&;$*?#()`]")))
        return path;
    path.replace('\\', "\\\\\\\\").replace('"', "\\\\\"").replace('`', "\\\\`").replace('$', "\\\\$");
    return '"' + path + '"';
}
bool write(const QString &path, const QByteArray &bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
} // namespace
QString desktopEntryText(const QString &exec) {
    return QStringLiteral("[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=ynotbit\n"
                          "Exec=%1\n"
                          "Icon=ynotbit\n"
                          "Categories=Network;Chat;\n"
                          "StartupWMClass=ynotbit\n"
                          "%2\n")
        .arg(execPath(exec), kMarker);
}
bool ensureDesktopEntry(const QString &dataHome, const QStringList &systemDirs, const QString &exec) {
    for (const auto &dir : systemDirs)
        if (QFile::exists(dir + "/applications/ynotbit.desktop"))
            return false;
    const auto entry = dataHome + "/applications/ynotbit.desktop";
    QFile existing(entry);
    if (existing.exists() &&
        (!existing.open(QIODevice::ReadOnly) || !existing.readAll().contains(kMarker)))
        return false; // not ours to touch
    existing.close();
    bool wrote = false;
    const auto text = desktopEntryText(exec).toUtf8();
    if (!existing.exists() || (existing.open(QIODevice::ReadOnly) && existing.readAll() != text)) {
        existing.close();
        wrote = write(entry, text);
    }
    for (int size : kIconSizes) {
        QFile picture(QString(":/icons/application/%1.png").arg(size));
        const auto target =
            QString("%1/icons/hicolor/%2x%2/apps/ynotbit.png").arg(dataHome).arg(size);
        if (!picture.open(QIODevice::ReadOnly) || QFileInfo(target).size() == picture.size())
            continue;
        wrote = write(target, picture.readAll()) || wrote;
    }
    return wrote;
}
} // namespace bm
