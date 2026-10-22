#include "data_migration.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
static void require(bool ok, const char *what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        throw std::runtime_error(what);
    }
}
static void put(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write a file");
}
static QByteArray get(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<none>");
}
static void testFolder() {
    QTemporaryDir root;
    const auto old = root.path() + "/NotbitDesktop/Notbit Desktop", now = root.path() + "/ynotbit";
    put(old + "/node/keys.dat", "secret");
    require(bm::adoptLegacyFolder(old, now) == now && get(now + "/node/keys.dat") == "secret",
            "the old folder's contents are in the new one");
    require(!QFileInfo(old).exists() && !QFileInfo(root.path() + "/NotbitDesktop").exists(),
            "nothing of the old is left, not even its empty parent");
    require(bm::adoptLegacyFolder(old, now) == now, "with no old folder, the new one is used");
    put(old + "/node/keys.dat", "other");
    require(bm::adoptLegacyFolder(old, now) == now && get(now + "/node/keys.dat") == "secret" &&
                get(old + "/node/keys.dat") == "other",
            "a new folder already in use is not merged into or replaced");
    QDir(now).removeRecursively();
    QDir().mkpath(now);
    require(bm::adoptLegacyFolder(old, now) == now && get(now + "/node/keys.dat") == "other",
            "an empty new folder is taken over");
    QDir(now).removeRecursively();
    put(old + "/node/keys.dat", "kept");
    put(old + "/sound-themes/old.txt", "old");
    put(now + "/sound-themes/new.txt", "new");
    require(bm::adoptLegacyFolder(old, now) == now && get(now + "/node/keys.dat") == "kept" &&
                get(now + "/sound-themes/new.txt") == "new" && !QFileInfo(old + "/node").exists(),
            "a new folder with no node, only a theme, still gets the old node");
}
static void testSettings() {
    QCoreApplication::setOrganizationDomain("ynotbit-test");
    QCoreApplication::setApplicationName("ynotbit-test");
    {
        QSettings old("OldOrg", "Old App");
        old.setValue("listDensity", "compact");
        old.setValue("railCollapsed/chats", true);
    }
    require(bm::adoptLegacySettings("OldOrg", "Old App"), "old settings are found and copied");
    QSettings now;
    require(now.value("listDensity").toString() == "compact" &&
                now.value("railCollapsed/chats").toBool(),
            "the new settings hold the old values");
    require(QSettings("OldOrg", "Old App").allKeys().isEmpty(), "the old settings are cleared");
    {
        QSettings old("OldOrg", "Old App");
        old.setValue("listDensity", "cozy");
    }
    require(!bm::adoptLegacySettings("OldOrg", "Old App") &&
                QSettings().value("listDensity").toString() == "compact",
            "settings already there are not overwritten");
}
int main(int argc, char **argv) {
    // Settings go in a folder of their own, never the real one.
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    QCoreApplication app(argc, argv);
    try {
        testFolder();
        testSettings();
    } catch (const std::exception &) {
        return 1;
    }
    std::cout << "PASS: legacy folder and settings\n";
    return 0;
}
